#!/usr/bin/env python3
"""生成 Agent 资源实测报告，或从评测矩阵生成网页基线。

collect   读 ELF、map、rtconfig.h 和可选的运行时 JSON，输出报告。
baseline  读评测矩阵与 M0 的 map，输出资源评估页加载的基线 JSON。

报告按白名单取值，不写 broker 地址、账号、密码、密钥或接口地址。
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

for stream in (sys.stdout, sys.stderr):
    if hasattr(stream, 'reconfigure'):
        stream.reconfigure(encoding='utf-8', errors='replace')

SCHEMA = 'agent-resource-report'
BASELINE_SCHEMA = 'agent-resource-baseline'
SCHEMA_VERSION = 1
GEN_VERSION = '1.0.0'
PKG_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BASELINE = PKG_ROOT / 'resource/web/client/public/agent-resource-baseline.json'

SECTION_SAME = re.compile(r'^\s+(\.\S+)\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)\s+(\S.*?)\s*$')
SECTION_HEAD = re.compile(r'^\s+(\.\S+)\s*$')
SECTION_ENTRY = re.compile(r'^\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)\s+(\S.*?)\s*$')
SIZE_LINE = re.compile(r'^\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+[0-9a-fA-F]+\s+(.+?)\s*$', re.M)
NUMBER_DEFINE = re.compile(r'^#define\s+(\w+)\s+(\d+)\s*$')

BOOL_DEFINES = {
    'memory': ('PKG_AGENT_MEMORY_ENABLE',),
    'memoryTools': ('PKG_AGENT_MEMORY_TOOLS_ENABLE',),
    'memoryPersist': ('PKG_AGENT_MEMORY_PERSIST',),
    'mqtt': ('PKG_AGENT_TOOL_MQTT_ENABLE',),
    'device': ('PKG_AGENT_TOOL_DEVICE',),
    'mcp': ('PKG_AGENT_TOOL_MCP_ENABLE',),
    'multimodal': ('PKG_AGENT_MULTIMODAL_ENABLE',),
    'stt': ('PKG_AGENT_STT_ENABLE',),
    'voice': ('PKG_AGENT_TTS_ENABLE',),
    'audio': ('PKG_AGENT_DRIVER_AUDIO',),
}

CHANNELS = ('CLI', 'WEBNET', 'DEBUG', 'LVGL', 'A2A')

NUMBER_DEFINES = {
    'memoryCoreItems': 'PKG_AGENT_MEMORY_CORE_ITEMS',
    'memoryRecallItems': 'PKG_AGENT_MEMORY_RECALL_ITEMS',
    'memoryItemBytes': 'PKG_AGENT_MEMORY_ITEM_BYTES',
    'memoryTopK': 'PKG_AGENT_MEMORY_TOPK',
    'memoryInjectBytes': 'PKG_AGENT_MEMORY_INJECT_BYTES',
    'fileOpStackBytes': 'PKG_AGENT_FILE_OP_THREAD_SIZE',
    'mqttRxStackBytes': 'PKG_AGENT_TOOL_MQTT_RX_THREAD_STACK',
    'mqttWorkerStackBytes': 'RT_PKG_MQTT_THREAD_STACK_SIZE',
}

NON_ALLOC_PREFIX = ('.debug', '.comment', '.ARM.attributes', '.stab', '.note.GNU-stack')

# baseline 阶段把 map 里的对象按功能分组，只有 collect 的包归属不受影响。
MODULE_RULES = (
    ('mqtt', ('/tool_mqtt/',)),
    ('device', ('/tool_device/',)),
    ('mcp', ('/tool_mcp',)),
    ('memory', ('/core/memory', '/tool_memory')),
    ('multimodal', ('/file_op', '/utils/stt', '/driver/audio', 'multimodal')),
)

# 评测矩阵里的线程名到资源模型里的栈键。
STACK_THREADS = {
    'loop': ('AgentLoop',),
    'a2a': ('agenta2a', 'a2a'),
    'fileOp': ('fileop_wk', 'fileOp'),
    'mqttRx': ('mqtt_rx',),
    'mqttWorker': ('mqtt0',),
    'playback': ('playback',),
    'record': ('record',),
}

STACK_FALLBACK = {'playback': 8192, 'record': 1024}

# 矩阵变体之间的差值，用来还原每个功能模块的整机 Flash 增量。
FLASH_DELTAS = {
    'mqtt': ('M0_mem_on', 'D1_no_mqtt'),
    'mcp': ('M0_mem_on', 'D2_no_mcp'),
    'device': ('M0_mem_on', 'D3_no_device'),
    'multimodal': ('M0_mem_on', 'D4_no_mm'),
    'memory': ('M0_mem_on', 'M1_mem_off'),
    'memoryPersist': ('M2_mem_persist', 'M0_mem_on'),
}


def fail(message):
    print('错误: ' + message, file=sys.stderr)
    sys.exit(1)


def now_iso():
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace('+00:00', 'Z')


def is_alloc_section(name):
    return not name.startswith(NON_ALLOC_PREFIX)


def is_flash_section(name):
    if not is_alloc_section(name):
        return False
    if name.startswith(('.bss', '.noinit', '.stack', '.heap')):
        return False
    return True


def is_ram_section(name):
    return name.startswith(('.data', '.bss', '.noinit', '.stack', '.heap'))


def section_group(name):
    for prefix, group in (('.text', 'text'), ('.rodata', 'rodata'), ('.data', 'data'),
                          ('.bss', 'bss'), ('.noinit', 'bss'), ('.ARM.exidx', 'exidx'),
                          ('.ARM.extab', 'extab')):
        if name.startswith(prefix):
            return group
    return 'other'


def find_size_tool(explicit):
    if explicit:
        return explicit
    env = os.environ.get('RTT_EXEC_PATH')
    names = ('arm-none-eabi-size.exe', 'arm-none-eabi-size') if os.name == 'nt' else ('arm-none-eabi-size',)
    if env:
        for name in names:
            candidate = Path(env) / name
            if candidate.exists():
                return str(candidate)
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    return None


def read_size(elf, size_tool):
    tool = find_size_tool(size_tool)
    if not tool:
        fail('找不到 arm-none-eabi-size，用 --size 指定路径')
    result = subprocess.run([tool, str(elf)], capture_output=True, text=True, encoding='utf-8', errors='replace')
    if result.returncode != 0:
        fail('arm-none-eabi-size 执行失败\n' + result.stderr.strip())
    matches = SIZE_LINE.findall(result.stdout)
    if not matches:
        fail('没解析到 size 输出\n' + result.stdout.strip())
    text, data, bss, dec, _name = matches[-1]
    return {'text': int(text), 'data': int(data), 'bss': int(bss), 'dec': int(dec)}


def read_map(map_path, marker):
    objects = {}
    sections = {}
    current = None
    for raw in Path(map_path).read_text(encoding='utf-8', errors='replace').splitlines():
        head = SECTION_HEAD.match(raw)
        if head:
            current = head.group(1)
            continue
        entry = SECTION_SAME.match(raw)
        if entry:
            section, size, path = entry.group(1), int(entry.group(2), 16), entry.group(3)
        else:
            entry = SECTION_ENTRY.match(raw)
            if not entry or current is None:
                continue
            section, size, path = current, int(entry.group(1), 16), entry.group(2)
        current = None
        if size == 0 or marker not in path:
            continue
        objects.setdefault(path, 0)
        objects[path] += size
        sections[section] = sections.get(section, 0) + size

    groups = {}
    flash = ram = 0
    for section, size in sections.items():
        if not is_alloc_section(section):
            continue
        groups[section_group(section)] = groups.get(section_group(section), 0) + size
        if is_flash_section(section):
            flash += size
        if is_ram_section(section):
            ram += size
    return {
        'objects': len(objects),
        'groups': groups,
        'flash': flash,
        'ram': ram,
        'by_object': objects,
    }


def read_config(header):
    defines = set()
    numbers = {}
    for line in Path(header).read_text(encoding='utf-8', errors='replace').splitlines():
        line = line.strip()
        if not line.startswith('#define'):
            continue
        parts = line.split()
        if len(parts) >= 2:
            defines.add(parts[1])
        match = NUMBER_DEFINE.match(line)
        if match:
            numbers[match.group(1)] = int(match.group(2))

    channel = 'unknown'
    for name in CHANNELS:
        if 'PKG_AGENT_%s_CHANNEL' % name in defines:
            channel = name
            break

    modules = {}
    for key, names in BOOL_DEFINES.items():
        modules[key] = all(name in defines for name in names)

    config_numbers = {}
    for key, name in NUMBER_DEFINES.items():
        if name in numbers:
            config_numbers[key] = numbers[name]
    return {'channel': channel, 'modules': modules, 'numbers': config_numbers}


def read_runtime(path):
    if not path:
        return None
    data = json.loads(Path(path).read_text(encoding='utf-8'))
    if isinstance(data, dict) and 'runtime' in data:
        data = data['runtime']
    stacks = []
    for entry in (data or {}).get('stacks') or (data or {}).get('thread_stacks') or []:
        if isinstance(entry, dict):
            name = entry.get('name')
            size = entry.get('bytes')
            watermark = entry.get('watermark')
        elif isinstance(entry, (list, tuple)) and len(entry) >= 2:
            name, size = entry[0], entry[1]
            watermark = entry[2] if len(entry) > 2 else None
        else:
            continue
        if not name or not isinstance(size, (int, float)):
            continue
        if watermark is not None:
            watermark = float(watermark)
            if watermark > 1:
                watermark = watermark / 100.0
        stacks.append({'name': str(name), 'bytes': int(size), 'watermark': watermark})
    return {'source': Path(path).name, 'stacks': stacks}


def collect(args):
    elf = Path(args.elf)
    if not elf.exists():
        fail('ELF 不存在: ' + str(elf))
    size = read_size(elf, args.size)
    image = elf.with_suffix('.bin')
    firmware = dict(size)
    firmware['bin'] = image.stat().st_size if image.exists() else None
    firmware['flashLoad'] = size['text'] + size['data']
    firmware['staticRam'] = size['data'] + size['bss']

    report = {
        'schema': SCHEMA,
        'schemaVersion': SCHEMA_VERSION,
        'generatedAt': now_iso(),
        'generator': {'name': 'resource_report.py', 'version': GEN_VERSION},
        'firmware': {
            'elf': elf.name,
            'map': Path(args.map).name if args.map else None,
            **firmware,
        },
        'config': read_config(args.config),
        'runtime': read_runtime(args.runtime),
        'notes': [
            'Firmware 来自 arm-none-eabi-size，flashLoad 为 text+data，staticRam 为 data+bss。',
            'Agent 归属只统计 map 中路径含 %s 的对象，不含 paho、lwIP 等外部依赖。' % args.package,
            'config 只保留白名单里的开关与数值，不含任何字符串配置。',
        ],
    }

    if args.map:
        attribution = read_map(args.map, args.package)
        by_object = attribution.pop('by_object')
        report['agent'] = {'package': args.package, **attribution}
        del by_object

    out = Path(args.out) if args.out else Path.cwd() / 'agent-resource-report.json'
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, ensure_ascii=False, indent=1) + '\n', encoding='utf-8')
    print('已生成报告 %s' % out)
    print('整机 Flash %d / RAM %d，包内 Flash %s / RAM %s' % (
        firmware['flashLoad'], firmware['staticRam'],
        report.get('agent', {}).get('flash', '未统计'), report.get('agent', {}).get('ram', '未统计')))


def categorize(path):
    normalized = path.replace('\\', '/')
    for key, needles in MODULE_RULES:
        if any(needle in normalized for needle in needles):
            return key
    return 'fixed'


def matrix_flash_delta(matrix, on, off):
    return matrix[on]['size']['text'] - matrix[off]['size']['text']


def matrix_ram_delta(matrix, on, off):
    def static_ram(name):
        return matrix[name]['size']['data'] + matrix[name]['size']['bss']
    return static_ram(on) - static_ram(off)


def baseline(args):
    matrix_path = Path(args.matrix)
    if not matrix_path.exists():
        fail('评测矩阵不存在: ' + str(matrix_path))
    matrix = json.loads(matrix_path.read_text(encoding='utf-8'))
    for name in ('M0_mem_on', 'M1_mem_off', 'M2_mem_persist', 'D1_no_mqtt', 'D2_no_mcp',
                 'D3_no_device', 'D4_no_mm'):
        if name not in matrix:
            fail('评测矩阵缺少变体 ' + name)

    if not args.map:
        fail('baseline 需要 --map，用 M0 变体的 map 拆分包内固定体积与模块 RAM')
    # 按段归属，避免把 .text 体积算进 RAM 或把 .bss 算进 Flash。
    attribution = read_map(args.map, args.package)
    buckets = {'fixed': {'flash': 0, 'ram': 0}}
    current = None
    for raw in Path(args.map).read_text(encoding='utf-8', errors='replace').splitlines():
        head = SECTION_HEAD.match(raw)
        if head:
            current = head.group(1)
            continue
        entry = SECTION_SAME.match(raw)
        if entry:
            section, size, path = entry.group(1), int(entry.group(2), 16), entry.group(3)
        else:
            entry = SECTION_ENTRY.match(raw)
            if not entry or current is None:
                continue
            section, size, path = current, int(entry.group(1), 16), entry.group(2)
        current = None
        if size == 0 or args.package not in path or not is_alloc_section(section):
            continue
        bucket = buckets.setdefault(categorize(path), {'flash': 0, 'ram': 0})
        if is_flash_section(section):
            bucket['flash'] += size
        if is_ram_section(section):
            bucket['ram'] += size

    fixed_flash = buckets.get('fixed', {}).get('flash', 0)
    ram_modules = {key: buckets[key]['ram'] for key in ('memory', 'mqtt', 'device', 'mcp', 'multimodal')
                   if key in buckets}
    ram_modules['memoryPersist'] = 0

    flash_modules = {}
    for key, (on, off) in FLASH_DELTAS.items():
        flash_modules[key] = matrix_flash_delta(matrix, on, off)
    flash_modules['voice'] = 2200

    stacks = {}
    for key, threads in STACK_THREADS.items():
        best = None
        for variant, payload in matrix.items():
            runtime = payload.get('runtime') or {}
            for entry in runtime.get('thread_stacks', []):
                name, size = entry[0], int(entry[1])
                watermark = entry[2] if len(entry) > 2 else None
                if name in threads and (best is None or size > best['bytes']):
                    best = {
                        'bytes': size,
                        'watermark': (float(watermark) / 100.0) if watermark is not None else None,
                        'thread': name,
                        'variant': variant,
                        'estimated': False,
                    }
        if best is None:
            best = {'bytes': STACK_FALLBACK.get(key, 0), 'watermark': None,
                    'thread': None, 'variant': None, 'estimated': True}
        stacks[key] = best

    m0 = matrix['M0_mem_on']
    version = args.version or datetime.now().date().isoformat()
    baseline = {
        'schema': BASELINE_SCHEMA,
        'schemaVersion': SCHEMA_VERSION,
        'generatedAt': now_iso(),
        'model': {
            'version': version,
            'source': 'build/memeval/matrix.json 的 M0_mem_on 变体与 map_M0.map',
            'defaultVariant': 'M0_mem_on',
            'baseline': 'MQTT、设备工具、MCP、多模态、STT、音频驱动与分层记忆全开，记忆落盘关，A2A 通道',
        },
        'firmware': {
            'variant': 'M0_mem_on',
            'channel': 'A2A',
            'size': m0['size'],
            'flashLoad': m0['size']['text'] + m0['size']['data'],
            'staticRam': m0['size']['data'] + m0['size']['bss'],
            'agentFlash': attribution['flash'],
            'agentRam': attribution['ram'],
        },
        'flash': {'fixed': fixed_flash, 'modules': flash_modules},
        'ram': {'fixedStatic': buckets['fixed']['ram'], 'modules': ram_modules},
        'stacks': stacks,
    }

    out = Path(args.out) if args.out else DEFAULT_BASELINE
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(baseline, ensure_ascii=False, indent=1) + '\n', encoding='utf-8')
    print('已生成基线 %s' % out)
    print('固定 Flash %d，模块 Flash %s' % (fixed_flash, flash_modules))
    print('固定 RAM %d，模块 RAM %s' % (buckets['fixed']['ram'], ram_modules))


def main():
    parser = argparse.ArgumentParser(description='Agent 资源报告与网页基线生成')
    sub = parser.add_subparsers(dest='command', required=True)

    collect_parser = sub.add_parser('collect', help='从 ELF/map/配置生成实测报告')
    collect_parser.add_argument('--elf', required=True)
    collect_parser.add_argument('--map')
    collect_parser.add_argument('--config', required=True)
    collect_parser.add_argument('--runtime', help='运行时 JSON，含 stacks 数组')
    collect_parser.add_argument('--size', help='arm-none-eabi-size 路径')
    collect_parser.add_argument('--package', default='Agent-latest', help='map 里用于归属的包路径关键字')
    collect_parser.add_argument('-o', '--out')
    collect_parser.set_defaults(func=collect)

    baseline_parser = sub.add_parser('baseline', help='从评测矩阵生成网页基线')
    baseline_parser.add_argument('--matrix', required=True)
    baseline_parser.add_argument('--map', required=True)
    baseline_parser.add_argument('--package', default='Agent-latest')
    baseline_parser.add_argument('--version', help='基线版本号，默认取当天日期')
    baseline_parser.add_argument('-o', '--out')
    baseline_parser.set_defaults(func=baseline)

    args = parser.parse_args()
    args.func(args)


if __name__ == '__main__':
    main()
