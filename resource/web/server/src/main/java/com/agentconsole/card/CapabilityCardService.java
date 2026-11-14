package com.agentconsole.card;

import com.agentconsole.a2a.WebSocketDeviceService;
import com.agentconsole.device.Device;
import com.agentconsole.device.DeviceRepository;
import org.springframework.stereotype.Service;

import java.util.List;

/**
 * 能力卡业务：CRUD 与设备绑定。
 *
 * <p>绑定规则由这里保证：一台设备只能绑一张卡（device.card_id 单列），
 * 一张卡可以绑多台设备。设备已经绑了别的卡时，换卡直接拒绝，
 * 必须先解绑再绑，避免静默替换。
 */
@Service
public class CapabilityCardService {

    private final CapabilityCardRepository cards;
    private final DeviceRepository devices;
    private final WebSocketDeviceService ws;

    public CapabilityCardService(CapabilityCardRepository cards, DeviceRepository devices,
                                 WebSocketDeviceService ws) {
        this.cards = cards;
        this.devices = devices;
        this.ws = ws;
    }

    public List<CapabilityCard> list(long userId) {
        return cards.findByUser(userId);
    }

    public CapabilityCard get(long id, long userId) {
        return cards.findById(id, userId)
                .orElseThrow(() -> new IllegalArgumentException("能力卡不存在"));
    }

    public CapabilityCard create(long userId, String name, String description, String version,
                                 List<CapabilityCard.Skill> skills) {
        if (name == null || name.isBlank()) {
            throw new IllegalArgumentException("能力卡名字不能为空");
        }
        return cards.create(userId, name.trim(),
                description == null ? "" : description.trim(),
                version, skills);
    }

    public CapabilityCard update(long id, long userId, String name, String description,
                                 String version, List<CapabilityCard.Skill> skills) {
        if (name == null || name.isBlank()) {
            throw new IllegalArgumentException("能力卡名字不能为空");
        }
        if (!cards.update(id, userId, name.trim(),
                description == null ? "" : description.trim(), version, skills)) {
            throw new IllegalArgumentException("能力卡不存在");
        }
        return get(id, userId);
    }

    /** 删除卡：绑着它的设备先置空（与外键 SET NULL 一致），不会留下悬挂引用。 */
    public void delete(long id, long userId) {
        if (!cards.delete(id, userId)) {
            throw new IllegalArgumentException("能力卡不存在");
        }
    }

    /**
     * 把设备绑到这张卡。
     *
     * @throws IllegalArgumentException 设备不存在
     * @throws IllegalStateException    设备已绑着另一张卡（换卡必须先解绑），控制器映射成 409
     */
    public CapabilityCard bindDevice(long cardId, long userId, long deviceId) {
        get(cardId, userId);
        Device device = devices.findById(deviceId, userId)
                .orElseThrow(() -> new IllegalArgumentException("设备不存在"));
        if (device.cardId() != null && device.cardId() != cardId) {
            throw new IllegalStateException(
                    "设备已绑定能力卡「" + (device.cardName() == null ? "#" + device.cardId() : device.cardName())
                            + "」，请先解绑再绑定新的能力卡");
        }
        if (!devices.bindCard(deviceId, userId, cardId)) {
            throw new IllegalArgumentException("绑定失败，设备不存在");
        }
        // 在线板端立刻收到新卡；离线的下次 welcome 也会推
        ws.pushCard(device.address());
        return get(cardId, userId);
    }

    /** 解绑设备。设备不在绑定状态时按成功处理，方便前端重复点。 */
    public CapabilityCard unbindDevice(long cardId, long userId, long deviceId) {
        get(cardId, userId);
        Device device = devices.findById(deviceId, userId)
                .orElseThrow(() -> new IllegalArgumentException("设备不存在"));
        if (device.cardId() != null && device.cardId() == cardId) {
            devices.bindCard(deviceId, userId, null);
        }
        return get(cardId, userId);
    }
}
