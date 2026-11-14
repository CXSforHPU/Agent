package com.agentconsole.device;

import org.springframework.stereotype.Service;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * 端侧上报的接收与转发。
 *
 * <p>板端把自己产生的结论（MQTT 告警、传感器越限）沿着已建立的 A2A WebSocket
 * 发过来，这里按设备逻辑地址认出是哪块板子，落库，再推给正在听 SSE 的网页。
 *
 * <p>归属按板子地址算，不按设备行算：同一块板子可能被几个账号各登记一次，
 * 网页打开哪一行都该看到同一份上报。
 */
@Service
public class DeviceEventService {

    private final DeviceEventRepository events;
    private final DeviceRepository devices;
    /** 每块板子一组订阅者，网页开着 SSE 时挂着。 */
    private final Map<String, Set<Subscription>> subscribers = new ConcurrentHashMap<>();

    public DeviceEventService(DeviceEventRepository events, DeviceRepository devices) {
        this.events = events;
        this.devices = devices;
    }

    /**
     * 收一条板端上报。
     *
     * @param token 回调地址里的令牌
     * @param kind  事件类型，空的按 report 算
     * @param text  事件正文
     * @return 落库后的事件
     * @throws IllegalArgumentException 令牌对不上任何设备
     */
    public void receiveAddress(String address,String kind,String text) {
        Device owner=devices.findAll().stream().filter(d -> address.equals(d.address()))
            .min(java.util.Comparator.comparingLong(Device::id)).orElse(null);
        if(owner==null) return;
        DeviceEvent event=events.append(owner.id(),kind,text);
        publish(address,event);
    }

    /** 取最近的若干条，新的在前。同一块板子的几行设备共享这份事件。 */
    public List<DeviceEvent> list(long deviceId, int limit) {
        return events.findByDevices(deviceIdsOf(deviceId), limit);
    }

    public Subscription subscribe(long deviceId) {
        Subscription subscription = new Subscription();
        subscribers.computeIfAbsent(addressOf(deviceId), key -> ConcurrentHashMap.newKeySet())
                .add(subscription);
        return subscription;
    }

    public void unsubscribe(long deviceId, Subscription subscription) {
        Set<Subscription> set = subscribers.get(addressOf(deviceId));
        if (set != null) {
            set.remove(subscription);
        }
        subscription.close();
    }

    private void publish(String address, DeviceEvent event) {
        Set<Subscription> set = subscribers.get(address);
        if (set == null) {
            return;
        }
        for (Subscription subscription : set) {
            subscription.offer(event);
        }
    }

    /** 设备行所在的板子地址。行被删掉了就退回到按 id 区分。 */
    private String addressOf(long deviceId) {
        for (Device device : devices.findAll()) {
            if (device.id() == deviceId) {
                return device.address();
            }
        }
        return "device:" + deviceId;
    }

    /** 同一块板子的所有设备行，事件挂在其中任意一行上都算同一条板子的。 */
    private List<Long> deviceIdsOf(long deviceId) {
        String address = addressOf(deviceId);
        List<Long> ids = new ArrayList<>();
        for (Device device : devices.findAll()) {
            if (address.equals(device.address())) {
                ids.add(device.id());
            }
        }
        if (ids.isEmpty()) {
            ids.add(deviceId);
        }
        return ids;
    }

    /**
     * 一条设备事件的订阅。网页开着 SSE 时一直挂着，事件从队列里取。
     */
    public static final class Subscription implements AutoCloseable {

        private final BlockingQueue<DeviceEvent> queue = new LinkedBlockingQueue<>();
        private volatile boolean closed;

        /** 等一条事件，超时返回 null。 */
        public DeviceEvent poll(long timeoutMillis) throws InterruptedException {
            return queue.poll(timeoutMillis, TimeUnit.MILLISECONDS);
        }

        void offer(DeviceEvent event) {
            if (!closed) {
                queue.offer(event);
            }
        }

        @Override
        public void close() {
            closed = true;
            queue.clear();
        }
    }
}
