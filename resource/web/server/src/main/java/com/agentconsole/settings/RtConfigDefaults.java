package com.agentconsole.settings;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Component;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * 工程根目录 rtconfig.h 的编译期配置。
 *
 * 网站侧 agent 的默认协议、地址、模型名、API Key 都以这里为准，
 * 环境变量和用户设置只在上面做覆盖。找不到 rtconfig.h 时全部留空，
 * 调用方回退到自己的内置默认值。
 */
@Component
public class RtConfigDefaults {

    public static final String PROTOCOL_OPENAI = "openai";
    public static final String PROTOCOL_ANTHROPIC = "anthropic";

    private static final Logger log = LoggerFactory.getLogger(RtConfigDefaults.class);

    private static final Pattern DEFINE = Pattern.compile("^\\s*#define\\s+(\\w+)\\s*(.*)$");

    private final Path source;
    private final Map<String, String> defines = new LinkedHashMap<>();

    public RtConfigDefaults(@Value("${agent.website-agent.rtconfig:}") String configuredPath) {
        this.source = locate(configuredPath);
        if (this.source != null) {
            parse(this.source);
        }
        if (defines.isEmpty()) {
            log.info("没找到 rtconfig.h，网站侧 agent 用环境变量和内置默认值");
        } else {
            log.info("读到 rtconfig.h：{}（{} 条宏）", source, defines.size());
        }
    }

    /** 编译进固件的协议名，openai 或 anthropic。 */
    public String protocol() {
        if (defines.containsKey("PKG_AGENT_API_PROTOCOL_ANTHROPIC")) {
            return PROTOCOL_ANTHROPIC;
        }
        if (defines.containsKey("PKG_AGENT_API_PROTOCOL_OPENAI")) {
            return PROTOCOL_OPENAI;
        }
        return "";
    }

    /** 当前协议对应的服务根地址，不含协议路径。 */
    public String baseUrl() {
        return baseUrlFor(protocol());
    }

    /**
     * 指定协议对应的服务根地址。
     *
     * @param protocol openai 或 anthropic，空串按 rtconfig.h 里选中的那个
     */
    public String baseUrlFor(String protocol) {
        if (PROTOCOL_ANTHROPIC.equals(protocol)) {
            return defines.getOrDefault("PKG_AGENT_API_BASE_URL_ANTHROPIC", "");
        }
        if (PROTOCOL_OPENAI.equals(protocol)) {
            return defines.getOrDefault("PKG_AGENT_API_BASE_URL_OPENAI", "");
        }
        return baseUrl();
    }

    public String model() {
        return defines.getOrDefault("PKG_AGENT_MODEL_NAME", "");
    }

    public String apiKey() {
        return defines.getOrDefault("PKG_AGENT_API_KEY", "");
    }

    public String anthropicVersion() {
        return defines.getOrDefault("PKG_AGENT_ANTHROPIC_VERSION", "");
    }

    /** rtconfig.h 里的最大输出 token 数，没配就是 0。 */
    public int maxTokens() {
        String value = defines.get("PKG_AGENT_ANTHROPIC_MAX_TOKENS");
        if (value == null || value.isBlank()) {
            return 0;
        }
        try {
            return Integer.parseInt(value.trim());
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    /** rtconfig.h 里读到了东西才为真。 */
    public boolean available() {
        return !defines.isEmpty();
    }

    /** 读到的 rtconfig.h 路径，读不到时为 null。 */
    public Path source() {
        return source;
    }

    /**
     * 找 rtconfig.h：先看显式配置的路径，没有就从工作目录往上找带 Agent 包的工程根。
     */
    private static Path locate(String configuredPath) {
        if (configuredPath != null && !configuredPath.isBlank()) {
            Path path = Paths.get(configuredPath.trim()).toAbsolutePath();
            if (Files.isRegularFile(path)) {
                return path;
            }
            log.warn("配置的 rtconfig.h 不存在：{}", path);
            return null;
        }

        Path dir = Paths.get(System.getProperty("user.dir", ".")).toAbsolutePath();
        for (int i = 0; i < 8 && dir != null; i++) {
            Path candidate = dir.resolve("rtconfig.h");
            if (Files.isRegularFile(candidate) && looksLikeAgentProject(candidate)) {
                return candidate;
            }
            dir = dir.getParent();
        }
        return null;
    }

    /** 只认启用了 Agent 包的 rtconfig.h，避免捡到别的工程的文件。 */
    private static boolean looksLikeAgentProject(Path candidate) {
        try {
            return Files.readAllLines(candidate, StandardCharsets.UTF_8).stream()
                    .anyMatch(line -> line.startsWith("#define PKG_USING_AGENT"));
        } catch (IOException e) {
            return false;
        }
    }

    private void parse(Path path) {
        List<String> lines;
        try {
            lines = Files.readAllLines(path, StandardCharsets.UTF_8);
        } catch (IOException e) {
            log.warn("读 rtconfig.h 失败：{}", e.toString());
            return;
        }

        for (String line : lines) {
            Matcher matcher = DEFINE.matcher(line);
            if (!matcher.matches()) {
                continue;
            }
            String name = matcher.group(1);
            if (name.startsWith("PKG_AGENT_")) {
                defines.put(name, literal(matcher.group(2)));
            }
        }
    }

    /** 把宏的值取出来：带引号的取引号里的，其余去掉行尾注释。 */
    private static String literal(String raw) {
        String value = raw.trim();
        if (value.startsWith("\"")) {
            int end = value.lastIndexOf('"');
            if (end > 0) {
                return value.substring(1, end);
            }
        }
        int comment = value.indexOf("//");
        if (comment >= 0) {
            value = value.substring(0, comment).trim();
        }
        return value;
    }
}
