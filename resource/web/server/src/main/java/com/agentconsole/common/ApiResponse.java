package com.agentconsole.common;

/**
 * 统一响应包。前端只认 success / message / data 三个字段。
 */
public record ApiResponse<T>(boolean success, String message, T data) {

    public static <T> ApiResponse<T> ok(T data) {
        return new ApiResponse<>(true, "ok", data);
    }

    public static ApiResponse<Void> ok() {
        return new ApiResponse<>(true, "ok", null);
    }

    public static <T> ApiResponse<T> fail(String message) {
        return new ApiResponse<>(false, message, null);
    }
}
