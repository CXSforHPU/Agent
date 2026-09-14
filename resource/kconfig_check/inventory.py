import os, re, sys
HERE = r"C:\Users\31309\Desktop\agent_test\packages\Agent-latest\resource\kconfig_check"
BSP = r"C:\Users\31309\Desktop\agent_test"
kc = open(os.path.join(HERE, "Agent.Kconfig"), encoding="utf-8").read()
syms = set(re.findall(r"^\s*(?:menuconfig|config)\s+(PKG_[A-Z0-9_]+)", kc, re.M))
real = set()
for line in open(os.path.join(BSP, ".config"), encoding="utf-8"):
    line = line.strip()
    if line.startswith("CONFIG_PKG_AGENT"):
        real.add(line[7:].split("=")[0])
print("Kconfig symbols: %d   .config PKG_AGENT*: %d" % (len(syms), len(real)))
print("in .config but NOT in new Kconfig:")
for s in sorted(real - syms):
    print("   ", s)
print("added by the new Kconfig (not in current .config):")
for s in sorted(syms - real):
    print("   ", s)

# code-side inventory
code = ""
for root, _, files in os.walk(os.path.join(BSP, "packages", "Agent-latest")):
    if "resource" in root:
        continue
    for f in files:
        if f.endswith((".c", ".h")):
            code += open(os.path.join(root, f), encoding="utf-8", errors="ignore").read()
used = set(re.findall(r"PKG_AGENT[A-Z0-9_]*", code))
print("code references missing from new Kconfig:", sorted(used - syms))
