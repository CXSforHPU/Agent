"""Scenario runner: load a Kconfig file under test with given symbol values and
report the vsnprintf decision + the paho-pipe situation.

Usage: python scenarios.py Agent.Kconfig
"""

import os
import sys

sys.path.insert(0, r"D:\rtt\env-windows\.venv\Lib\site-packages")
import kconfiglib

HERE = os.path.dirname(os.path.abspath(__file__))
os.chdir(HERE)
os.environ["srctree"] = HERE

target = sys.argv[1] if len(sys.argv) > 1 else "Agent.Kconfig"
with open(os.path.join(HERE, "Kconfig.test"), "w", encoding="utf-8") as f:
    f.write('source "stubs/Kconfig.deps"\n\nsource "%s"\n' % target)

# label, rt_ver_num (hex str or None), major, libc_vsnprintf, klibc_standard, mode, mqtt, expected
SCENARIOS = [
    ("4.0.x: klibc has no rt_vsnprintf", "0x40003", 4, False, False, "AUTO", "y", "y"),
    ("4.1.x: klibc std -> keep kernel", "0x40100", 4, False, True, "AUTO", "y", "n"),
    ("4.1.x: klibc tiny -> keep kernel", "0x40100", 4, False, False, "AUTO", "y", "n"),
    ("5.3.x (=this BSP) std", "0x50300", 5, False, True, "AUTO", "y", "n"),
    ("5.3.x tiny (32-bit default)", "0x50300", 5, False, False, "AUTO", "y", "n"),
    ("5.3.x + LIBC_VSNPRINTF -> pkg unusable", "0x50300", 5, True, False, "AUTO", "y", "n"),
    ("5.2.x + LIBC_VSNPRINTF -> pkg unusable", "0x50200", 5, True, False, "AUTO", "y", "n"),
    ("5.1.x + LIBC_VSNPRINTF -> select pkg", "0x50100", 5, True, False, "AUTO", "y", "y"),
    ("4.0.x, explicit USE_KLIB", "0x40003", 4, False, False, "KLIB", "y", "n"),
    ("5.3.x, explicit USE_PKG (unavailable)", "0x50300", 5, False, True, "PKG", "y", "n"),
    ("5.1.x, explicit USE_PKG", "0x50100", 5, False, True, "PKG", "y", "y"),
    # Kconfig cannot tell "RT_VER_NUM undefined" from "comparison false", so a
    # pre-4.0 tree falls back to "leave the kernel alone" (documented in help);
    # such a tree must force USE_PKG or enable the package in its own menu.
    ("pre-4.0 tree (no RT_VER_NUM): documented limit", None, 4, False, False, "AUTO", "y", "n"),
    ("pre-4.0 tree, explicit USE_PKG: still unavailable", None, 4, False, False, "PKG", "y", "n"),
    ("MQTT tool disabled", "0x50300", 5, False, True, "AUTO", "n", "n"),
]

fail = 0
print("%-42s %-6s %-6s %s" % ("scenario", "pkg", "want", "notes"))
for label, ver, major, libc, standard, mode, mqtt, want in SCENARIOS:
    lines = ["CONFIG_PKG_USING_AGENT=y"]
    if ver is not None:
        lines.append("CONFIG_RT_VER_NUM=%s" % ver)
    lines.append("CONFIG_PKG_AGENT_RT_VERSION_MAJOR=%d" % major)
    lines.append("CONFIG_RT_KLIBC_USING_LIBC_VSNPRINTF=%s" % ("y" if libc else "n"))
    lines.append("CONFIG_RT_KLIBC_USING_VSNPRINTF_STANDARD=%s" % ("y" if standard else "n"))
    lines.append("CONFIG_PKG_AGENT_VSNPRINTF_AUTO=%s" % ("y" if mode == "AUTO" else "n"))
    lines.append("CONFIG_PKG_AGENT_VSNPRINTF_USE_KLIB=%s" % ("y" if mode == "KLIB" else "n"))
    lines.append("CONFIG_PKG_AGENT_VSNPRINTF_USE_PKG=%s" % ("y" if mode == "PKG" else "n"))
    lines.append("CONFIG_PKG_AGENT_TOOL_MQTT_ENABLE=%s" % mqtt)

    # ver is None -> emulate a tree without RT_VER_NUM at all, so the manual
    # PKG_AGENT_RT_VERSION_MAJOR fallback is exercised for real.
    stub = "stubs/Kconfig.deps" if ver is not None else "stubs/Kconfig.base"
    with open(os.path.join(HERE, "Kconfig.test"), "w", encoding="utf-8") as f:
        f.write('source "%s"\n\nsource "%s"\n' % (stub, target))

    path = os.path.join(HERE, ".config.scen")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    kconf = kconfiglib.Kconfig(os.path.join(HERE, "Kconfig.test"), warn_to_stderr=False)
    kconf.load_config(path)

    def val(name):
        sym = kconf.syms.get(name)
        return sym.str_value if sym else "<undefined>"

    full = val("PKG_USING_RT_VSNPRINTF_FULL")
    ok = (full == want)
    if not ok:
        fail += 1
    print("%-42s %-6s %-6s %s" % (label, full, want, "OK" if ok else "*** MISMATCH ***"))

print("\n%s" % ("ALL SCENARIOS PASSED" if not fail else "%d SCENARIO(S) FAILED" % fail))
sys.exit(1 if fail else 0)
