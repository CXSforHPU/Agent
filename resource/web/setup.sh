#!/usr/bin/env bash
# 一键安装并启动 web 控制台。
#
#   ./setup.sh              解压工具链、装依赖、启动前后端
#   ./setup.sh --install    只装依赖，不启动
#   ./setup.sh --server     只启动后端
#   ./setup.sh --client     只启动前端
#   ./setup.sh --stop       停掉之前后台起的进程
#
# 工具链（JDK、Maven）随包放在 .jdk/ 与 .maven/ 下，不改系统 PATH；
# 系统里已经装了 java 时优先用系统的。

set -euo pipefail

WEB_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVER_DIR="$WEB_DIR/server"
CLIENT_DIR="$WEB_DIR/client"
RUN_DIR="$WEB_DIR/.run"
JDK_ZIP="$SERVER_DIR/.jdk/jdk.zip"
MAVEN_ZIP="$SERVER_DIR/.maven/maven.zip"
JDK_URL="https://mirrors.tuna.tsinghua.edu.cn/Adoptium/17/jdk/x64/windows/OpenJDK17U-jdk_x64_windows_hotspot_17.0.21_9.zip"
MAVEN_URL="https://archive.apache.org/dist/maven/maven-3/3.9.9/binaries/apache-maven-3.9.9-bin.zip"

log()  { printf '\033[32m[web]\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[web]\033[0m %s\n' "$*"; }
die()  { printf '\033[31m[web]\033[0m %s\n' "$*" >&2; exit 1; }

# ---------- 工具链 ----------

find_dir() {  # 在目录里找第一个匹配前缀的子目录
    local parent="$1" prefix="$2"
    [ -d "$parent" ] || return 1
    local d
    for d in "$parent"/${prefix}*/; do
        [ -d "$d" ] && { printf '%s' "${d%/}"; return 0; }
    done
    return 1
}

ensure_jdk() {
    if command -v java >/dev/null 2>&1; then
        JAVA_BIN="$(command -v java)"
        JAVA_HOME_DIR="$(dirname "$(dirname "$(readlink -f "$JAVA_BIN" 2>/dev/null || echo "$JAVA_BIN")")")"
        log "使用系统 java：$("$JAVA_BIN" -version 2>&1 | head -1)"
        return
    fi

    local home
    if home="$(find_dir "$SERVER_DIR/.jdk" "jdk-")"; then
        JAVA_HOME_DIR="$home"
        JAVA_BIN="$home/bin/java"
        log "使用包内 JDK：$JAVA_HOME_DIR"
        return
    fi

    log "包内没有 JDK，开始下载（约 190 MB）"
    mkdir -p "$SERVER_DIR/.jdk"
    if [ ! -f "$JDK_ZIP" ]; then
        curl -fL --retry 3 -o "$JDK_ZIP" "$JDK_URL" || die "JDK 下载失败，可手动下载后放到 $JDK_ZIP"
    fi
    unzip -q -o "$JDK_ZIP" -d "$SERVER_DIR/.jdk" || die "JDK 解压失败"
    home="$(find_dir "$SERVER_DIR/.jdk" "jdk-")" || die "解压后没找到 jdk- 目录"
    JAVA_HOME_DIR="$home"
    JAVA_BIN="$home/bin/java"
    log "JDK 就绪：$JAVA_HOME_DIR"
}

ensure_maven() {
    if command -v mvn >/dev/null 2>&1; then
        MVN_BIN="$(command -v mvn)"
        log "使用系统 mvn：$MVN_BIN"
        return
    fi

    local home
    if home="$(find_dir "$SERVER_DIR/.maven" "apache-maven-")"; then
        MVN_BIN="$home/bin/mvn"
        log "使用包内 Maven：$MVN_BIN"
        return
    fi

    log "包内没有 Maven，开始下载"
    mkdir -p "$SERVER_DIR/.maven"
    if [ ! -f "$MAVEN_ZIP" ]; then
        curl -fL --retry 3 -o "$MAVEN_ZIP" "$MAVEN_URL" || die "Maven 下载失败，可手动下载后放到 $MAVEN_ZIP"
    fi
    unzip -q -o "$MAVEN_ZIP" -d "$SERVER_DIR/.maven" || die "Maven 解压失败"
    home="$(find_dir "$SERVER_DIR/.maven" "apache-maven-")" || die "解压后没找到 apache-maven- 目录"
    MVN_BIN="$home/bin/mvn"
    log "Maven 就绪：$MVN_BIN"
}

mvn_run() {
    JAVA_HOME="$JAVA_HOME_DIR" PATH="$JAVA_HOME_DIR/bin:$PATH" "$MVN_BIN" "$@"
}

# ---------- 安装 ----------

install_server() {
    log "编译后端"
    mvn_run -q -f "$SERVER_DIR/pom.xml" -DskipTests package
    log "后端产物：$SERVER_DIR/target/agent-console-server-1.0.0.jar"
}

install_client() {
    if [ ! -f "$CLIENT_DIR/package.json" ]; then
        warn "前端还没写，跳过（$CLIENT_DIR/package.json 不存在）"
        return
    fi
    log "安装前端依赖"
    (cd "$CLIENT_DIR" && npm install --no-fund --no-audit)
}

# ---------- 启动 ----------

wait_http() {  # 等 HTTP 探活，最多等 $2 秒
    local url="$1" seconds="${2:-60}" i=0
    while [ "$i" -lt "$seconds" ]; do
        if curl -fsS -o /dev/null "$url" 2>/dev/null; then
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    return 1
}

# Git Bash 里 $! 拿到的是 MSYS 的 pid，kill 不到 Windows 上的 java，
# 所以统一按端口反查 pid 再杀。
pid_on_port() {
    # netstat 列：$1 协议 $2 本地地址 $3 外部地址 $4 状态 $5 pid
    netstat -ano 2>/dev/null \
        | awk -v p=":$1" '$2 ~ p"$" && $4 == "LISTENING" {print $5}' \
        | head -1
}

kill_port() {
    local port="$1" pid os
    pid="$(pid_on_port "$port")"
    [ -n "$pid" ] || return 1
    os="$(uname -s 2>/dev/null || echo unknown)"
    case "$os" in
        MSYS*|MINGW*|CYGWIN*)
            # MSYS2_ARG_CONV_EXCL 防止 /PID 被改写成盘符路径
            MSYS2_ARG_CONV_EXCL="*" taskkill /PID "$pid" /F >/dev/null 2>&1 || true
            ;;
        *)
            kill "$pid" 2>/dev/null || true
            ;;
    esac
    # taskkill 是异步的，等端口真正释放
    local i=0
    while [ "$i" -lt 10 ] && [ -n "$(pid_on_port "$port")" ]; do
        sleep 0.5
        i=$((i + 1))
    done
    log "已停掉监听 $port 的进程（pid $pid）"
}

start_server() {
    local jar="$SERVER_DIR/target/agent-console-server-1.0.0.jar"
    [ -f "$jar" ] || die "后端没编译过，先跑 ./setup.sh --install"
    if wait_http "http://127.0.0.1:8080/api/health" 2; then
        log "后端已经在跑（8080）"
        return
    fi
    kill_port 8080 || true
    mkdir -p "$RUN_DIR"
    log "启动后端（8080），日志 $RUN_DIR/server.log"
    (cd "$SERVER_DIR" && "$JAVA_BIN" -jar "$jar" >"$RUN_DIR/server.log" 2>&1 &)
    wait_http "http://127.0.0.1:8080/api/health" 90 || {
        warn "后端 90 秒内没起来，最后 40 行日志："
        tail -40 "$RUN_DIR/server.log" || true
        die "启动失败"
    }
    log "后端已就绪：http://127.0.0.1:8080"
}

start_client() {
    if [ ! -f "$CLIENT_DIR/package.json" ]; then
        warn "前端还没写，跳过"
        return
    fi
    if wait_http "http://127.0.0.1:5173" 2; then
        log "前端已经在跑（5173）"
        return
    fi
    kill_port 5173 || true
    mkdir -p "$RUN_DIR"
    log "启动前端（5173），日志 $RUN_DIR/client.log"
    (cd "$CLIENT_DIR" && npm run dev >"$RUN_DIR/client.log" 2>&1 &)
    wait_http "http://127.0.0.1:5173" 90 || {
        warn "前端 90 秒内没起来，最后 40 行日志："
        tail -40 "$RUN_DIR/client.log" || true
        die "启动失败"
    }
    log "前端已就绪：http://127.0.0.1:5173"
}

stop_all() {
    local found=0
    kill_port 8080 && found=1
    kill_port 5173 && found=1
    [ "$found" = 1 ] || log "没有在跑的前后端进程"
}

# ---------- 入口 ----------

action="${1:-all}"

case "$action" in
    --stop)
        stop_all
        exit 0
        ;;
    --install|--server|--client|all) ;;
    *) die "未知参数：$action（可用：--install --server --client --stop 或不带参数）" ;;
esac

ensure_jdk
ensure_maven

if [ "$action" = "--install" ] || [ "$action" = "all" ]; then
    install_server
    install_client
fi

if [ "$action" = "--server" ] || [ "$action" = "all" ]; then
    start_server
fi

if [ "$action" = "--client" ] || [ "$action" = "all" ]; then
    start_client
fi

if [ "$action" = "all" ]; then
    log "全部就绪"
    log "  后端  http://127.0.0.1:8080"
    log "  前端  http://127.0.0.1:5173"
    log "  停止  ./setup.sh --stop"
fi
