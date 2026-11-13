package com.agentconsole.config;

import org.springframework.beans.factory.annotation.Value;
import org.springframework.context.annotation.Configuration;
import org.springframework.web.method.support.HandlerMethodArgumentResolver;
import org.springframework.web.servlet.config.annotation.CorsRegistry;
import org.springframework.web.servlet.config.annotation.WebMvcConfigurer;

import java.util.List;

/**
 * 跨域放行与参数解析器注册。前端开发服务器在 5173，需要跨域调 8080。
 */
@Configuration
public class WebConfig implements WebMvcConfigurer {

    @Value("${agent.cors.allowed-origins:http://localhost:5173,http://127.0.0.1:5173}")
    private String allowedOrigins;

    private final ClaimsArgumentResolver claimsResolver;

    public WebConfig(ClaimsArgumentResolver claimsResolver) {
        this.claimsResolver = claimsResolver;
    }

    @Override
    public void addCorsMappings(CorsRegistry registry) {
        registry.addMapping("/api/**")
                .allowedOrigins(allowedOrigins.split(","))
                // PATCH 用于改会话名。方法不在这个名单里时 Spring 直接回 403，
                // 浏览器那边看到的是改名失败，其它接口都正常。
                .allowedMethods("GET", "POST", "PUT", "PATCH", "DELETE", "OPTIONS")
                .allowedHeaders("Authorization", "Content-Type")
                .maxAge(3600);
    }

    @Override
    public void addArgumentResolvers(List<HandlerMethodArgumentResolver> resolvers) {
        resolvers.add(claimsResolver);
    }
}
