from building import *
import os
cwd = GetCurrentDir()

src = []
CPPPATH = []




for file in os.listdir(os.path.join(cwd, 'src')):
    file_path = os.path.join(cwd, 'src', file)
    if file.endswith('.c') and os.path.isfile(file_path):
        src.append(file_path)

for file in os.listdir(os.path.join(cwd, 'src',"channels")):
    file_path = os.path.join(cwd, 'src', "channels",file)
    if file.endswith('.c') and os.path.isfile(file_path):
        src.append(file_path)

for file in os.listdir(os.path.join(cwd, 'src',"tools")):
    file_path = os.path.join(cwd, 'src', "tools",file)
    if file.endswith('.c') and os.path.isfile(file_path):
        src.append(file_path)

# 添加头文件路径
CPPPATH.append(os.path.join(cwd, 'include'))
CPPPATH.append(os.path.join(cwd,"include","channels"))
CPPPATH.append(os.path.join(cwd,"include","tools"))
# 定义软件包组
group = DefineGroup('Agent',
                    src,
                    depend=[],
                    CPPPATH=CPPPATH)

Return('group')
