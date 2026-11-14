package com.agentconsole.a2a;
import org.springframework.stereotype.Component;
import java.util.function.Consumer;
@Component
public class A2aClient {
    private final WebSocketDeviceService ws;
    public A2aClient(WebSocketDeviceService ws) { this.ws=ws; }
    public A2aModels.AgentCard discover(String address) throws Exception { return ws.discover(address); }
    public A2aModels.TaskResponse send(String address,String text) throws Exception { return ws.send(address,text); }
    public A2aModels.TaskResponse sendSubscribe(String address,String text,Consumer<A2aModels.TaskResponse> onEvent) throws Exception {
        A2aModels.TaskResponse result=send(address,text);
        if(onEvent!=null) onEvent.accept(result);
        return result;
    }
    /** 设备当前是否有实时 WebSocket 会话（状态列用，读取时算，不落库）。 */
    public boolean isConnected(String address) { return ws.isConnected(address); }
}
