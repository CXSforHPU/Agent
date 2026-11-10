"""工具调用能看见的东西。

一次工具调用要拿到两样东西：文件服务器（图片和音频存进去）和配置（搜索服务
的地址这类）。这两样以前放在模块级全局变量里，谁改谁说了算，多个工具同时跑
还会互相覆盖。

现在改成一份上下文：网关建一次，调用工具时按工具名复制一份放进
ContextVar，工具里用 current_context() 取。工具不 import 网关，也不碰全局。
"""
import contextvars


class ToolContext:
    """一次工具调用能看见的东西。

    file_server 是文件服务器客户端，没有就是 None，图片和音频存不了。
    settings 是网关的配置对象，工具按键取自己要的。
    tool_name 是当前调用的工具名，存文件时用它起名。
    """

    def __init__(self, file_server=None, settings=None, tool_name=""):
        self.file_server = file_server
        self.settings = settings
        self.tool_name = tool_name

    def for_tool(self, name):
        """按工具名复制一份，别的内容一样。"""
        return ToolContext(self.file_server, self.settings, name)

    def setting(self, key, default=None):
        """取一项配置，没有就用默认值。"""
        if self.settings is None:
            return default
        return self.settings.get(key, default)


_current = contextvars.ContextVar("agent_tool_context", default=None)


def current_context():
    """当前这次调用的上下文，不在调用里就是 None。"""
    return _current.get()


def use(context):
    """把上下文放进当前调用，返回一个令牌，调用完用 reset 还原。"""
    return _current.set(context)


def reset(token):
    """还原上下文。"""
    _current.reset(token)
