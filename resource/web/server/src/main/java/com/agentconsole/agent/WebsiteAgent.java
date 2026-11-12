package com.agentconsole.agent;

import com.agentconsole.conversation.Message;
import com.agentconsole.device.Device;
import com.agentconsole.device.DeviceRepository;
import com.agentconsole.settings.RtConfigDefaults;
import com.agentconsole.settings.SettingService;
import com.fasterxml.jackson.databind.ObjectMapper;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Service;

import java.io.IOException;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * 网站侧 agent。协议、地址、模型名、API Key 默认取工程 rtconfig.h（编译进固件的那份），
 * 用户在设置页填的值优先。端侧 agent 不走这里，走 A2A；网站侧 agent 需要问设备时用 ask_device 转发。
 */
@Service
public class WebsiteAgent {

    /** 工具调用最多来回几轮，防止模型绕圈。 */
    private static final int MAX_TOOL_ROUNDS = 4;

    private static final String DEFAULT_PROMPT =
            "你是设备管理控制台的助手。回答要简短、具体，用中文。"
                    + "当用户问到具体设备时，先看上下文里给出的设备信息；信息不足就直接说明缺什么，不要编造。"
                    + "需要设备当前状态、传感器读数，或者要设备执行某个动作时，"
                    + "用 ask_device 工具去问端侧 agent，再根据它的回答作答，不要自己编设备数据。";

    private final SettingService settings;
    private final DeviceTool deviceTool;
    private final DeviceRepository devices;
    private final RtConfigDefaults rtConfig;
    private final ObjectMapper json;
    private final int maxTokens;
    private final long timeoutSeconds;

    public WebsiteAgent(SettingService settings, DeviceTool deviceTool,
                        DeviceRepository devices, RtConfigDefaults rtConfig, ObjectMapper json,
                        @Value("${agent.website-agent.max-tokens:0}") int maxTokens,
                        @Value("${agent.website-agent.timeout-seconds:60}") long timeoutSeconds) {
        this.settings = settings;
        this.deviceTool = deviceTool;
        this.devices = devices;
        this.rtConfig = rtConfig;
        this.json = json;
        this.maxTokens = maxTokens;
        this.timeoutSeconds = timeoutSeconds;
    }

    /** 配置齐了才有网站侧 agent。 */
    public boolean isConfigured() {
        return !settings.load(0).getOrDefault("llm.apiKey", "").isBlank();
    }

    /**
     * 带历史提问一次。
     *
     * @param userId   当前用户，按他存的设置取配置
     * @param history  本会话已有的消息，按时间升序
     * @param userText 本轮用户输入
     * @return 模型回答
     */
    public String reply(long userId, List<Message> history, String userText) throws IOException {
        Map<String, String> config = settings.load(userId);

        String apiKey = config.getOrDefault("llm.apiKey", "");
        if (apiKey.isBlank()) {
            throw new IllegalStateException(
                    "还没配置 API Key，去设置页填，或检查 rtconfig.h 里的 PKG_AGENT_API_KEY");
        }
        String model = config.getOrDefault("llm.model", "");
        String baseUrl = config.getOrDefault("llm.baseUrl", "");
        String protocol = config.getOrDefault("llm.protocol", "openai");
        String prompt = config.getOrDefault("agent.systemPrompt", "");

        LlmClient client = createClient(protocol, apiKey, model, baseUrl);

        List<LlmClient.Turn> turns = new ArrayList<>();
        if (history != null) {
            for (Message m : history) {
                if (m == null || m.content() == null || m.content().isBlank()) {
                    continue;
                }
                if ("user".equals(m.role())) {
                    turns.add(LlmClient.Turn.user(m.content()));
                } else {
                    turns.add(LlmClient.Turn.assistant(m.content(), List.of()));
                }
            }
        }
        turns.add(LlmClient.Turn.user(userText));

        String system = (prompt.isBlank() ? DEFAULT_PROMPT : prompt) + deviceBrief(userId);
        List<LlmClient.ToolSpec> tools = List.of(deviceTool.spec());

        for (int round = 0; round < MAX_TOOL_ROUNDS; round++) {
            LlmClient.Reply reply = client.complete(system, turns, tools);
            if (reply.toolCalls().isEmpty()) {
                return reply.text();
            }
            turns.add(LlmClient.Turn.assistant(reply.text(), reply.toolCalls()));
            for (LlmClient.ToolCall call : reply.toolCalls()) {
                turns.add(LlmClient.Turn.tool(deviceTool.invoke(userId, call), call.id()));
            }
        }
        return "（工具调用轮数用完，没有拿到最终回答）";
    }

    /** 按协议选客户端。 */
    private LlmClient createClient(String protocol, String apiKey, String model, String baseUrl) {
        if (baseUrl.isBlank()) {
            throw new IllegalStateException("没配置协议地址，去设置页填，"
                    + "或检查 rtconfig.h 里的 PKG_AGENT_API_BASE_URL_OPENAI / _ANTHROPIC");
        }
        if (model.isBlank()) {
            throw new IllegalStateException("没配置模型名，去设置页填，"
                    + "或检查 rtconfig.h 里的 PKG_AGENT_MODEL_NAME");
        }
        int limit = maxTokens > 0 ? maxTokens : Math.max(rtConfig.maxTokens(), 2048);
        Duration timeout = Duration.ofSeconds(timeoutSeconds);
        if (RtConfigDefaults.PROTOCOL_ANTHROPIC.equals(protocol)) {
            String version = rtConfig.anthropicVersion();
            return new AnthropicLlmClient(json, baseUrl, apiKey, model,
                    version.isBlank() ? "2023-06-01" : version, limit, timeout);
        }
        return new OpenAiLlmClient(json, baseUrl, apiKey, model, limit, timeout);
    }

    /** 把这个账号的设备列进系统提示词，模型才知道能对哪台设备提问。 */
    private String deviceBrief(long userId) {
        List<Device> list = devices.findByUser(userId);
        if (list.isEmpty()) {
            return "\n\n当前账号还没有登记设备，ask_device 暂时用不了。\n";
        }
        StringBuilder sb = new StringBuilder("\n\n可用设备：\n");
        for (Device device : list) {
            sb.append("- ").append(device.name())
                    .append("（地址 ").append(device.address())
                    .append(device.online() ? "，在线" : "，离线")
                    .append("）\n");
        }
        return sb.toString();
    }
}
