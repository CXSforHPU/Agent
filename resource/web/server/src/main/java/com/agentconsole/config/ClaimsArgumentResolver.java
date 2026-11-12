package com.agentconsole.config;

import com.agentconsole.auth.JwtService;
import org.springframework.core.MethodParameter;
import org.springframework.stereotype.Component;
import org.springframework.web.bind.support.WebDataBinderFactory;
import org.springframework.web.context.request.NativeWebRequest;
import org.springframework.web.context.request.RequestAttributes;
import org.springframework.web.method.support.HandlerMethodArgumentResolver;
import org.springframework.web.method.support.ModelAndViewContainer;

/**
 * 让控制器直接声明 {@code JwtService.Claims xxx} 参数，值来自 AuthFilter 挂的请求属性。
 */
@Component
public class ClaimsArgumentResolver implements HandlerMethodArgumentResolver {

    @Override
    public boolean supportsParameter(MethodParameter parameter) {
        return parameter.getParameterType() == JwtService.Claims.class;
    }

    @Override
    public Object resolveArgument(MethodParameter parameter, ModelAndViewContainer mavContainer,
                                  NativeWebRequest webRequest, WebDataBinderFactory binderFactory) {
        Object attr = webRequest.getAttribute(AuthFilter.CLAIMS_ATTR, RequestAttributes.SCOPE_REQUEST);
        if (attr instanceof JwtService.Claims claims) {
            return claims;
        }
        throw new IllegalStateException("请求缺少已认证的用户身份");
    }
}
