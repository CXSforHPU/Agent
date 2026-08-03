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

# add config
src.append(os.path.join(cwd, 'config', 'AgentConfig.c'))


CPPPATH.append(os.path.join(cwd, 'include'))
CPPPATH.append(os.path.join(cwd,"include","channels"))
CPPPATH.append(os.path.join(cwd,"include","tools"))
CPPPATH.append(os.path.join(cwd,"config"))
group = DefineGroup('Agent',
                    src,
                    depend=["PKG_USING_AGENT"],
                    CPPPATH=CPPPATH)

Return('group')
