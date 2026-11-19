const KB = 1024
const MB = 1024 * 1024
const GB = 1024 * MB

export const MODEL_INFO = {
  version: '2026-11-18',
  source: 'docs/resources.md 第 3～5 节',
  baseline: 'MQTT、设备工具、MCP、多模态、STT、音频驱动与分层记忆全开，记忆落盘关，两份协议都编译，随包 5 份技能',
}

// 资源评估页启动后加载 public/agent-resource-baseline.json，覆盖下面这些内置默认值。
export const BASELINE_STATE = {
  loaded: false,
  version: MODEL_INFO.version,
  source: MODEL_INFO.source,
  baseline: MODEL_INFO.baseline,
  generatedAt: null,
  variant: null,
  firmware: null,
  stacks: {},
}

// Agent 包内固定部分：核心、协议、通道、技能运行时、工具注册与 5 份技能。
export const FLASH_FIXED = { bytes: 109734 }

export const DEPS = [
  { key: 'tls', name: 'mbedTLS', bytes: 832169, note: 'HTTPS/TLS，Agent 包 select 引入' },
  { key: 'lwip', name: 'lwIP', bytes: 171285, note: 'TCP/IP 协议栈' },
  { key: 'newlib', name: 'newlib', bytes: 79433, note: 'C 运行库，用 klibc 的工程可以去掉' },
]

export const MODULES = [
  {
    key: 'memory',
    name: '分层记忆',
    kconfig: 'PKG_AGENT_MEMORY_ENABLE / PKG_AGENT_MEMORY_TOOLS_ENABLE',
    flash: 10784,
    ramStatic: 9024,
    note: '8 个核心槽与 32 个回忆槽，核心记忆每轮注入，回忆记忆按关键词取前 3 条',
  },
  {
    key: 'memoryPersist',
    name: '记忆落盘',
    kconfig: 'PKG_AGENT_MEMORY_PERSIST',
    flash: 1772,
    ramStatic: 0,
    depends: ['memory'],
    needsFs: true,
    note: '启动时读回、停止时写回；需要可写文件系统，romfs 不行',
  },
  {
    key: 'mqtt',
    name: 'MQTT 工具',
    kconfig: 'PKG_AGENT_TOOL_MQTT_ENABLE',
    flash: 81544,
    ramStatic: 16672,
    stacks: ['mqttRx', 'mqttWorker'],
    note: '含 paho-mqtt、接收线程与 paho 工作线程',
  },
  {
    key: 'device',
    name: '设备框架工具',
    kconfig: 'PKG_AGENT_TOOL_DEVICE',
    flash: 44156,
    ramStatic: 0,
    note: '通用层加 PIN/PWM/ADC/DAC/I2C/SPI/sensor/LED 各类模块',
  },
  {
    key: 'mcp',
    name: 'MCP 客户端',
    kconfig: 'PKG_AGENT_TOOL_MCP_ENABLE',
    flash: 8400,
    ramStatic: 0,
    note: '连接主机侧 MCP 网关，把远端工具注册进工具表',
  },
  {
    key: 'multimodal',
    name: '多模态与 STT',
    kconfig: 'PKG_AGENT_MULTIMODAL_ENABLE / PKG_AGENT_STT_ENABLE',
    flash: 11028,
    ramStatic: 0,
    stacks: ['fileOp'],
    note: '图片与文件收发、语音转写',
  },
  {
    key: 'voice',
    name: '语音播报与音频',
    kconfig: 'PKG_AGENT_TTS_AUTO_PLAY / PKG_AGENT_DRIVER_AUDIO',
    flash: 2200,
    ramStatic: 0,
    stacks: ['playback', 'record'],
    depends: ['multimodal'],
    estimated: true,
    note: '打开播报后音频驱动才被引用；播报与录音线程随之创建',
  },
]

export const TRIMS = [
  {
    key: 'skillsOne',
    name: '技能只保留 1 份',
    kconfig: 'PKG_AGENT_SKILL_DIR',
    flash: -16000,
    estimated: true,
    note: '默认随包 5 份技能占 20,527 字节；把目录指向只放 1 份的文件夹',
  },
  {
    key: 'singleProtocol',
    name: '只编译一种协议',
    kconfig: '修改 SConscript',
    flash: -7952,
    estimated: true,
    note: 'Kconfig 没有单协议开关，要自己改协议源文件列表',
  },
]

// 线程栈默认值来自实测配置，可在评估页按需调高。
export const STACKS = [
  {
    key: 'loop',
    name: 'AgentLoop 上下文栈',
    bytes: 10240,
    minKB: 4,
    maxKB: 64,
    source: 'AgentLoop.c:226',
    note: '主循环与上下文处理，chat() 的 1 KB 行缓冲也在这条栈上；离线回归峰值 3.3～3.8 KB，长上下文或更大响应可按需加大',
  },
  {
    key: 'a2a',
    name: 'A2A 工作线程栈',
    bytes: 8192,
    minKB: 4,
    maxKB: 32,
    source: 'AgentA2aChannel.c:620',
    note: '控制台 WebSocket 连接与自发上报',
  },
  {
    key: 'fileOp',
    name: '文件操作线程栈',
    bytes: 4096,
    minKB: 2,
    maxKB: 16,
    requires: 'multimodal',
    source: 'Kconfig',
    kconfig: 'CONFIG_PKG_AGENT_FILE_OP_THREAD_SIZE',
    note: '音视频与文件收发线程',
  },
  {
    key: 'playback',
    name: '播报线程栈',
    bytes: 8192,
    minKB: 2,
    maxKB: 32,
    requires: 'voice',
    source: 'tts.c',
    note: '打开语音播报后才创建',
  },
  {
    key: 'record',
    name: '录音线程栈',
    bytes: 1024,
    minKB: 1,
    maxKB: 8,
    requires: 'voice',
    source: '源码常量',
    note: '打开语音播报后才创建',
  },
  {
    key: 'mqttRx',
    name: 'MQTT 接收线程栈',
    bytes: 3072,
    minKB: 2,
    maxKB: 16,
    requires: 'mqtt',
    source: 'Kconfig',
    kconfig: 'CONFIG_PKG_AGENT_TOOL_MQTT_RX_THREAD_STACK',
    note: '收消息与分发订阅',
  },
  {
    key: 'mqttWorker',
    name: 'MQTT 工作线程栈',
    bytes: 8192,
    minKB: 8,
    maxKB: 32,
    requires: 'mqtt',
    source: 'paho',
    kconfig: 'RT_PKG_MQTT_THREAD_STACK_SIZE',
    note: 'paho 工作线程，低于 8 KB 时打开 DFS 路径会溢出',
  },
]

// 基线 35,250 字节减去记忆 9,024 与 MQTT 16,672，按包内静态表估算。
export const RAM_FIXED = {
  staticBytes: 9554,
  staticNote: '基线 35,250 字节减去记忆 9,024 与 MQTT 16,672',
  heapBytes: 18432,
  heapNote: '空闲堆 12 KB 加一轮带工具调用约 5.6 KB',
}

export function applyBaseline(data) {
  if (!data || data.schema !== 'agent-resource-baseline' || data.schemaVersion !== 1) return false
  const model = data.model || {}
  const flash = data.flash || {}
  const ram = data.ram || {}

  if (Number.isFinite(flash.fixed)) FLASH_FIXED.bytes = flash.fixed
  for (const module of MODULES) {
    const flashBytes = flash.modules?.[module.key]
    if (Number.isFinite(flashBytes)) module.flash = flashBytes
    const ramBytes = ram.modules?.[module.key]
    if (Number.isFinite(ramBytes)) module.ramStatic = ramBytes
  }
  if (Number.isFinite(ram.fixedStatic)) RAM_FIXED.staticBytes = ram.fixedStatic

  for (const stack of STACKS) {
    const measured = data.stacks?.[stack.key]
    if (!measured || !Number.isFinite(measured.bytes) || measured.bytes <= 0) continue
    stack.bytes = measured.bytes
    stack.watermark = measured.watermark ?? null
    stack.thread = measured.thread || null
    stack.variant = measured.variant || null
    stack.measured = !measured.estimated
  }

  Object.assign(MODEL_INFO, {
    version: model.version || MODEL_INFO.version,
    source: model.source || MODEL_INFO.source,
    baseline: model.baseline || MODEL_INFO.baseline,
  })
  Object.assign(BASELINE_STATE, {
    loaded: true,
    version: MODEL_INFO.version,
    source: MODEL_INFO.source,
    baseline: MODEL_INFO.baseline,
    generatedAt: data.generatedAt || null,
    variant: model.defaultVariant || null,
    firmware: data.firmware || null,
    stacks: data.stacks || {},
  })
  return true
}

export function defaultStacks() {
  return Object.fromEntries(STACKS.map((stack) => [stack.key, stack.bytes / KB]))
}

export function stackBytes(state, stack) {
  const raw = Number(state.stacks?.[stack.key])
  const kb = Number.isFinite(raw) && raw > 0 ? raw : stack.bytes / KB
  return kb * KB
}

export function moduleRam(module, state) {
  const stackSum = (module.stacks || []).reduce((sum, key) => {
    const stack = STACKS.find((item) => item.key === key)
    return stack ? sum + stackBytes(state, stack) : sum
  }, 0)
  return module.ramStatic + stackSum
}

export const PRESETS = [
  {
    key: 'full',
    name: '完整功能',
    desc: '全部模块打开，含记忆落盘与语音播报',
    modules: { memory: true, memoryPersist: true, mqtt: true, device: true, mcp: true, multimodal: true, voice: true },
    trims: {},
  },
  {
    key: 'device',
    name: '设备控制',
    desc: '记忆、MQTT 与设备工具，关掉 MCP、多模态与语音',
    modules: { memory: true, mqtt: true, device: true },
    trims: {},
  },
  {
    key: 'memoryLite',
    name: '记忆增强',
    desc: '只加分层记忆，其余模块关闭',
    modules: { memory: true },
    trims: {},
  },
  {
    key: 'minimal',
    name: '最小对话',
    desc: '只保留对话与内置技能工具，技能与协议各留一份',
    modules: {},
    trims: { skillsOne: true, singleProtocol: true },
  },
]

export const MEMORY_SLOT_COUNT = 40
export const MEMORY_SLOT_BYTES = 220

export function defaultModules() {
  return Object.fromEntries(MODULES.map((m) => [m.key, false]))
}

export function defaultTrims() {
  return Object.fromEntries(TRIMS.map((t) => [t.key, false]))
}

export function formatBytes(bytes, digits = 1) {
  const value = Number(bytes) || 0
  const abs = Math.abs(value)
  if (abs < KB) return `${Math.round(value)} B`
  if (abs < MB) return `${(value / KB).toFixed(digits)} KB`
  return `${(value / MB).toFixed(2)} MB`
}

export function formatNumber(bytes) {
  return new Intl.NumberFormat('zh-CN').format(Math.round(Number(bytes) || 0))
}

export function formatPercent(percent) {
  return `${(Number(percent) || 0).toFixed(1)}%`
}

function activeModules(state) {
  const modules = state.modules || {}
  return MODULES.filter((m) => modules[m.key] && (m.depends || []).every((dep) => modules[dep]))
}

function activeTrims(state) {
  const trims = state.trims || {}
  return TRIMS.filter((t) => trims[t.key])
}

export function calculate(state) {
  const existingDeps = new Set(state.existingDeps || [])
  const existingFlash = Math.max(0, Number(state.existingFlashKB) || 0) * KB
  const existingRam = Math.max(0, Number(state.existingRamKB) || 0) * KB
  const modules = activeModules(state)
  const trims = activeTrims(state)
  const persist = modules.some((m) => m.key === 'memoryPersist')

  const flashRows = [
    {
      key: 'base',
      name: 'Agent 固定部分',
      bytes: FLASH_FIXED.bytes,
      note: '核心、协议、通道、技能运行时、工具注册与 5 份技能',
    },
    ...DEPS.map((dep) => ({
      key: `dep-${dep.key}`,
      name: dep.name,
      bytes: existingDeps.has(dep.key) ? 0 : dep.bytes,
      note: existingDeps.has(dep.key) ? '已在现有工程中，计入下面的现有固件' : dep.note,
      skipped: existingDeps.has(dep.key),
    })),
    ...modules.map((m) => ({
      key: `mod-${m.key}`,
      name: m.name,
      bytes: m.flash,
      note: m.note,
      estimated: m.estimated,
    })),
    ...trims.map((t) => ({
      key: `trim-${t.key}`,
      name: t.name,
      bytes: t.flash,
      note: t.note,
      estimated: t.estimated,
    })),
    {
      key: 'existing-flash',
      name: '现有固件',
      bytes: existingFlash,
      note: '你填的片内 Flash 占用，不含本方案',
    },
  ]
  const flashTotal = Math.max(0, flashRows.reduce((sum, row) => sum + row.bytes, 0))

  const activeKeys = new Set(modules.map((m) => m.key))
  const stackRows = STACKS.filter((stack) => !stack.requires || activeKeys.has(stack.requires))
  const ramRows = [
    { key: 'static-fixed', name: 'Agent 固定静态 RAM', bytes: RAM_FIXED.staticBytes, note: RAM_FIXED.staticNote },
    ...stackRows.map((stack) => ({
      key: `stack-${stack.key}`,
      name: stack.name,
      bytes: stackBytes(state, stack),
      note: stack.note,
    })),
    { key: 'heap', name: '堆预留', bytes: RAM_FIXED.heapBytes, note: RAM_FIXED.heapNote },
  ]
  for (const m of modules) {
    if (m.ramStatic) {
      ramRows.push({ key: `ram-static-${m.key}`, name: `${m.name}静态表`, bytes: m.ramStatic, note: m.note })
    }
  }
  ramRows.push({
    key: 'existing-ram',
    name: '现有固件静态 RAM',
    bytes: existingRam,
    note: '你填的 RAM 占用，不含本方案',
  })
  const ramTotal = ramRows.reduce((sum, row) => sum + row.bytes, 0)

  const flashCapacity = Math.max(0, Number(state.chipFlashKB) || 0) * KB
  const ramCapacity = Math.max(0, Number(state.chipRamKB) || 0) * KB
  const flashMargin = Number(state.marginFlash) || 0
  const ramMargin = Number(state.marginRam) || 0
  const flashBudget = flashCapacity * (1 - flashMargin / 100)
  const ramBudget = ramCapacity * (1 - ramMargin / 100)
  const flashFit = flashCapacity > 0 && flashTotal <= flashBudget
  const ramFit = ramCapacity > 0 && ramTotal <= ramBudget
  const warnings = []

  if (!flashCapacity) warnings.push('请填写片内 Flash 容量')
  if (!ramCapacity) warnings.push('请填写片内 RAM 容量')
  if (flashCapacity && !flashFit) {
    warnings.push(`片内 Flash 超出推荐上限：还差 ${formatBytes(flashTotal - flashBudget)}`)
  }
  if (ramCapacity && !ramFit) {
    warnings.push(`片内 RAM 超出推荐上限：还差 ${formatBytes(ramTotal - ramBudget)}`)
  }
  if (persist && !Number(state.spiFlashMB) && !Number(state.sdCardGB)) {
    warnings.push('记忆落盘需要可写文件系统：填 SD 卡或 SPI Flash，并在板端挂载 elmfat/littlefs')
  }
  if (existingDeps.size && !existingFlash) {
    warnings.push('勾选了现有工程已包含的依赖，但现有固件 Flash 填的是 0，占用会被低估')
  }
  if (activeKeys.has('mqtt')) {
    const mqttWorker = STACKS.find((stack) => stack.key === 'mqttWorker')
    if (stackBytes(state, mqttWorker) < 8 * KB) {
      warnings.push('MQTT 工作线程栈低于 8 KB，paho 打开 DFS 路径会溢出')
    }
  }

  return {
    flash: { total: flashTotal, rows: flashRows },
    ram: { total: ramTotal, rows: ramRows },
    capacity: {
      flashCapacity,
      ramCapacity,
      flashBudget,
      ramBudget,
      flashPercent: flashCapacity ? (flashTotal / flashCapacity) * 100 : 0,
      ramPercent: ramCapacity ? (ramTotal / ramCapacity) * 100 : 0,
      flashFree: flashCapacity - flashTotal,
      ramFree: ramCapacity - ramTotal,
      flashFit,
      ramFit,
      fit: flashFit && ramFit,
    },
    warnings,
  }
}

export function recommend(state) {
  const hasWritableStorage = Number(state.spiFlashMB) > 0 || Number(state.sdCardGB) > 0
  const results = PRESETS.map((preset) => {
    const next = {
      ...state,
      modules: { ...defaultModules(), ...preset.modules },
      trims: { ...defaultTrims(), ...preset.trims },
    }
    const result = calculate(next)
    const capacityFit = result.capacity.fit
    const needsFs = Boolean(preset.modules.memoryPersist)
    const fit = capacityFit && (!needsFs || hasWritableStorage)
    const reason = capacityFit ? (fit ? '' : '缺文件系统') : '超限'
    return { ...preset, modules: next.modules, trims: next.trims, result, fit, capacityFit, reason }
  })
  const best = results.find((item) => item.fit) || results[results.length - 1]
  return { results, bestKey: best.key, hasFit: results.some((item) => item.fit) }
}

export function storageInfo(state, result) {
  const persistent = Boolean(state.modules?.memory && state.modules?.memoryPersist)
  const memoryFileBytes = persistent ? MEMORY_SLOT_COUNT * MEMORY_SLOT_BYTES : 0
  const imageBytes = Math.ceil(result.flash.total * 1.02)
  const spiBytes = Math.max(0, Number(state.spiFlashMB) || 0) * MB
  const sdBytes = Math.max(0, Number(state.sdCardGB) || 0) * GB

  return {
    persistent,
    memoryFileBytes,
    imageBytes,
    spi: spiBytes
      ? {
          bytes: spiBytes,
          otaSingle: spiBytes >= imageBytes,
          otaDouble: spiBytes >= imageBytes * 2,
          memory: memoryFileBytes === 0 || spiBytes >= memoryFileBytes,
          singlePercent: (imageBytes / spiBytes) * 100,
          doublePercent: ((imageBytes * 2) / spiBytes) * 100,
          memoryPercent: memoryFileBytes ? (memoryFileBytes / spiBytes) * 100 : 0,
        }
      : null,
    sd: sdBytes
      ? {
          bytes: sdBytes,
          memory: memoryFileBytes === 0 || sdBytes >= memoryFileBytes,
          recommended: sdBytes >= 32 * MB,
          memoryPercent: memoryFileBytes ? (memoryFileBytes / sdBytes) * 100 : 0,
        }
      : null,
  }
}
