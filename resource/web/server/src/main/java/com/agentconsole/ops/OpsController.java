package com.agentconsole.ops;

import com.agentconsole.auth.JwtService;
import com.agentconsole.common.ApiResponse;
import org.springframework.http.HttpHeaders;
import org.springframework.http.HttpStatus;
import org.springframework.http.MediaType;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.PutMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.bind.annotation.RestController;
import org.springframework.web.multipart.MultipartFile;

import java.io.IOException;
import java.util.List;
import java.util.Map;

@RestController
@RequestMapping("/api/ops")
public class OpsController {

    private final OpsService ops;

    public OpsController(OpsService ops) {
        this.ops = ops;
    }

    /** 各组件状态。 */
    @GetMapping("/status")
    public ApiResponse<Map<String, Object>> status(JwtService.Claims claims) {
        return ApiResponse.ok(ops.status());
    }

    /** 运维面板的宿主级设置，目前是文件服务器的端口。 */
    @GetMapping("/settings")
    public ApiResponse<Map<String, Object>> settings(JwtService.Claims claims) {
        return ApiResponse.ok(ops.settings());
    }

    /** 改文件服务器的端口。body 是 {"port": 8001}。 */
    @PutMapping("/settings")
    public ResponseEntity<ApiResponse<Map<String, Object>>> updateSettings(
            @RequestBody Map<String, Object> body,
            JwtService.Claims claims) {
        Object raw = body.get("port");
        int port;
        if (raw instanceof Number number) {
            port = number.intValue();
        } else if (raw != null) {
            try {
                port = Integer.parseInt(raw.toString().trim());
            } catch (NumberFormatException e) {
                return ResponseEntity.badRequest().body(ApiResponse.fail("端口要填数字"));
            }
        } else {
            return ResponseEntity.badRequest().body(ApiResponse.fail("没给端口"));
        }
        try {
            return ResponseEntity.ok(ApiResponse.ok(ops.setPort(port)));
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail(e.getMessage()));
        } catch (IllegalStateException e) {
            return ResponseEntity.status(HttpStatus.CONFLICT).body(ApiResponse.fail(e.getMessage()));
        }
    }

    /** 装依赖。返回任务 id，前端轮询 /jobs/{id} 看进度。 */
    @PostMapping("/install")
    public ResponseEntity<ApiResponse<Map<String, String>>> install(
            @RequestParam(defaultValue = "file-server") String target,
            @RequestParam(defaultValue = "false") boolean withTts,
            JwtService.Claims claims) {
        try {
            String id = ops.install(target, withTts);
            return ResponseEntity.ok(ApiResponse.ok(Map.of("jobId", id)));
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail(e.getMessage()));
        }
    }

    @GetMapping("/jobs")
    public ApiResponse<List<Map<String, Object>>> jobs(JwtService.Claims claims) {
        return ApiResponse.ok(ops.jobs().stream().map(OpsService.Job::view).toList());
    }

    @GetMapping("/jobs/{id}")
    public ResponseEntity<ApiResponse<Map<String, Object>>> job(@PathVariable String id,
                                                               JwtService.Claims claims) {
        try {
            return ResponseEntity.ok(ApiResponse.ok(ops.job(id).view()));
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail(e.getMessage()));
        }
    }

    /** 起文件服务器，连带起 MCP 网关。 */
    @PostMapping("/start")
    public ResponseEntity<ApiResponse<Map<String, Object>>> start(JwtService.Claims claims) {
        try {
            return ResponseEntity.ok(ApiResponse.ok(ops.start()));
        } catch (IllegalStateException e) {
            return ResponseEntity.status(HttpStatus.BAD_GATEWAY).body(ApiResponse.fail(e.getMessage()));
        }
    }

    /** 停文件服务器。 */
    @PostMapping("/stop")
    public ResponseEntity<ApiResponse<Map<String, Object>>> stop(JwtService.Claims claims) {
        return ResponseEntity.ok(ApiResponse.ok(ops.stop()));
    }

    /** 可用音色。 */
    @GetMapping("/tts/speakers")
    public ResponseEntity<ApiResponse<Map<String, Object>>> speakers(JwtService.Claims claims) {
        try {
            return ResponseEntity.ok(ApiResponse.ok(ops.speakers()));
        } catch (IllegalStateException e) {
            return ResponseEntity.status(HttpStatus.BAD_GATEWAY).body(ApiResponse.fail(e.getMessage()));
        }
    }

    /** 合成进度，文件服务器自己记着跑到第几句。 */
    @GetMapping("/tts/progress")
    public ResponseEntity<ApiResponse<Map<String, Object>>> ttsProgress(JwtService.Claims claims) {
        try {
            return ResponseEntity.ok(ApiResponse.ok(ops.ttsProgress()));
        } catch (IllegalStateException e) {
            return ResponseEntity.status(HttpStatus.BAD_GATEWAY).body(ApiResponse.fail(e.getMessage()));
        }
    }

    /** 合成语音，直接返回 WAV 字节，前端塞进 audio 标签播。出错时回 JSON 错误包。 */
    @PostMapping("/tts")
    public ResponseEntity<?> tts(@RequestParam String text,
                                 @RequestParam(required = false) String speaker,
                                 JwtService.Claims claims) {
        byte[] wav;
        try {
            wav = ops.tts(text, speaker);
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail(e.getMessage()));
        } catch (IllegalStateException e) {
            return ResponseEntity.status(HttpStatus.BAD_GATEWAY).body(ApiResponse.fail(e.getMessage()));
        }
        HttpHeaders headers = new HttpHeaders();
        headers.setContentType(MediaType.parseMediaType("audio/wav"));
        headers.setContentLength(wav.length);
        return new ResponseEntity<>(wav, headers, HttpStatus.OK);
    }

    /** 语音转文本，multipart 的 file 字段。 */
    @PostMapping("/stt")
    public ResponseEntity<ApiResponse<Map<String, String>>> stt(@RequestParam("file") MultipartFile file,
                                                                JwtService.Claims claims) {
        try {
            String text = ops.stt(file.getBytes(), file.getOriginalFilename());
            return ResponseEntity.ok(ApiResponse.ok(Map.of("text", text)));
        } catch (IOException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail("读音频失败：" + e.getMessage()));
        } catch (IllegalArgumentException e) {
            return ResponseEntity.badRequest().body(ApiResponse.fail(e.getMessage()));
        } catch (IllegalStateException e) {
            return ResponseEntity.status(HttpStatus.BAD_GATEWAY).body(ApiResponse.fail(e.getMessage()));
        }
    }

    /** 日志尾部，name 取 file-server 或 install。 */
    @GetMapping("/log")
    public ApiResponse<List<String>> log(@RequestParam(defaultValue = "file-server") String name,
                                         @RequestParam(defaultValue = "100") int lines,
                                         JwtService.Claims claims) {
        return ApiResponse.ok(ops.tail(name, Math.min(lines, 500)));
    }
}
