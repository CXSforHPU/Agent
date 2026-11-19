import { STACKS } from './resourceModel.js'

export const REPORT_SCHEMA = 'agent-resource-report'
export const BASELINE_SCHEMA = 'agent-resource-baseline'
export const SCHEMA_VERSION = 1

const THREAD_KEYS = {
  loop: ['AgentLoop'],
  a2a: ['agenta2a', 'a2a'],
  fileOp: ['fileop_wk', 'fileOp'],
  mqttRx: ['mqtt_rx'],
  mqttWorker: ['mqtt0'],
  playback: ['playback'],
  record: ['record'],
}

const MODULE_LABELS = {
  memory: '分层记忆',
  memoryTools: '记忆工具',
  memoryPersist: '记忆落盘',
  mqtt: 'MQTT',
  device: '设备工具',
  mcp: 'MCP',
  multimodal: '多模态',
  stt: 'STT',
  voice: '语音播报',
  audio: '音频驱动',
}

export async function loadBaseline(url = '/agent-resource-baseline.json') {
  try {
    const response = await fetch(url, { cache: 'no-store' })
    if (!response.ok) return null
    const data = await response.json()
    if (data?.schema !== BASELINE_SCHEMA || data.schemaVersion !== SCHEMA_VERSION) return null
    return data
  } catch {
    return null
  }
}

export function parseReport(raw) {
  let data
  try {
    data = typeof raw === 'string' ? JSON.parse(raw) : raw
  } catch {
    return { error: '文件不是合法的 JSON' }
  }
  if (!data || data.schema !== REPORT_SCHEMA || data.schemaVersion !== SCHEMA_VERSION) {
    return { error: '不是资源实测报告，schema 或版本不对' }
  }
  if (!data.firmware || !Number.isFinite(data.firmware.text) || !Number.isFinite(data.firmware.data)) {
    return { error: '报告缺少 firmware.text 或 firmware.data' }
  }
  return { report: data }
}

export function reportWhole(report) {
  const firmware = report?.firmware || {}
  const text = Number(firmware.text) || 0
  const data = Number(firmware.data) || 0
  const bss = Number(firmware.bss) || 0
  return {
    flash: Number(firmware.flashLoad) || text + data,
    ram: Number(firmware.staticRam) || data + bss,
    text,
    data,
    bss,
    bin: Number(firmware.bin) || null,
  }
}

function normalizeWatermark(value) {
  if (value === null || value === undefined) return null
  const number = Number(value)
  if (!Number.isFinite(number) || number < 0) return null
  return number > 1 ? number / 100 : number
}

export function measurementsByKey(stacks) {
  const result = {}
  for (const entry of stacks || []) {
    if (!entry?.name) continue
    const key = Object.keys(THREAD_KEYS).find((name) => THREAD_KEYS[name].includes(entry.name))
    if (!key || result[key]) continue
    result[key] = {
      name: entry.name,
      bytes: Number(entry.bytes) || 0,
      watermark: normalizeWatermark(entry.watermark),
    }
  }
  return result
}

export function reportMeasurements(report) {
  return measurementsByKey(report?.runtime?.stacks || [])
}

export function baselineMeasurements(baseline) {
  const result = {}
  for (const [key, entry] of Object.entries(baseline?.stacks || {})) {
    result[key] = {
      name: entry.thread || key,
      bytes: Number(entry.bytes) || 0,
      watermark: normalizeWatermark(entry.watermark),
      estimated: Boolean(entry.estimated),
      variant: entry.variant || null,
    }
  }
  return result
}

export function reportTrust(report, form) {
  if (!report) {
    return {
      level: 'reference',
      label: '参考估算',
      type: 'default',
      hint: '没有实测报告，数字来自基线或内置常量。',
    }
  }

  const modules = report.config?.modules || {}
  const expected = {
    memory: Boolean(form.modules.memory),
    memoryTools: Boolean(form.modules.memory),
    memoryPersist: Boolean(form.modules.memory && form.modules.memoryPersist),
    mqtt: Boolean(form.modules.mqtt),
    device: Boolean(form.modules.device),
    mcp: Boolean(form.modules.mcp),
    multimodal: Boolean(form.modules.multimodal),
    stt: Boolean(form.modules.multimodal),
    voice: Boolean(form.modules.voice),
  }
  const mismatched = Object.keys(expected)
    .filter((key) => Boolean(modules[key]) !== expected[key])
    .map((key) => MODULE_LABELS[key] || key)
  // 音频驱动随固件编进去但没人引用时会被链接器丢掉，关掉语音后不算配置差异。
  if (form.modules.voice && !modules.audio) mismatched.push(MODULE_LABELS.audio)

  const numbers = report.config?.numbers || {}
  const stackChecks = [
    [numbers.fileOpStackBytes, 'fileOp', form.modules.multimodal],
    [numbers.mqttRxStackBytes, 'mqttRx', form.modules.mqtt],
    [numbers.mqttWorkerStackBytes, 'mqttWorker', form.modules.mqtt],
  ]
  for (const [measured, key, active] of stackChecks) {
    if (!active || !Number.isFinite(measured)) continue
    if (Math.round((Number(form.stacks?.[key]) || 0) * 1024) !== measured) mismatched.push(key)
  }
  const runtime = reportMeasurements(report)
  for (const key of ['loop', 'a2a']) {
    if (!runtime[key]) continue
    if (Math.round((Number(form.stacks?.[key]) || 0) * 1024) !== runtime[key].bytes) mismatched.push(key)
  }

  if (!mismatched.length) {
    return {
      level: 'measured',
      label: '当前配置实测',
      type: 'success',
      hint: '报告的模块开关与栈设置和当前方案一致，可直接对照。',
    }
  }
  return {
    level: 'estimated',
    label: '同环境估算',
    type: 'warning',
    hint: `报告配置与当前方案不同（${[...new Set(mismatched)].join('、')}），数字按当前方案估算。`,
  }
}

export function calibration(report, calc) {
  if (!report) return { ok: false, message: '先导入实测报告' }
  const whole = reportWhole(report)
  const agentFlash = calc.flash.rows
    .filter((row) => row.key !== 'existing-flash')
    .reduce((sum, row) => sum + (Number(row.bytes) || 0), 0)
  const agentRam = calc.ram.rows
    .filter((row) => row.key !== 'existing-ram')
    .reduce((sum, row) => sum + (Number(row.bytes) || 0), 0)
  const flash = whole.flash - agentFlash
  const ram = whole.ram - agentRam
  if (flash < 0 || ram < 0) {
    return {
      ok: false,
      message: `报告整机体积小于当前方案估算（Flash ${Math.round(flash / 1024)} KB，RAM ${Math.round(ram / 1024)} KB），先核对开关和依赖。`,
    }
  }
  return {
    ok: true,
    existingFlashKB: Math.round(flash / 1024),
    existingRamKB: Math.round(ram / 1024),
    flash,
    ram,
  }
}

export function stackRows(measurements, form, activeKeys) {
  return STACKS.filter((stack) => !stack.requires || activeKeys.has(stack.requires)).map((stack) => {
    const measured = measurements?.[stack.key]
    const budget = Math.round((Number(form.stacks?.[stack.key]) || stack.bytes / 1024) * 1024)
    const peak = measured && measured.watermark !== null
      ? Math.round(measured.bytes * measured.watermark)
      : null
    return {
      key: stack.key,
      name: stack.name,
      measured: Boolean(measured && measured.watermark !== null),
      source: measured?.variant || measured?.name || null,
      bytes: measured?.bytes || null,
      watermark: measured?.watermark ?? null,
      peak,
      budget,
      over: peak !== null && peak > budget,
    }
  })
}
