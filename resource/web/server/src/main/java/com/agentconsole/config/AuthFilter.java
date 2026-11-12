package com.agentconsole.config;

import com.agentconsole.auth.JwtService;
import jakarta.servlet.FilterChain;
import jakarta.servlet.ServletException;
import jakarta.servlet.http.HttpServletRequest;
import jakarta.servlet.http.HttpServletResponse;
import org.springframework.http.MediaType;
import org.springframework.stereotype.Component;
import org.springframework.web.filter.OncePerRequestFilter;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.List;

/**
 * 校验 Authorization: Bearer &lt;jwt&gt;，把载荷挂到请求属性上，
 * 供 {@link ClaimsArgumentResolver} 注入到控制器参数。
 * 白名单路径直接放行。
 */
@Component
public class AuthFilter extends OncePerRequestFilter {

    public static final String CLAIMS_ATTR = "agentconsole.claims";

    private static final List<String> PUBLIC_PREFIXES = List.of(
            "/api/auth/register",
            "/api/auth/login",
            "/api/health",
            // 板端启动时来要文件服务器与 MCP 地址，手上没有登录令牌
            "/api/ops/endpoints",
            "/error");

    private final JwtService jwt;

    public AuthFilter(JwtService jwt) {
        this.jwt = jwt;
    }

    @Override
    protected boolean shouldNotFilter(HttpServletRequest request) {
        String path = request.getRequestURI();
        return PUBLIC_PREFIXES.stream().anyMatch(path::startsWith);
    }

    @Override
    protected void doFilterInternal(HttpServletRequest request, HttpServletResponse response,
                                    FilterChain chain) throws ServletException, IOException {
        String header = request.getHeader("Authorization");
        if (header == null || !header.startsWith("Bearer ")) {
            reject(response, "缺少 Authorization 头");
            return;
        }
        try {
            JwtService.Claims claims = jwt.parse(header.substring(7).trim());
            request.setAttribute(CLAIMS_ATTR, claims);
        } catch (RuntimeException e) {
            reject(response, "令牌无效或已过期");
            return;
        }
        chain.doFilter(request, response);
    }

    private void reject(HttpServletResponse response, String message) throws IOException {
        response.setStatus(HttpServletResponse.SC_UNAUTHORIZED);
        response.setContentType(MediaType.APPLICATION_JSON_VALUE);
        response.setCharacterEncoding(StandardCharsets.UTF_8.name());
        response.getWriter().write("{\"success\":false,\"message\":\"" + message + "\"}");
    }
}
