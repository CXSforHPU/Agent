package com.agentconsole.ops;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Service;

import java.io.IOException;
import java.io.InputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.time.Duration;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeUnit;

/**
 * 宿主侧运维：装依赖、起停文件服务器与 MCP 网关、试 TTS 与 STT。
 * 文件服务器是本工程 resource/file_server 下那套 FastAPI 服务，
 * MCP 网关是它带的 mcp/mcp_gateway.py。
 */
@Service
public class OpsService {

    private static final Logger log = LoggerFactory.getLogger(OpsService.class);
    private static final com.fasterxml.jackson.databind.ObjectMapper mapper =
            new com.fasterxml.jackson.databind.ObjectMapper();
    /** 宿主级设置里存端口的那两条。 */
    private static final String KEY_FILE_SERVER_PORT = "fileServerPort";
    private static final String KEY_MCP_PORT = "mcpPort";

    private final Path fileServerDir;
    private final Path clientDir;
    private final Path runDir;
    private final String fileServerHost;
    private final int defaultPort;
    private final int defaultMcpPort;
    private final int ttsTimeoutSeconds;
    private final HttpClient http;
    private final OpsSettingRepository settings;
    private final Map<String, Job> jobs = new ConcurrentHashMap<>();

    private volatile Process fileServerProcess;
    /** 文件服务器当前的端口。运维页可以改，改完存库。 */
    private volatile int fileServerPort;
    /** MCP 网关当前的端口。运维页可以改，改完存库。 */
    private volatile int mcpPort;

    public OpsService(@Value("${agent.console.file-server-dir:../file_server}") String fileServerDir,
                      @Value("${agent.console.client-dir:../client}") String clientDir,
                      @Value("${agent.console.run-dir:../.run}") String runDir,
                      @Value("${agent.console.file-server-host:127.0.0.1}") String host,
                      @Value("${agent.console.file-server-port:8000}") int port,
                      @Value("${agent.console.mcp-port:8765}") int mcpPort,
                      @Value("${agent.console.tts-timeout-seconds:900}") int ttsTimeoutSeconds,
                      OpsSettingRepository settings) {
        this.fileServerDir = Paths.get(fileServerDir).toAbsolutePath().normalize();
        this.clientDir = Paths.get(clientDir).toAbsolutePath().normalize();
        this.runDir = Paths.get(runDir).toAbsolutePath().normalize();
        this.fileServerHost = host;
        this.defaultPort = port;
        this.defaultMcpPort = mcpPort;
        this.ttsTimeoutSeconds = ttsTimeoutSeconds;
        this.settings = settings;
        // 运维页存过的端口优先，没有就用配置文件里的默认值
        this.fileServerPort = settings.find(KEY_FILE_SERVER_PORT)
                .map(OpsService::parsePort)
                .filter(p -> p > 0)
                .orElse(port);
        this.mcpPort = settings.find(KEY_MCP_PORT)
                .map(OpsService::parsePort)
                .filter(p -> p > 0)
                .orElse(mcpPort);
        // 文件服务器是 uvicorn，只讲 HTTP/1.1。JDK 的 HttpClient 默认 HTTP/2，
        // 明文时会先发 Upgrade: h2c，uvicorn 认不出就把请求体丢了，
        // 回 422「Field required」。
        this.http = HttpClient.newBuilder()
                .version(HttpClient.Version.HTTP_1_1)
                .connectTimeout(Duration.ofSeconds(10))
                .build();
    }

    private static int parsePort(String raw) {
        try {
            return Integer.parseInt(raw.trim());
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    // ------------------------------------------------------------------ 状态

    /** 各组件当前状态。端口探活为准，不依赖是不是本进程起的。 */
    public Map<String, Object> status() {
        Map<String, Object> out = new LinkedHashMap<>();
        out.put("fileServer", portOpen(fileServerHost, fileServerPort));
        out.put("mcpGateway", portOpen(fileServerHost, mcpPort));
        out.put("venvReady", Files.isDirectory(fileServerDir.resolve(".venv")));
        out.put("ttsReady", Files.isDirectory(fileServerDir.resolve("third_party/gpt-sovits-infer")));
        out.put("fileServerUrl", baseUrl());
        out.put("fileServerPort", fileServerPort);
        out.put("mcpPort", mcpPort);
        out.put("mcpUrl", mcpUrl());
        out.put("managed", fileServerProcess != null && fileServerProcess.isAlive());
        return out;
    }

    /** 运维面板的宿主级设置。 */
    public Map<String, Object> settings() {
        Map<String, Object> out = new LinkedHashMap<>();
        out.put("fileServerPort", fileServerPort);
        out.put("defaultPort", defaultPort);
        out.put("mcpPort", mcpPort);
        out.put("defaultMcpPort", defaultMcpPort);
        return out;
    }

    /**
     * 改端口。fileServerPort 与 mcpPort 都可空，空的那项不动。
     *
     * <p>服务还在跑的时候不让改：换了端口以后旧进程没人管，会占着旧端口，
     * 而按端口反查 pid 的停止逻辑也只会查新端口，旧进程就漏了。
     */
    public Map<String, Object> updateSettings(Integer fileServerPort, Integer mcpPort) {
        if (fileServerPort == null && mcpPort == null) {
            throw new IllegalArgumentException("没给端口");
        }
        if (fileServerPort != null) {
            checkPort(fileServerPort);
            if (fileServerPort != this.fileServerPort && portOpen(fileServerHost, this.fileServerPort)) {
                throw new IllegalStateException("文件服务器还在跑，先点停止再改端口");
            }
        }
        if (mcpPort != null) {
            checkPort(mcpPort);
            if (mcpPort != this.mcpPort && portOpen(fileServerHost, this.mcpPort)) {
                throw new IllegalStateException("MCP 网关还在跑，先点停止再改端口");
            }
        }
        int newFileServerPort = fileServerPort == null ? this.fileServerPort : fileServerPort;
        int newMcpPort = mcpPort == null ? this.mcpPort : mcpPort;
        if (newFileServerPort == newMcpPort) {
            throw new IllegalArgumentException("文件服务器和 MCP 网关不能用同一个端口");
        }

        List<String> changed = new ArrayList<>();
        if (fileServerPort != null && fileServerPort != this.fileServerPort) {
            settings.put(KEY_FILE_SERVER_PORT, String.valueOf(fileServerPort));
            this.fileServerPort = fileServerPort;
            changed.add("文件服务器端口 " + fileServerPort);
        }
        if (mcpPort != null && mcpPort != this.mcpPort) {
            settings.put(KEY_MCP_PORT, String.valueOf(mcpPort));
            this.mcpPort = mcpPort;
            changed.add("MCP 网关端口 " + mcpPort);
        }

        Map<String, Object> out = new LinkedHashMap<>();
        out.put("fileServerPort", this.fileServerPort);
        out.put("mcpPort", this.mcpPort);
        out.put("message", changed.isEmpty()
                ? "端口没变"
                : String.join("、", changed) + "，下次启动生效");
        return out;
    }

    private static void checkPort(int port) {
        if (port < 1 || port > 65535) {
            throw new IllegalArgumentException("端口只能在 1~65535 之间");
        }
    }

    private String baseUrl() {
        return "http://" + fileServerHost + ":" + fileServerPort;
    }

    private String mcpUrl() {
        return "http://" + fileServerHost + ":" + mcpPort + "/mcp";
    }

    private boolean portOpen(String host, int port) {
        try (Socket socket = new Socket()) {
            socket.connect(new InetSocketAddress(host, port), 800);
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    // -------------------------------------------------------------- 装依赖

    /**
     * 装依赖，跑在后台。前端轮询 job 看进度。
     *
     * @param target web 表示前端 npm 依赖，file-server 表示文件服务器那套
     * @param withTts 文件服务器是否连带装 TTS 的 torch 与权重
     * @return 任务 id
     */
    public String install(String target, boolean withTts) {
        List<String> command;
        String label;
        if ("web".equals(target)) {
            label = "安装前端依赖";
            command = onWindows()
                    ? List.of("cmd.exe", "/c", "npm install --no-fund --no-audit")
                    : List.of("npm", "install", "--no-fund", "--no-audit");
        } else if ("file-server".equals(target)) {
            label = withTts ? "安装文件服务器依赖（含 TTS）" : "安装文件服务器依赖";
            command = deployCommand(Deploy.CHECK, withTts);
        } else {
            throw new IllegalArgumentException("不支持的目标：" + target);
        }

        Job job = new Job(UUID.randomUUID().toString(), label, command.toString());
        jobs.put(job.id, job);
        Thread thread = new Thread(() -> runJob(job, command, workingDir(target)), "ops-" + job.id);
        thread.setDaemon(true);
        thread.start();
        return job.id;
    }

    private Path workingDir(String target) {
        return "web".equals(target) ? clientDir : fileServerDir;
    }

    /** 部署脚本的三种用法。 */
    private enum Deploy {
        /** 装依赖，不启动服务。 */
        CHECK,
        /** 只启动服务，不碰依赖。 */
        START,
    }

    /** 文件服务器部署脚本的参数。Windows 用 bat，其余用 bash。 */
    private List<String> deployCommand(Deploy mode, boolean withTts) {
        List<String> cmd = new ArrayList<>();
        if (onWindows()) {
            cmd.add("cmd.exe");
            cmd.add("/c");
            cmd.add("deploy.bat");
        } else {
            cmd.add("bash");
            cmd.add("deploy.sh");
        }
        if (withTts) {
            cmd.add("--tts");
        }
        if (mode == Deploy.CHECK) {
            cmd.add("--check");
        } else if (mode == Deploy.START) {
            cmd.add("--start-only");
            // 端口跟着运维页设的值走，脚本会用它起文件服务器，
            // MCP 网关的 --file-server 地址也跟着变
            cmd.add("--port");
            cmd.add(String.valueOf(fileServerPort));
            cmd.add("--mcp-port");
            cmd.add(String.valueOf(mcpPort));
        }
        return cmd;
    }

    private void runJob(Job job, List<String> command, Path cwd) {
        job.status = "running";
        job.startedAt = System.currentTimeMillis();
        try {
            Files.createDirectories(runDir);
            Path logFile = runDir.resolve("install.log");
            job.logFile = logFile.toString();
            ProcessBuilder builder = new ProcessBuilder(command)
                    .directory(cwd.toFile())
                    .redirectErrorStream(true)
                    .redirectOutput(logFile.toFile());
            builder.environment().put("PYTHONIOENCODING", "utf-8");
            Process process = builder.start();
            job.pid = process.pid();
            boolean finished = process.waitFor(30, TimeUnit.MINUTES);
            if (!finished) {
                process.destroyForcibly();
                job.status = "failed";
                job.message = "超过 30 分钟未结束，已终止";
                return;
            }
            job.exitCode = process.exitValue();
            job.status = job.exitCode == 0 ? "done" : "failed";
            job.message = job.exitCode == 0 ? "完成" : "退出码 " + job.exitCode;
        } catch (IOException e) {
            job.status = "failed";
            job.message = "起不来：" + e.getMessage();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            job.status = "failed";
            job.message = "被中断";
        } finally {
            job.finishedAt = System.currentTimeMillis();
        }
    }

    public Job job(String id) {
        Job job = jobs.get(id);
        if (job == null) {
            throw new IllegalArgumentException("任务不存在");
        }
        return job;
    }

    public List<Job> jobs() {
        List<Job> out = new ArrayList<>(jobs.values());
        out.sort((a, b) -> Long.compare(b.startedAt, a.startedAt));
        return out;
    }

    // ------------------------------------------------------------ 起停服务

    /** 起文件服务器，连带起 MCP 网关。端口已经被占了就当已经在跑。 */
    public Map<String, Object> start() {
        if (portOpen(fileServerHost, fileServerPort)) {
            return Map.<String, Object>of("started", false, "message", "端口 " + fileServerPort + " 已经在监听");
        }
        try {
            Files.createDirectories(runDir);
            Path logFile = runDir.resolve("file-server.log");
            ProcessBuilder builder = new ProcessBuilder(deployCommand(Deploy.START, false))
                    .directory(fileServerDir.toFile())
                    .redirectErrorStream(true)
                    .redirectOutput(logFile.toFile());
            builder.environment().put("PYTHONIOENCODING", "utf-8");
            Process process = builder.start();
            fileServerProcess = process;
            // 等端口起来，最多 90 秒
            for (int i = 0; i < 90; i++) {
                if (portOpen(fileServerHost, fileServerPort)) {
                    return Map.<String, Object>of("started", true, "message", "文件服务器已在 " + baseUrl(),
                            "pid", process.pid());
                }
                if (!process.isAlive()) {
                    return Map.<String, Object>of("started", false, "message", "进程退出了，看日志 " + logFile);
                }
                sleep(1000);
            }
            return Map.<String, Object>of("started", false, "message", "90 秒没监听上，看日志 " + logFile);
        } catch (IOException e) {
            throw new IllegalStateException("起文件服务器失败：" + e.getMessage(), e);
        }
    }

    /** 停文件服务器，连带把 MCP 网关带走。 */
    public Map<String, Object> stop() {
        StringBuilder note = new StringBuilder();
        Process process = fileServerProcess;
        if (process != null && process.isAlive()) {
            killTree(process);
            fileServerProcess = null;
            note.append("停掉了本控制台起的进程");
        }

        // 后端重启过，或者服务是脚本起的，本实例手上没有进程。
        // 这时按端口反查 pid 再杀一次，不然停止按钮只能干看着。
        if (portOpen(fileServerHost, fileServerPort)) {
            Optional<Long> pid = pidOnPort(fileServerPort);
            if (pid.isPresent()) {
                killPid(pid.get());
                if (note.length() > 0) {
                    note.append('；');
                }
                note.append("按端口杀掉了 pid ").append(pid.get());
            }
        }

        killMcpGateway();

        // 等端口真正释放
        for (int i = 0; i < 20; i++) {
            if (!portOpen(fileServerHost, fileServerPort)) {
                break;
            }
            sleep(500);
        }
        boolean stillUp = portOpen(fileServerHost, fileServerPort);
        if (stillUp) {
            return Map.<String, Object>of("stopped", false,
                    "message", "端口还占着，手动查一下：" + baseUrl());
        }
        if (note.length() == 0) {
            note.append("已停止");
        }
        return Map.<String, Object>of("stopped", true, "message", note.toString());
    }

    /**
     * 找监听某端口的进程 id。
     *
     * <p>Windows 走 netstat，其余走 ss。
     */
    private Optional<Long> pidOnPort(int port) {
        try {
            List<String> command = onWindows()
                    ? List.of("netstat", "-ano")
                    : List.of("ss", "-lptnH", "sport", "=", ":" + port);
            Process process = new ProcessBuilder(command)
                    .redirectErrorStream(true)
                    .start();
            String output = new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
            process.waitFor(10, TimeUnit.SECONDS);
            for (String line : output.split("\\r?\\n")) {
                if (onWindows()) {
                    // netstat 列：协议 本地地址 外部地址 状态 pid
                    String[] parts = line.trim().split("\\s+");
                    if (parts.length < 5
                            || !"LISTENING".equalsIgnoreCase(parts[3])
                            || !parts[1].endsWith(":" + port)) {
                        continue;
                    }
                    return parsePid(parts[4]);
                }
                // ss 里端口在第 4 列，pid 在 users:(("python",pid=1234,fd=3))
                if (!line.contains(":" + port)) {
                    continue;
                }
                int at = line.indexOf("pid=");
                if (at < 0) {
                    continue;
                }
                int end = at + 4;
                while (end < line.length() && Character.isDigit(line.charAt(end))) {
                    end++;
                }
                return parsePid(line.substring(at + 4, end));
            }
        } catch (IOException e) {
            log.warn("查端口 {} 的进程失败：{}", port, e.getMessage());
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        return Optional.empty();
    }

    private static Optional<Long> parsePid(String raw) {
        try {
            return Optional.of(Long.parseLong(raw.trim()));
        } catch (NumberFormatException e) {
            return Optional.empty();
        }
    }

    /** 杀掉一个进程及其子进程。 */
    private void killPid(long pid) {
        try {
            if (onWindows()) {
                new ProcessBuilder("taskkill", "/PID", String.valueOf(pid), "/T", "/F")
                        .redirectErrorStream(true)
                        .start()
                        .waitFor(15, TimeUnit.SECONDS);
            } else {
                new ProcessBuilder("kill", String.valueOf(pid)).start().waitFor(10, TimeUnit.SECONDS);
            }
        } catch (IOException e) {
            log.warn("杀进程 {} 失败：{}", pid, e.getMessage());
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    /**
     * 连子进程一起杀。
     *
     * <p>Windows 上 Process.destroy() 只结束 cmd.exe，deploy 脚本起的 python 会留下来
     * 继续占着端口，所以走 taskkill /T 按进程树杀。
     */
    private void killTree(Process process) {
        if (!onWindows()) {
            process.destroy();
            return;
        }
        try {
            new ProcessBuilder("taskkill", "/PID", String.valueOf(process.pid()), "/T", "/F")
                    .redirectErrorStream(true)
                    .start()
                    .waitFor(15, TimeUnit.SECONDS);
        } catch (IOException e) {
            process.destroy();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            process.destroy();
        }
    }

    /**
     * 按 pid 文件停 MCP 网关。deploy 脚本把它挂在后台，不在文件服务器那棵进程树里。
     * pid 文件不在就按端口反查。
     */
    private void killMcpGateway() {
        Path pidFile = fileServerDir.resolve("mcp_gateway.pid");
        if (Files.exists(pidFile)) {
            try {
                String pid = Files.readString(pidFile, StandardCharsets.UTF_8).trim();
                if (!pid.isEmpty()) {
                    killPid(Long.parseLong(pid));
                }
            } catch (IOException e) {
                log.warn("读 MCP 网关 pid 文件失败：{}", e.getMessage());
            } catch (NumberFormatException e) {
                log.warn("MCP 网关 pid 文件内容不对：{}", e.getMessage());
            }
            return;
        }
        pidOnPort(mcpPort).ifPresent(this::killPid);
    }

    // ------------------------------------------------------------ TTS / STT

    /** 列出可用音色。文件服务器返回 {"code":0,"data":{...}}，这里取 data 部分。 */
    public Map<String, Object> speakers() {
        HttpRequest request = HttpRequest.newBuilder(URI.create(baseUrl() + "/tts/speakers"))
                .timeout(Duration.ofSeconds(20))
                .GET()
                .build();
        try {
            HttpResponse<String> response = http.send(request, HttpResponse.BodyHandlers.ofString());
            if (response.statusCode() / 100 != 2) {
                throw new IllegalStateException("文件服务器返回 HTTP " + response.statusCode());
            }
            var json = new com.fasterxml.jackson.databind.ObjectMapper().readTree(response.body());
            var data = json.path("data");
            Map<String, Object> out = new LinkedHashMap<>();
            out.put("speakers", data.path("speakers").isArray()
                    ? mapper.convertValue(data.path("speakers"), new com.fasterxml.jackson.core.type.TypeReference<List<String>>() {})
                    : List.of());
            out.put("default", data.path("default").asText(""));
            out.put("languages", data.path("languages").isArray()
                    ? mapper.convertValue(data.path("languages"), new com.fasterxml.jackson.core.type.TypeReference<List<String>>() {})
                    : List.of());
            return out;
        } catch (IOException e) {
            throw new IllegalStateException("连不上文件服务器：" + e.getMessage(), e);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("被中断");
        }
    }

    /**
     * 读合成进度。文件服务器那边自己记着跑到第几句了。
     */
    public Map<String, Object> ttsProgress() {
        HttpRequest request = HttpRequest.newBuilder(URI.create(baseUrl() + "/tts/progress"))
                .timeout(Duration.ofSeconds(10))
                .GET()
                .build();
        try {
            HttpResponse<String> response = http.send(request, HttpResponse.BodyHandlers.ofString());
            if (response.statusCode() == 404) {
                throw new IllegalStateException("文件服务器没有进度接口，重启一下文件服务器");
            }
            if (response.statusCode() / 100 != 2) {
                throw new IllegalStateException("文件服务器返回 HTTP " + response.statusCode());
            }
            var data = mapper.readTree(response.body()).path("data");
            Map<String, Object> out = new LinkedHashMap<>();
            out.put("stage", data.path("stage").asText("idle"));
            out.put("index", data.path("index").asInt(0));
            out.put("total", data.path("total").asInt(0));
            out.put("audioSeconds", data.path("audioSeconds").asDouble(0));
            out.put("elapsed", data.path("elapsed").asDouble(0));
            out.put("message", data.path("message").asText(""));
            return out;
        } catch (IOException e) {
            throw new IllegalStateException("连不上文件服务器：" + e.getMessage(), e);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("被中断");
        }
    }

    /**
     * 合成一段语音。
     *
     * @return WAV 字节
     */
    public byte[] tts(String text, String speaker) {
        if (text == null || text.isBlank()) {
            throw new IllegalArgumentException("文本不能为空");
        }
        String body = "{\"text\":" + jsonString(text)
                + (speaker == null || speaker.isBlank() ? "" : ",\"speaker\":" + jsonString(speaker))
                + "}";
        HttpRequest request = HttpRequest.newBuilder(URI.create(baseUrl() + "/tts"))
                // 冷启动要装权重、编译 CUDA graph，实测整机要跑十分钟上下，
                // 超时太短会中途掐断，运维页只剩一个 502。
                .timeout(Duration.ofSeconds(ttsTimeoutSeconds))
                .header("Content-Type", "application/json")
                .POST(HttpRequest.BodyPublishers.ofString(body, StandardCharsets.UTF_8))
                .build();
        try {
            HttpResponse<byte[]> response = http.send(request, HttpResponse.BodyHandlers.ofByteArray());
            if (response.statusCode() / 100 != 2) {
                throw new IllegalStateException("文件服务器返回 HTTP " + response.statusCode()
                        + "：" + new String(response.body(), StandardCharsets.UTF_8));
            }
            return response.body();
        } catch (IOException e) {
            throw new IllegalStateException("调 TTS 失败：" + e.getMessage(), e);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("被中断");
        }
    }

    /**
     * 转写一段语音。
     *
     * @param audio    WAV 字节
     * @param filename 文件名，给文件服务器判断类型用
     * @return 识别出的文本
     */
    public String stt(byte[] audio, String filename) {
        if (audio == null || audio.length == 0) {
            throw new IllegalArgumentException("音频为空");
        }
        String boundary = "----agentconsole" + UUID.randomUUID().toString().replace("-", "");
        StringBuilder head = new StringBuilder();
        head.append("--").append(boundary).append("\r\n");
        head.append("Content-Disposition: form-data; name=\"file\"; filename=\"")
                .append(filename == null || filename.isBlank() ? "clip.wav" : filename)
                .append("\"\r\n");
        head.append("Content-Type: audio/wav\r\n\r\n");

        byte[] headBytes = head.toString().getBytes(StandardCharsets.UTF_8);
        byte[] tailBytes = ("\r\n--" + boundary + "--\r\n").getBytes(StandardCharsets.UTF_8);
        byte[] payload = new byte[headBytes.length + audio.length + tailBytes.length];
        System.arraycopy(headBytes, 0, payload, 0, headBytes.length);
        System.arraycopy(audio, 0, payload, headBytes.length, audio.length);
        System.arraycopy(tailBytes, 0, payload, headBytes.length + audio.length, tailBytes.length);

        HttpRequest request = HttpRequest.newBuilder(URI.create(baseUrl() + "/stt"))
                .timeout(Duration.ofSeconds(300))
                .header("Content-Type", "multipart/form-data; boundary=" + boundary)
                .POST(HttpRequest.BodyPublishers.ofByteArray(payload))
                .build();
        try {
            HttpResponse<String> response = http.send(request, HttpResponse.BodyHandlers.ofString());
            if (response.statusCode() / 100 != 2) {
                throw new IllegalStateException("文件服务器返回 HTTP " + response.statusCode()
                        + "：" + response.body());
            }
            // 响应形如 {"code":0,"data":{"text":"..."}}
            var json = mapper.readTree(response.body());
            var text = json.path("data").path("text").asText("");
            return text;
        } catch (IOException e) {
            throw new IllegalStateException("调 STT 失败：" + e.getMessage(), e);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("被中断");
        }
    }

    /** 读日志尾部。 */
    public List<String> tail(String name, int lines) {
        Path file = switch (name == null ? "" : name) {
            case "file-server" -> runDir.resolve("file-server.log");
            case "install" -> runDir.resolve("install.log");
            default -> null;
        };
        if (file == null || !Files.exists(file)) {
            return List.of();
        }
        try {
            // 日志里混着编码：文件服务器走 UTF-8，GPT-SoVITS 那些库往 stdout 写的
            // 是 UTF-16LE。按 UTF-8 硬解会抛 MalformedInputException，日志就整个读不出来，
            // 所以这里把坏字节替换掉，保证能读出大部分内容。
            String text = StandardCharsets.UTF_8.newDecoder()
                    .onMalformedInput(java.nio.charset.CodingErrorAction.REPLACE)
                    .onUnmappableCharacter(java.nio.charset.CodingErrorAction.REPLACE)
                    .decode(java.nio.ByteBuffer.wrap(Files.readAllBytes(file)))
                    .toString();
            List<String> all = text.lines().toList();
            int from = Math.max(0, all.size() - lines);
            return new ArrayList<>(all.subList(from, all.size()));
        } catch (IOException e) {
            log.warn("读日志 {} 失败：{}", file, e.getMessage());
            return List.of();
        }
    }

    private static String jsonString(String value) {
        StringBuilder sb = new StringBuilder("\"");
        for (char c : value.toCharArray()) {
            switch (c) {
                case '"' -> sb.append("\\\"");
                case '\\' -> sb.append("\\\\");
                case '\n' -> sb.append("\\n");
                case '\r' -> sb.append("\\r");
                case '\t' -> sb.append("\\t");
                default -> {
                    if (c < 0x20) {
                        sb.append(String.format("\\u%04x", (int) c));
                    } else {
                        sb.append(c);
                    }
                }
            }
        }
        return sb.append('"').toString();
    }

    private static boolean onWindows() {
        return System.getProperty("os.name", "").toLowerCase().contains("win");
    }

    private static void sleep(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    /** 一个后台任务的进度。 */
    public static class Job {
        public final String id;
        public final String label;
        public final String command;
        public volatile String status = "pending";
        public volatile String message = "";
        public volatile long pid = 0;
        public volatile Integer exitCode;
        public volatile long startedAt = System.currentTimeMillis();
        public volatile long finishedAt;
        public volatile String logFile;

        Job(String id, String label, String command) {
            this.id = id;
            this.label = label;
            this.command = command;
        }

        public Map<String, Object> view() {
            Map<String, Object> map = new LinkedHashMap<>();
            map.put("id", id);
            map.put("label", label);
            map.put("command", command);
            map.put("status", status);
            map.put("message", message);
            map.put("pid", pid);
            map.put("exitCode", exitCode);
            map.put("startedAt", startedAt);
            map.put("finishedAt", finishedAt);
            map.put("logFile", logFile);
            return map;
        }
    }
}
