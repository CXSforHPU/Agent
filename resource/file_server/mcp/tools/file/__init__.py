"""内置文件工具。

工具名和参数与 Windows 文件资源管理器 MCP 连接器保持一致（get_file_details、
read_text_file、search_files 等），这样设备端在两种 Windows 上看到的接口一样：
Win11 走 ODR 连接器，Win10 走这个内置实现。

一个工具一个模块，模块里写一个继承基类的工具类。要加新工具，在这个目录写
一个模块，再把工具类登记到 TOOLS 里。

所有路径都被限制在根目录内，根目录之外的访问一律拒绝。
"""
import os

from .create_directory import CreateDirectoryTool
from .create_text_file import CreateTextFileTool
from .edit_text_file import EditTextFileTool
from .get_directories import GetDirectoriesTool
from .get_file_details import GetFileDetailsTool
from .move_file import MoveFileTool
from .read_file import ReadFileTool
from .read_text_file import ReadTextFileTool
from .search_files import SearchFilesTool
from .unzip_folder import UnzipFolderTool
from .zip_folder import ZipFolderTool

TOOLS = (GetFileDetailsTool, GetDirectoriesTool, ReadTextFileTool, ReadFileTool,
         SearchFilesTool, CreateDirectoryTool, CreateTextFileTool, EditTextFileTool,
         MoveFileTool, ZipFolderTool, UnzipFolderTool)


def build_tools(root=None):
    """按工具类表造出文件工具的列表。"""
    base = os.path.realpath(os.path.expanduser(root or "~"))
    return [tool_class(base) for tool_class in TOOLS]
