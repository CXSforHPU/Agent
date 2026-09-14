# MQTT 工具测试脚本

配套 [../../docs/tool_mqtt.md](../../docs/tool_mqtt.md) 的验证工具：在 QEMU 上跑固件，通过 TCP 串口驱动 MSH 命令，并用 PC 端最小 MQTT 客户端（仅标准库）做真实 broker 收发对照。

## 文件

| 文件 | 说明 |
|---|---|
| `qemu_msh_test.ps1` | MSH 冒烟测试：启动客户端、订阅多个话题、通配符路由、发布/回环、poll 缓冲与退订 |
| `qemu_e2e_test.ps1` | 端到端测试：自然语言让 agent 用 `mqtt_publish` 下发命令（PC 订阅端验证收到），并向 `agent/sub` 发消息验证「订阅 → 交给 agent 分析」 |
| `mqtt_pub.py` | 最小 MQTT 3.1.1 发布客户端（stdlib，无第三方依赖） |
| `mqtt_sub.py` | 最小 MQTT 3.1.1 订阅客户端（stdlib，带 PINGREQ 保活） |

## 依赖

- QEMU：`D:\rtt\env-windows\tools\qemu\qemu64\qemu-system-arm.exe`（脚本内 `$qemu` 可按需修改）
- Python 3（用于 PC 端 MQTT 客户端）
- 已编译的 `rtthread.bin`、`sd.bin`（BSP 根目录）
- 网络可达的 broker（默认取 `rtconfig.h` 里配置的 `tcp://47.93.225.100:1883`，PC 与 QEMU 都需要能访问）

## 用法

```powershell
# MSH 冒烟测试（建议先跑这个）
powershell -ExecutionPolicy Bypass -File .\qemu_msh_test.ps1 -Port 5580 -Smp 2

# 端到端测试（会调用大模型，需要有效 API Key）
powershell -ExecutionPolicy Bypass -File .\qemu_e2e_test.ps1 -Port 5600
```

脚本会用 `-serial tcp:127.0.0.1:<Port>,server,nowait` 连接串口，自动完成：杀残留 QEMU → 启动 → 等启动完成 → 逐条执行命令 → 打印完整记录 → 关闭 QEMU。

## 只看板端、手动验证

```powershell
qemu-system-arm -M vexpress-a9 -smp cpus=2 -kernel rtthread.bin -sd sd.bin `
    -display none -serial tcp:127.0.0.1:5580,server,nowait `
    -net nic,model=lan9118 -net user
# 再用 PuTTY / MobaXterm 以 Raw 模式连接 127.0.0.1:5580
```

MSH 命令见文档 §6。不需要 broker 也能验证链路：

```
msh /> mqtt_tool start
msh /> mqtt_tool sub device/+/data poll
msh /> mqtt_tool sim device/1/data {"temp":25}
msh /> mqtt_tool recv
```

## 注意

- QEMU 实例若被强制结束可能残留（脚本开头会自动清理），残留进程会占用串口端口并共用同一个 MQTT client id。
- 使用 gdb 调试时给 QEMU 加 `-s`，再 `arm-none-eabi-gdb -ex "target remote :1234" rtthread.elf`；板端卡死时可 `bt` 看当前线程调用栈。
