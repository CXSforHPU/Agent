package com.agentconsole;

import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.scheduling.annotation.EnableScheduling;

import java.nio.file.Files;
import java.nio.file.Path;

@SpringBootApplication
@EnableScheduling
public class ConsoleApplication {

    /** SQLite 不会自己建父目录，启动前先把库文件所在目录准备好。 */
    private static final String DEFAULT_DB = "./data/agent-console.db";

    public static void main(String[] args) {
        prepareDatabaseDirectory();
        SpringApplication.run(ConsoleApplication.class, args);
    }

    private static void prepareDatabaseDirectory() {
        String url = System.getenv().getOrDefault("AGENT_CONSOLE_DB", DEFAULT_DB);
        // application.yml 里写死的默认值和这里一致；用 AGENT_CONSOLE_DB 覆盖时两边同步
        if (System.getProperty("agent.console.db") != null) {
            url = System.getProperty("agent.console.db");
        }
        try {
            Path dir = Path.of(url).toAbsolutePath().getParent();
            if (dir != null) {
                Files.createDirectories(dir);
            }
        } catch (Exception e) {
            throw new IllegalStateException("无法创建数据库目录：" + url, e);
        }
    }
}
