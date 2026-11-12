package com.agentconsole.agent;

import com.agentconsole.a2a.A2aClient;
import com.agentconsole.a2a.A2aModels;
import com.agentconsole.device.Device;
import com.agentconsole.device.DeviceRepository;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import org.springframework.stereotype.Service;

import java.util.List;

/**
 * 中继工具：网站侧 agent 通过它向端侧 agent 提问。
 *
 * 端侧 agent 跑在设备上，能读传感器、下发指令、连 MQTT 和 MCP；
 * 网站侧 agent 只是控制台里的助手，问不到设备内部的东西，需要转一手。
 */
@Service
public class DeviceTool {

    public static final String NAME = "ask_device";

    private static final String DESCRIPTION =
            "向端侧 agent 提问。端侧 agent 跑在设备上，能读传感器、下发指令、"
                    + "用 MQTT 和 MCP 工具。问设备当前状态、传感器读数，或者让设备做某个动作时用它。"
                    + "返回的是端侧 agent 的原话。";

    private static final String PARAMETERS = """
            {
              "type": "object",
              "properties": {
                "device": {
                  "type": "string",
                  "description": "设备名，必须是上下文设备列表里的名字"
                },
                "question": {
                  "type": "string",
                  "description": "要问端侧 agent 的问题，说清楚要什么读数或要它做什么"
                }
              },
              "required": ["device", "question"]
            }
            """;

    private final A2aClient a2a;
    private final DeviceRepository devices;
    private final ObjectMapper json;

    public DeviceTool(A2aClient a2a, DeviceRepository devices, ObjectMapper json) {
        this.a2a = a2a;
        this.devices = devices;
        this.json = json;
    }

    /** 工具定义，给模型看。 */
    public LlmClient.ToolSpec spec() {
        return new LlmClient.ToolSpec(NAME, DESCRIPTION, PARAMETERS);
    }

    /**
     * 执行一次提问，把端侧 agent 的回答原样回给模型。
     *
     * @param userId 当前用户，只能用它自己的设备
     * @param call   模型发起的调用
     * @return 回给模型的文本，出错也是一段说明
     */
    public String invoke(long userId, LlmClient.ToolCall call) {
        String deviceName;
        String question;
        try {
            String raw = call.arguments();
            JsonNode args = json.readTree(raw == null || raw.isBlank() ? "{}" : raw);
            deviceName = args.path("device").asText("");
            question = args.path("question").asText("");
        } catch (Exception e) {
            return "工具参数不是合法 JSON：" + e.getMessage();
        }

        if (deviceName.isBlank() || question.isBlank()) {
            return "工具参数不完整，需要 device 和 question 两个字段。";
        }

        Device device = find(userId, deviceName);
        if (device == null) {
            return "没有找到名为「" + deviceName + "」的设备。可用设备见上下文里的设备列表。";
        }

        try {
            A2aModels.TaskResponse response = a2a.send(device.address(), question);
            String text = response.finalText();
            if (text == null || text.isBlank()) {
                String state = response.status() == null ? "unknown" : response.status().state();
                return "端侧 agent 没有返回内容（状态：" + state + "）。";
            }
            return text;
        } catch (Exception e) {
            return "调用端侧 agent 失败：" + e.getMessage();
        }
    }

    /** 按名字找设备，找不到再按地址找，都没有就当作没这个设备。 */
    private Device find(long userId, String name) {
        List<Device> list = devices.findByUser(userId);
        for (Device device : list) {
            if (device.name().equalsIgnoreCase(name) || device.address().equalsIgnoreCase(name)) {
                return device;
            }
        }
        return null;
    }
}
