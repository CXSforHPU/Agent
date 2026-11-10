"""工具表。

网关认识的所有工具都在这里排成一队，每一项是一个 Tool。工具表管三件事：

- 注册时把上下文交给工具，让工具自己决定能不能用。
- 给工具编对外的名字，撞名的加前缀，再撞就加序号。
- 按名字找出要调用的工具。

工具本身在 tools/ 目录里，不 import 这一层。
"""
import threading

from tools.base import Tool
from tools.context import ToolContext


class ToolRegistry:
    """工具表。"""

    def __init__(self, context=None):
        self._context = context or ToolContext()
        self._tools = []
        self._routes = {}
        self._descriptions = []
        self._lock = threading.Lock()

    def add_tool(self, tool):
        """注册一个工具。

        注册的时候调一次 configure(context)，工具把配置读进来。工具自己说
        不能用（enabled 为假）就跳过，返回 False。

        @return 注册成功返回 True
        """
        with self._lock:
            tool.configure(self._context)
            if not tool.enabled:
                print("[mcp] 工具 {} 现在不能用，跳过".format(tool.name))
                return False
            self._tools.append(tool)
            self._routes = {}
            self._descriptions = []
            return True

    def add_tools(self, tools):
        """注册一批工具，返回真正注册上的那些。"""
        return [tool for tool in tools if self.add_tool(tool)]

    def close(self):
        """释放工具持有的资源。"""
        with self._lock:
            tools = list(self._tools)
            self._tools = []
            self._routes = {}
            self._descriptions = {}
        for tool in tools:
            tool.close()

    def list_tools(self):
        """工具描述数组，每项含 name、description、inputSchema。"""
        with self._lock:
            if not self._routes:
                self._build_routes()
            return [dict(item) for item in self._descriptions]

    def find(self, name):
        """按对外的名字找工具，找不到返回 None。"""
        with self._lock:
            if not self._routes:
                self._build_routes()
            return self._routes.get(name)

    def _build_routes(self):
        """给工具表编名字，撞名的加前缀，再撞就加序号。"""
        routes = {}
        descriptions = []
        for tool in self._tools:
            name = tool.exposed_name
            if name in routes:
                name = (tool.prefix or "") + tool.exposed_name
                base = name
                index = 2
                while name in routes:
                    name = "{}_{}".format(base, index)
                    index += 1
            routes[name] = tool
            item = tool.schema()
            item["name"] = name
            descriptions.append(item)
        self._routes = routes
        self._descriptions = descriptions
        return descriptions
