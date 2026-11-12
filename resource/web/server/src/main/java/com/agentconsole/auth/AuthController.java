package com.agentconsole.auth;

import com.agentconsole.common.ApiResponse;
import jakarta.validation.Valid;
import jakarta.validation.constraints.NotBlank;
import jakarta.validation.constraints.Size;
import org.springframework.http.HttpStatus;
import org.springframework.http.ResponseEntity;
import org.springframework.validation.annotation.Validated;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;

@RestController
@RequestMapping("/api/auth")
@Validated
public class AuthController {

    private final AuthService auth;

    public AuthController(AuthService auth) {
        this.auth = auth;
    }

    public record Credentials(@NotBlank @Size(max = 32) String username,
                              @NotBlank @Size(min = 6, max = 64) String password) {
    }

    @PostMapping("/register")
    public ResponseEntity<ApiResponse<AuthService.Token>> register(@Valid @RequestBody Credentials body) {
        try {
            return ResponseEntity.ok(ApiResponse.ok(auth.register(body.username(), body.password())));
        } catch (IllegalArgumentException e) {
            return ResponseEntity.status(HttpStatus.BAD_REQUEST).body(ApiResponse.fail(e.getMessage()));
        }
    }

    @PostMapping("/login")
    public ResponseEntity<ApiResponse<AuthService.Token>> login(@Valid @RequestBody Credentials body) {
        try {
            return ResponseEntity.ok(ApiResponse.ok(auth.login(body.username(), body.password())));
        } catch (IllegalArgumentException e) {
            return ResponseEntity.status(HttpStatus.UNAUTHORIZED).body(ApiResponse.fail(e.getMessage()));
        }
    }

    @GetMapping("/me")
    public ApiResponse<Me> me(JwtService.Claims claims) {
        return ApiResponse.ok(new Me(claims.userId(), claims.username()));
    }

    public record Me(long userId, String username) {
    }
}
