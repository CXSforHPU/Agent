import os, sys
sys.path.insert(0, r"D:\rtt\env-windows\.venv\Lib\site-packages")
import kconfiglib
HERE = r"C:\Users\31309\Desktop\agent_test\packages\Agent-latest\resource\kconfig_check"
os.chdir(HERE); os.environ["srctree"] = HERE
cfg = """CONFIG_PKG_USING_AGENT=y
CONFIG_PKG_AGENT_DRIVER_AUDIO=y
CONFIG_PKG_AGENT_TOOL_MQTT_ENABLE=y
CONFIG_PKG_AGENT_MULTIMODAL_DISABLE=y
"""
open(os.path.join(HERE,".config.audio"),"w").write(cfg)
for target in ("Agent.Kconfig", "Agent.Kconfig.current"):
    open(os.path.join(HERE,"Kconfig.test"),"w").write('source "stubs/Kconfig.deps"\n\nsource "%s"\n' % target)
    k = kconfiglib.Kconfig(os.path.join(HERE,"Kconfig.test"), warn_to_stderr=False)
    k.load_config(os.path.join(HERE,".config.audio"))
    def v(n):
        s = k.syms.get(n); return s.str_value if s else "<undefined>"
    print("%-24s audio=%s wavplayer=%s wav_record=%s rt_using_audio=%s bsp_using_audio=%s" % (
        target, v("PKG_AGENT_DRIVER_AUDIO"), v("PKG_USING_WAVPLAYER"), v("PKG_WP_USING_RECORD"),
        v("RT_USING_AUDIO"), v("BSP_USING_AUDIO")))
    print("   warnings:", k.warnings)
