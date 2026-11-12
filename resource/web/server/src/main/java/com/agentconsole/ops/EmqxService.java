package com.agentconsole.ops;

import org.springframework.stereotype.Service;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.Map;
import java.util.Locale;
import java.util.concurrent.TimeUnit;

@Service
public class EmqxService {
    static final String IMAGE = "emqx/emqx-enterprise:6.3.2";
    private static final String NAME = "agent-console-emqx";

    private record Result(int code, String output) {}

    private Result command(String... args) {
        Path output = null;
        Process process = null;
        try {
            output = Files.createTempFile("agent-emqx-", ".log");
            if ("docker".equals(args[0])) args[0] = dockerExecutable();
            process = new ProcessBuilder(args).redirectErrorStream(true).redirectOutput(output.toFile()).start();
            if (!process.waitFor(15, TimeUnit.SECONDS)) {
                process.destroyForcibly();
                throw new IllegalStateException("Docker 命令超时，请检查运行环境");
            }
            return new Result(process.exitValue(), Files.readString(output).trim());
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("操作已中断", e);
        } catch (java.io.IOException e) {
            throw new IllegalStateException("未找到 Docker，请安装并启动对应平台的 Docker 环境");
        } finally {
            if (process != null && process.isAlive()) process.destroyForcibly();
            if (output != null) try { Files.deleteIfExists(output); } catch (java.io.IOException ignored) {}
        }
    }

    public Map<String, Object> status() {
        String os = System.getProperty("os.name");
        String platform = os.toLowerCase(Locale.ROOT);
        String method = platform.contains("win") || platform.contains("mac")
                ? "Docker Desktop（Linux 容器）" : "Docker Engine（Linux 容器）";
        boolean ready = false;
        boolean running = false;
        String detail = "";
        try {
            Result info = command("docker", "info", "--format", "{{.OSType}}");
            ready = info.code == 0 && "linux".equals(info.output);
            detail = ready ? "" : "请启动 Docker 并切换到 Linux 容器";
            if (ready) {
                Result state = command("docker", "inspect", "--format", "{{.State.Running}}", NAME);
                running = state.code == 0 && "true".equals(state.output);
            }
        } catch (IllegalStateException e) {
            detail = e.getMessage();
        }
        return Map.of("platform", os, "arch", System.getProperty("os.arch"),
                "method", method, "runtimeReady", ready, "running", running,
                "message", detail, "image", IMAGE, "mqttPort", 1883, "dashboardPort", 18083);
    }

    private static String dockerExecutable() {
        String os = System.getProperty("os.name").toLowerCase(Locale.ROOT);
        if (os.contains("win")) {
            String programFiles = System.getenv("ProgramFiles");
            if (programFiles != null) {
                Path docker = Path.of(programFiles, "Docker", "Docker", "resources", "bin", "docker.exe");
                if (Files.isRegularFile(docker)) return docker.toString();
            }
        }
        if (os.contains("mac")) {
            Path docker = Path.of("/Applications/Docker.app/Contents/Resources/bin/docker");
            if (Files.isRegularFile(docker)) return docker.toString();
        }
        return "docker";
    }

    public List<String> installCommand() {
        String os = System.getProperty("os.name").toLowerCase(Locale.ROOT);
        Path scripts = Path.of("scripts").toAbsolutePath();
        if (os.contains("win")) {
            return List.of("powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                    "-File", scripts.resolve("install-emqx.ps1").toString(), "-Image", IMAGE);
        }
        if (os.contains("linux") || os.contains("mac")) {
            return List.of("bash", scripts.resolve("install-emqx.sh").toString(), IMAGE);
        }
        throw new IllegalStateException("当前操作系统不支持自动安装 Docker");
    }

    public synchronized void action(String action) {
        if (!List.of("start", "stop").contains(action)) throw new IllegalArgumentException("不支持的操作");
        Result result;
        if ("stop".equals(action)) {
            result = command("docker", "stop", "--time", "5", NAME);
        } else {
            Result existing = command("docker", "inspect", NAME);
            if (existing.code == 0) {
                result = command("docker", "start", NAME);
            } else {
                result = command("docker", "run", "--pull=never", "-d", "--name", NAME,
                        "--restart", "unless-stopped", "-p", "1883:1883", "-p", "127.0.0.1:18083:18083",
                        "-v", "agent-console-emqx-data:/opt/emqx/data",
                        "-v", "agent-console-emqx-log:/opt/emqx/log", IMAGE);
            }
        }
        if (result.code != 0) throw new IllegalStateException(result.output);
    }
}
