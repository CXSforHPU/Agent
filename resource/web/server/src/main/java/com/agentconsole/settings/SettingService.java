package com.agentconsole.settings;

import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Service;

import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;

/**
 * 用户级设置。key 走白名单，value 存原文，不落版本库（SQLite 文件已 gitignore）。
 *
 * 取值优先级：用户填的 → 环境变量 → 工程 rtconfig.h（编译进固件的那份）→ 空。
 */
@Service
public class SettingService {

    /** key -> 展示名。顺序就是设置页的顺序。 */
    public static final Map<String, String> KEYS = new LinkedHashMap<>();

    static {
        KEYS.put("llm.protocol", "采用的协议");
        KEYS.put("llm.baseUrl", "协议地址");
        KEYS.put("llm.apiKey", "API Key");
        KEYS.put("llm.model", "模型名称");
        KEYS.put("agent.systemPrompt", "系统提示词");
    }

    private final SettingRepository repo;
    private final RtConfigDefaults rtConfig;
    /** 环境变量兜底。 */
    private final Map<String, String> envDefaults = new LinkedHashMap<>();

    public SettingService(SettingRepository repo,
                          RtConfigDefaults rtConfig,
                          @Value("${agent.website-agent.api-key:}") String apiKey,
                          @Value("${agent.website-agent.model:}") String model,
                          @Value("${agent.website-agent.base-url:}") String baseUrl,
                          @Value("${agent.website-agent.protocol:}") String protocol,
                          @Value("${agent.website-agent.system-prompt:}") String prompt) {
        this.repo = repo;
        this.rtConfig = rtConfig;
        envDefaults.put("llm.protocol", protocol == null ? "" : protocol);
        envDefaults.put("llm.baseUrl", baseUrl == null ? "" : baseUrl);
        envDefaults.put("llm.apiKey", apiKey == null ? "" : apiKey);
        envDefaults.put("llm.model", model == null ? "" : model);
        envDefaults.put("agent.systemPrompt", prompt == null ? "" : prompt);
    }

    /** 合并后的完整设置，用户填过的项覆盖默认值。 */
    public Map<String, String> load(long userId) {
        return resolve(repo.findAll(userId));
    }

    /** 读单个设置，用在网站侧 agent 取配置时。 */
    public String get(long userId, String key) {
        return load(userId).getOrDefault(key, "");
    }

    /**
     * rtconfig.h 给出的默认值，设置页拿它显示「默认地址」「默认模型名称」。
     * 地址按当前生效的协议取，用户改了协议时默认地址也跟着换。
     */
    public Map<String, String> rtDefaults(long userId) {
        String protocol = resolveProtocol(repo.findAll(userId));
        Map<String, String> result = new LinkedHashMap<>();
        result.put("llm.protocol", rtConfig.protocol());
        result.put("llm.baseUrl", rtConfig.baseUrlFor(protocol));
        result.put("llm.apiKey", maskKey(rtConfig.apiKey()));
        result.put("llm.model", rtConfig.model());
        return result;
    }

    /** 只接受白名单里的 key，值做长度限制，避免塞爆 SQLite。 */
    public void save(long userId, Map<String, String> input) {
        for (Map.Entry<String, String> e : input.entrySet()) {
            String key = e.getKey();
            if (!KEYS.containsKey(key)) {
                throw new IllegalArgumentException("不支持的设置项：" + key);
            }
            String value = e.getValue() == null ? "" : e.getValue();
            if (value.length() > 8192) {
                throw new IllegalArgumentException(key + " 超过 8192 字符");
            }
            if ("llm.protocol".equals(key)) {
                value = requireProtocol(value);
            }
            repo.put(userId, key, value);
        }
    }

    /** 恢复某一项到环境变量或 rtconfig.h 的默认值。 */
    public void reset(long userId, String key) {
        if (!KEYS.containsKey(key)) {
            throw new IllegalArgumentException("不支持的设置项：" + key);
        }
        repo.delete(userId, key);
    }

    /** 按优先级算出每一项的生效值。 */
    private Map<String, String> resolve(Map<String, String> stored) {
        String protocol = resolveProtocol(stored);
        Map<String, String> merged = new LinkedHashMap<>();
        for (String key : KEYS.keySet()) {
            merged.put(key, switch (key) {
                case "llm.protocol" -> protocol;
                case "llm.baseUrl" -> firstNonBlank(stored.get(key), envDefaults.get(key),
                        rtConfig.baseUrlFor(protocol));
                case "llm.apiKey" -> firstNonBlank(stored.get(key), envDefaults.get(key),
                        rtConfig.apiKey());
                case "llm.model" -> firstNonBlank(stored.get(key), envDefaults.get(key),
                        rtConfig.model());
                default -> firstNonBlank(stored.get(key), envDefaults.get(key));
            });
        }
        return merged;
    }

    /** 生效的协议：用户填的优先，其次环境变量，最后 rtconfig.h 里选中的那个。 */
    private String resolveProtocol(Map<String, String> stored) {
        String value = firstNonBlank(stored.get("llm.protocol"), envDefaults.get("llm.protocol"));
        if (!value.isEmpty()) {
            return normalizeProtocol(value);
        }
        return rtConfig.protocol();
    }

    /** 把协议名统一成 openai / anthropic，认不出来就当没填。 */
    public static String normalizeProtocol(String raw) {
        String value = raw == null ? "" : raw.trim().toLowerCase(Locale.ROOT);
        return switch (value) {
            case "openai", "open_ai", "open-ai" -> RtConfigDefaults.PROTOCOL_OPENAI;
            case "anthropic", "claude" -> RtConfigDefaults.PROTOCOL_ANTHROPIC;
            default -> "";
        };
    }

    /** 保存时校验协议名，只允许两个值，空串表示用默认。 */
    private static String requireProtocol(String raw) {
        String value = raw == null ? "" : raw.trim();
        if (value.isEmpty()) {
            return "";
        }
        String normalized = normalizeProtocol(value);
        if (normalized.isEmpty()) {
            throw new IllegalArgumentException("不支持的协议：" + raw + "，可选 openai 或 anthropic");
        }
        return normalized;
    }

    private static String firstNonBlank(String... values) {
        for (String value : values) {
            if (value != null && !value.isBlank()) {
                return value;
            }
        }
        return "";
    }

    /** 密钥只露个尾巴，够认出是不是同一个 Key 就行。 */
    private static String maskKey(String key) {
        if (key == null || key.isEmpty()) {
            return "";
        }
        if (key.length() <= 4) {
            return "****";
        }
        return "****" + key.substring(key.length() - 4);
    }
}
