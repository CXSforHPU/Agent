#!/bin/sh
# 启动 RT-Thread Agent 的 MCP 网关。
# 用法：./start_mcp_gateway.sh [端口] [文件工具根目录] [网关额外参数]
# 端口默认 8765，根目录默认当前用户主目录。
# 额外参数原样传给 mcp_gateway.py，例如 --windows-mcp 会接入 Windows-MCP。
set -e
cd "$(dirname "$0")"

PORT=8765
ROOT="$HOME"
if [ -n "$1" ]; then PORT="$1"; shift; fi
if [ -n "$1" ]; then ROOT="$1"; shift; fi

echo "[mcp] 监听端口 $PORT，文件工具根目录 $ROOT"
echo "[mcp] 设备端配置 PKG_AGENT_MCP_SERVER_URL=http://本机IP:$PORT/mcp"
exec python3 -u mcp_gateway.py --host 0.0.0.0 --port "$PORT" --root "$ROOT" "$@"
