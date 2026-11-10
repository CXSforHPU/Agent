"""按目录发现用户工具。

网关启动时扫这个目录，把里面继承 Tool 的类实例化，装进工具表。目录里既有
网关自带的联网搜索，也有使用者自己加的工具。

一个模块一个工具类，类要有无参构造函数。一个文件出错不影响其他的文件。
"""
import importlib.util
import inspect
import os
import sys

from .base import Tool

PACKAGE_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.dirname(PACKAGE_DIR)
USER_TOOL_DIR = os.path.join(PACKAGE_DIR, "user_tools")


def load_user_tools(directory=None):
    """扫描目录，把里面继承 Tool 的类实例化。

    一个模块一个工具类，类要有无参构造函数。一个文件出错不影响其他的文件。
    同名的工具只留先加载的那个。

    类里写了 namespace 就用它自己的，没写就按用户工具加 user__ 前缀。

    @param directory 用户工具目录，默认 tools/user_tools
    @return 工具列表
    """
    directory = directory or USER_TOOL_DIR
    tools = []
    seen = set()
    # 用户文件里写 from tools.base import Tool，要能找到这个包
    if ROOT_DIR not in sys.path:
        sys.path.insert(0, ROOT_DIR)
    if not os.path.isdir(directory):
        print("[mcp] 用户工具目录不存在：{}".format(directory))
        return tools

    for entry in sorted(os.listdir(directory)):
        if not entry.endswith(".py") or entry.startswith("_"):
            continue
        module = _load_module(os.path.join(directory, entry))
        if module is None:
            continue
        for tool_class in _tool_classes(module):
            try:
                tool = tool_class()
            except Exception as exc:
                print("[mcp] 用户工具 {} 注册失败：{}".format(tool_class.__name__, exc))
                continue
            name = tool.name
            if not name:
                print("[mcp] 用户工具 {} 没有名字，跳过".format(tool_class.__name__))
                continue
            if name in seen:
                print("[mcp] 用户工具 {} 已经注册过了，跳过".format(name))
                continue
            seen.add(name)
            apply_namespace(tool)
            tools.append(tool)
    return tools


def apply_namespace(tool):
    """给没写 namespace 的工具加上用户工具的前缀。"""
    if tool.namespace is None:
        tool.namespace = "user__"
    if tool.prefix is None:
        tool.prefix = tool.namespace or ""


def _tool_classes(module):
    """模块里自己定义的 Tool 子类。import 进来的类不算。"""
    for _, obj in inspect.getmembers(module, inspect.isclass):
        if obj is Tool or not issubclass(obj, Tool):
            continue
        if obj.__module__ != module.__name__:
            continue
        yield obj


def _load_module(path):
    """按路径加载一个 .py 文件，出错返回 None。"""
    name = "agent_user_tool_" + os.path.splitext(os.path.basename(path))[0]
    try:
        spec = importlib.util.spec_from_file_location(name, path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)
        return module
    except Exception as exc:
        sys.modules.pop(name, None)
        print("[mcp] 加载用户工具 {} 失败：{}".format(os.path.basename(path), exc))
        return None
