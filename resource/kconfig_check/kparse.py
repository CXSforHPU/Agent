"""Parse-check a Kconfig file with kconfiglib and report errors/warnings.

Usage:
    python kparse.py Agent.Kconfig.current      # parse + list warnings
    python kparse.py --values Agent.Kconfig     # also dump PKG_AGENT* values
"""

import os
import sys

sys.path.insert(0, r"D:\rtt\env-windows\.venv\Lib\site-packages")
import kconfiglib

HERE = os.path.dirname(os.path.abspath(__file__))
os.chdir(HERE)
os.environ["srctree"] = HERE

args = [a for a in sys.argv[1:] if not a.startswith("--")]
dump = "--values" in sys.argv
target = args[0] if args else "Agent.Kconfig.current"

with open(os.path.join(HERE, "Kconfig.test"), "w", encoding="utf-8") as f:
    f.write('source "stubs/Kconfig.deps"\n\nsource "%s"\n' % target)

print("=== parsing %s ===" % target)
try:
    kconf = kconfiglib.Kconfig(os.path.join(HERE, "Kconfig.test"), warn_to_stderr=False)
except kconfiglib.KconfigError as exc:
    print("PARSE ERROR:\n%s" % exc)
    sys.exit(1)

print("parsed OK, warnings: %d" % len(kconf.warnings))
for w in kconf.warnings:
    print("   %s" % w)

if dump:
    print("--- values ---")
    for name in sorted(kconf.syms):
        if name.startswith("PKG_AGENT") or name == "PKG_USING_RT_VSNPRINTF_FULL" or name == "PKG_USING_PAHOMQTT":
            print("%-46s = %s" % (name, kconf.syms[name].str_value))
