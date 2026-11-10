"""网关的配置。

命令行和环境变量只在这一层解析一次，解析出来是一个 Settings 对象。往下的层
只读这个对象，不再碰 argparse 和 os.environ。

键用点分层级，比如 web_search.url、web_search.mode。工具要配置就在
configure() 钩子里按键取。

只用标准库。
"""
import argparse
import os

from tools.user_tools.web_search_tool import (DEFAULT_MAX_RESULTS as DEFAULT_WEB_SEARCH_MAX_RESULTS,
                                              DEFAULT_MODE as DEFAULT_WEB_SEARCH_MODE,
                                              DEFAULT_TIMEOUT as DEFAULT_WEB_SEARCH_TIMEOUT,
                                              DEFAULT_URL as DEFAULT_WEB_SEARCH_URL)

# 板端一次最多收下 32768 字节的响应体，超了整个调用就失败。网关发包前把
# 回包压到这个上限以内：图片存到文件服务器，只留一句带下载地址的说明，
# 文字按预算截断。
DEFAULT_MAX_TEXT_BYTES = 24000

# 文件服务器的地址，工具回包里的图片存到这里
DEFAULT_FILE_SERVER = "http://127.0.0.1:8000"

# 一个工具最多跑多久
DEFAULT_CALL_TIMEOUT = 60.0

# 外部 MCP 服务器启动与握手的超时
DEFAULT_SERVER_TIMEOUT = 60.0


class Settings:
    """网关的运行配置。

    键用点分层级：web_search.url、web_search.mode。
    """

    def __init__(self, values=None):
        self._values = dict(values or {})

    def get(self, key, default=None):
        """取一项配置，没有就用默认值。"""
        return self._values.get(key, default)

    def set(self, key, value):
        """改一项配置。"""
        self._values[key] = value

    def as_dict(self):
        """全部配置，调试时打印用。"""
        return dict(self._values)

    def __contains__(self, key):
        return key in self._values


def parse_args(argv=None):
    """解析命令行和环境变量，返回配置对象。

    命令行优先于环境变量，环境变量优先于默认值。
    """
    args = _parser().parse_args(argv)
    return Settings(_values(args))


def _parser():
    """命令行参数表。默认值放在配置层，工具不认命令行。"""
    parser = argparse.ArgumentParser(description="RT-Thread Agent MCP 网关")
    parser.add_argument("--host", default="0.0.0.0", help="监听地址，默认 0.0.0.0")
    parser.add_argument("--port", type=int, default=8765, help="监听端口，默认 8765")
    parser.add_argument("--root", default=None,
                        help="文件工具可访问的根目录，默认当前用户主目录")
    parser.add_argument("--no-odr", action="store_true",
                        help="不接入 Windows ODR，只用内置文件工具")
    parser.add_argument("--server-timeout", type=float, default=DEFAULT_SERVER_TIMEOUT,
                        help="外部 MCP 服务器启动与握手的超时秒数，默认 {}".format(
                            DEFAULT_SERVER_TIMEOUT))
    parser.add_argument("--call-timeout", type=float, default=DEFAULT_CALL_TIMEOUT,
                        help="一个工具最多跑多久秒，默认 {}".format(
                            DEFAULT_CALL_TIMEOUT))
    parser.add_argument("--max-text-bytes", type=int, default=DEFAULT_MAX_TEXT_BYTES,
                        help="单次回包里文本的字节上限，默认 {}，"
                             "要小于板端的 PKG_AGENT_MCP_RESP_BUFSZ".format(
                                 DEFAULT_MAX_TEXT_BYTES))
    parser.add_argument("--file-server", default=DEFAULT_FILE_SERVER,
                        help="文件服务器地址，工具回包里的图片存到这里，默认 {}".format(
                            DEFAULT_FILE_SERVER))
    parser.add_argument("--file-server-url", default=None,
                        help="图片下载地址的基地址，默认跟 --file-server 一样；"
                             "文件服务器在内网而模型在云上时，这里填公网地址")
    parser.add_argument("--no-file-server", action="store_true",
                        help="不接文件服务器，回包里的图片直接换成占位说明")
    parser.add_argument("--windows-mcp", action="store_true",
                        help="接入 Windows-MCP 服务器，能控制本机桌面")
    parser.add_argument("--no-windows-mcp", action="store_true",
                        help="不接入 Windows-MCP 服务器")
    parser.add_argument("--windows-mcp-cmd", nargs=argparse.REMAINDER,
                        default=["uvx", "windows-mcp", "serve"],
                        help="Windows-MCP 的启动命令，默认 uvx windows-mcp serve")
    parser.add_argument("--user-tools", action="append", default=None,
                        help="额外的用户工具目录，可以给多次；"
                             "内置的 tools/user_tools 始终加载")
    parser.add_argument("--no-user-tools", action="store_true",
                        help="不加载用户自己的工具")
    parser.add_argument("--web-search-url", default=None,
                        help="联网搜索的地址，默认 {}".format(DEFAULT_WEB_SEARCH_URL))
    parser.add_argument("--web-search-key", default=None,
                        help="联网搜索的密钥，Bing 和 Brave 用得上")
    parser.add_argument("--web-search-mode", default=None,
                        help="联网搜索的对接方式：auto、searxng、bing、brave、post")
    parser.add_argument("--web-search-timeout", type=float, default=None,
                        help="联网搜索的超时秒数，默认 {}".format(
                            DEFAULT_WEB_SEARCH_TIMEOUT))
    parser.add_argument("--web-search-max-results", type=int, default=None,
                        help="联网搜索一次最多返回几条，默认 {}".format(
                            DEFAULT_WEB_SEARCH_MAX_RESULTS))
    parser.add_argument("--no-web-search", action="store_true",
                        help="不接入联网搜索工具")
    parser.add_argument("--pid-file", default=None,
                        help="把进程号写到这个文件，退出时删掉")
    return parser


def _values(args):
    """把命令行参数摊成配置项。"""
    values = {
        "host": args.host,
        "port": args.port,
        "root": args.root,
        "max_text_bytes": args.max_text_bytes,
        "call_timeout": args.call_timeout,
        "server_timeout": args.server_timeout,
        "file_server": args.file_server,
        "file_server_url": args.file_server_url,
        "no_file_server": bool(args.no_file_server),
        "windows_mcp": bool(args.windows_mcp) and not bool(args.no_windows_mcp),
        "windows_mcp_cmd": list(args.windows_mcp_cmd or []),
        "user_tools": list(args.user_tools or []),
        "no_user_tools": bool(args.no_user_tools),
        "no_odr": bool(args.no_odr),
        "pid_file": args.pid_file,
    }
    values.update(_web_search_values(args))
    return values


def _web_search_values(args):
    """联网搜索的配置。命令行优先，其次环境变量，最后默认值。"""
    env = os.environ
    url = _first(args.web_search_url, env.get("MCP_WEB_SEARCH_URL"),
                 DEFAULT_WEB_SEARCH_URL)
    key = _first(args.web_search_key, env.get("MCP_WEB_SEARCH_KEY"), "")
    mode = _first(args.web_search_mode, env.get("MCP_WEB_SEARCH_MODE"),
                  DEFAULT_WEB_SEARCH_MODE)
    timeout = _to_float(
        _first(args.web_search_timeout, env.get("MCP_WEB_SEARCH_TIMEOUT")),
        DEFAULT_WEB_SEARCH_TIMEOUT)
    count = _to_int(
        _first(args.web_search_max_results, env.get("MCP_WEB_SEARCH_MAX_RESULTS")),
        DEFAULT_WEB_SEARCH_MAX_RESULTS)
    enabled = not args.no_web_search and env.get("MCP_WEB_SEARCH_ENABLED", "1") != "0"
    return {
        "web_search.url": str(url).strip(),
        "web_search.key": str(key).strip(),
        "web_search.mode": str(mode).strip().lower(),
        "web_search.timeout": timeout,
        "web_search.max_results": count,
        "web_search.enabled": enabled,
    }


def _first(*values):
    """取第一个不是 None 的值。"""
    for value in values:
        if value is not None:
            return value
    return None


def _to_float(value, fallback):
    """转成数字，转不了就用默认值。"""
    try:
        return float(value)
    except (TypeError, ValueError):
        return fallback


def _to_int(value, fallback):
    """转成整数，转不了就用默认值。"""
    try:
        return int(value)
    except (TypeError, ValueError):
        return fallback
