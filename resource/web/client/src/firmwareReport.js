const REPORT_SCHEMA = 'agent-resource-report'

const SECTION_SAME = /^\s+(\.\S+)\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)\s+(\S.*?)\s*$/
const SECTION_HEAD = /^\s+(\.\S+)\s*$/
const SECTION_ENTRY = /^\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)\s+(\S.*?)\s*$/
const NUMBER_DEFINE = /^#define\s+(\w+)\s+(\d+)\s*$/

const NON_ALLOC_PREFIX = ['.debug', '.comment', '.ARM.attributes', '.stab', '.note.GNU-stack']

const BOOL_DEFINES = {
  memory: ['PKG_AGENT_MEMORY_ENABLE'],
  memoryTools: ['PKG_AGENT_MEMORY_TOOLS_ENABLE'],
  memoryPersist: ['PKG_AGENT_MEMORY_PERSIST'],
  mqtt: ['PKG_AGENT_TOOL_MQTT_ENABLE'],
  device: ['PKG_AGENT_TOOL_DEVICE'],
  mcp: ['PKG_AGENT_TOOL_MCP_ENABLE'],
  multimodal: ['PKG_AGENT_MULTIMODAL_ENABLE'],
  stt: ['PKG_AGENT_STT_ENABLE'],
  voice: ['PKG_AGENT_TTS_ENABLE'],
  audio: ['PKG_AGENT_DRIVER_AUDIO'],
}

const CHANNELS = ['CLI', 'WEBNET', 'DEBUG', 'LVGL', 'A2A']

const NUMBER_DEFINES = {
  memoryCoreItems: 'PKG_AGENT_MEMORY_CORE_ITEMS',
  memoryRecallItems: 'PKG_AGENT_MEMORY_RECALL_ITEMS',
  memoryItemBytes: 'PKG_AGENT_MEMORY_ITEM_BYTES',
  memoryTopK: 'PKG_AGENT_MEMORY_TOPK',
  memoryInjectBytes: 'PKG_AGENT_MEMORY_INJECT_BYTES',
  fileOpStackBytes: 'PKG_AGENT_FILE_OP_THREAD_SIZE',
  mqttRxStackBytes: 'PKG_AGENT_TOOL_MQTT_RX_THREAD_STACK',
  mqttWorkerStackBytes: 'RT_PKG_MQTT_THREAD_STACK_SIZE',
}

const SHF_WRITE = 0x1
const SHF_ALLOC = 0x2
const SHT_NOBITS = 8

function isAlloc(name) {
  return !NON_ALLOC_PREFIX.some((prefix) => name.startsWith(prefix))
}

function isFlashSection(name) {
  if (!isAlloc(name)) return false
  return !['.bss', '.noinit', '.stack', '.heap'].some((prefix) => name.startsWith(prefix))
}

function isRamSection(name) {
  return ['.data', '.bss', '.noinit', '.stack', '.heap'].some((prefix) => name.startsWith(prefix))
}

function sectionGroup(name) {
  const groups = [
    ['.text', 'text'],
    ['.rodata', 'rodata'],
    ['.data', 'data'],
    ['.bss', 'bss'],
    ['.noinit', 'bss'],
    ['.ARM.exidx', 'exidx'],
    ['.ARM.extab', 'extab'],
  ]
  for (const [prefix, group] of groups) {
    if (name.startsWith(prefix)) return group
  }
  return 'other'
}

export function parseElfSize(buffer) {
  const bytes = new Uint8Array(buffer)
  if (bytes.length < 52) throw new Error('ELF 文件不完整')
  if (bytes[0] !== 0x7f || bytes[1] !== 0x45 || bytes[2] !== 0x4c || bytes[3] !== 0x46) {
    throw new Error('不是 ELF 文件，选错了吧')
  }
  if (bytes[4] !== 1 || bytes[5] !== 1) throw new Error('只支持 32 位小端 ELF')

  const view = new DataView(buffer)
  const shoff = view.getUint32(0x20, true)
  const shentsize = view.getUint16(0x2e, true)
  const shnum = view.getUint16(0x30, true)
  const shstrndx = view.getUint16(0x32, true)
  if (!shoff || !shnum) throw new Error('ELF 没有节表，可能被 strip 过')
  if (shentsize < 40) throw new Error('ELF 节表格式不认识')
  if (shoff + shnum * shentsize > bytes.length) throw new Error('ELF 节表超出文件范围')
  if (shstrndx >= shnum) throw new Error('ELF 缺少节名字符串表')

  const readName = (index) => {
    const base = shoff + shstrndx * shentsize
    const offset = view.getUint32(base + 16, true) + view.getUint32(index, true)
    let name = ''
    while (offset + name.length < bytes.length && bytes[offset + name.length] !== 0) {
      name += String.fromCharCode(bytes[offset + name.length])
    }
    return name
  }

  let text = 0
  let data = 0
  let bss = 0
  for (let i = 0; i < shnum; i += 1) {
    const base = shoff + i * shentsize
    const name = readName(base)
    const type = view.getUint32(base + 4, true)
    const flags = view.getUint32(base + 8, true)
    const size = view.getUint32(base + 20, true)
    if (!name || !size || !(flags & SHF_ALLOC)) continue
    if (type === SHT_NOBITS) bss += size
    else if (flags & SHF_WRITE) data += size
    else text += size
  }
  if (!text && !data && !bss) throw new Error('ELF 里没有可统计的段')
  return { text, data, bss, dec: text + data + bss }
}

export function parseMap(text, packageName, onProgress) {
  const objects = new Map()
  const sections = new Map()
  const lines = String(text).split(/\r?\n/)
  let current = null
  for (let index = 0; index < lines.length; index += 1) {
    const raw = lines[index]
    if (onProgress && index % 4096 === 0) onProgress(index / lines.length)
    const head = SECTION_HEAD.exec(raw)
    if (head) {
      current = head[1]
      continue
    }
    const same = SECTION_SAME.exec(raw)
    let section
    let size
    let path
    if (same) {
      section = same[1]
      size = parseInt(same[2], 16)
      path = same[3]
    } else {
      const entry = SECTION_ENTRY.exec(raw)
      if (!entry || current === null) continue
      section = current
      size = parseInt(entry[1], 16)
      path = entry[2]
    }
    current = null
    if (!size || !path.includes(packageName)) continue
    objects.set(path, (objects.get(path) || 0) + size)
    sections.set(section, (sections.get(section) || 0) + size)
  }
  if (onProgress) onProgress(1)

  const groups = {}
  let flash = 0
  let ram = 0
  for (const [section, size] of sections) {
    if (!isAlloc(section)) continue
    const group = sectionGroup(section)
    groups[group] = (groups[group] || 0) + size
    if (isFlashSection(section)) flash += size
    if (isRamSection(section)) ram += size
  }
  return { objects: objects.size, groups, flash, ram }
}

export function parseConfig(text) {
  const defines = new Set()
  const numbers = {}
  let hasDefine = false
  for (const raw of String(text).split(/\r?\n/)) {
    const line = raw.trim()
    if (!line.startsWith('#define')) continue
    hasDefine = true
    const parts = line.split(/\s+/)
    if (parts.length >= 2) defines.add(parts[1])
    const match = NUMBER_DEFINE.exec(line)
    if (match) numbers[match[1]] = Number(match[2])
  }
  if (!hasDefine) throw new Error('选中的 rtconfig.h 里没有 #define')

  let channel = 'unknown'
  for (const name of CHANNELS) {
    if (defines.has(`PKG_AGENT_${name}_CHANNEL`)) {
      channel = name
      break
    }
  }

  const modules = {}
  for (const [key, names] of Object.entries(BOOL_DEFINES)) {
    modules[key] = names.every((name) => defines.has(name))
  }

  const configNumbers = {}
  for (const [key, name] of Object.entries(NUMBER_DEFINES)) {
    if (name in numbers) configNumbers[key] = numbers[name]
  }
  return { channel, modules, numbers: configNumbers }
}

function rootDepth(file) {
  return file.webkitRelativePath ? file.webkitRelativePath.split('/').length : 0
}

function pickRoot(files) {
  const ranked = [...files].sort((a, b) => rootDepth(a) - rootDepth(b) || a.name.localeCompare(b.name))
  return ranked[0] || null
}

function baseName(name) {
  return name.replace(/\.[^.]+$/, '')
}

function pickBin(files, elf) {
  const bins = files.filter((file) => /\.bin$/i.test(file.name))
  const target = baseName(elf.name).toLowerCase()
  const match = bins.filter((file) => baseName(file.name).toLowerCase() === target)
  return pickRoot(match.length ? match : [])
}

export function selectFirmwareFiles(fileList) {
  const files = [...fileList]
  if (!files.length) throw new Error('没有选中文件')
  const elfs = files.filter((file) => /\.(elf|axf)$/i.test(file.name))
  const named = elfs.filter((file) => /^rtthread\./i.test(file.name))
  const elf = pickRoot(named.length ? named : elfs)
  if (!elf) throw new Error('没找到 .elf 固件，编译产物在工程根目录或 build/ 下')

  const maps = files.filter((file) => /\.map$/i.test(file.name))
  const sameMap = maps.filter((file) => baseName(file.name).toLowerCase() === baseName(elf.name).toLowerCase())
  const map = pickRoot(sameMap.length ? sameMap : maps)
  const bin = pickBin(files, elf)

  const headers = files.filter((file) => /\.h$/i.test(file.name))
  const configs = headers.filter((file) => /^rtconfig\.h$/i.test(file.name))
  const config = pickRoot(configs) || (headers.length === 1 ? headers[0] : null)
  if (!config) throw new Error('没找到 rtconfig.h，把它和固件一起选进来')
  return { elf, map, config, bin }
}

function nextFrame() {
  return new Promise((resolve) => setTimeout(resolve, 0))
}

function readBuffer(file, onRead) {
  if (typeof FileReader === 'undefined') {
    return file.arrayBuffer().then((buffer) => {
      if (onRead) onRead(1)
      return buffer
    })
  }
  return new Promise((resolve, reject) => {
    const reader = new FileReader()
    reader.onprogress = (event) => {
      if (onRead && event.lengthComputable && event.total) onRead(event.loaded / event.total)
    }
    reader.onload = () => {
      if (onRead) onRead(1)
      resolve(reader.result)
    }
    reader.onerror = () => reject(reader.error || new Error('读取文件失败'))
    reader.readAsArrayBuffer(file)
  })
}

async function readText(file, onRead) {
  return new TextDecoder().decode(await readBuffer(file, onRead))
}

export async function buildReport({ files, packageName = 'Agent-latest', onProgress }) {
  const { elf, map, config, bin } = files
  let percent = 0
  const emit = (value, stage) => {
    percent = Math.max(percent, Math.min(100, Math.round(value)))
    if (onProgress) onProgress({ percent, stage })
  }
  const phase = (start, end, stage) => (fraction) => {
    emit(start + (end - start) * Math.min(1, Math.max(0, fraction)), stage)
  }

  const elfReadEnd = map ? 30 : 45
  emit(0, '读取 ELF')
  const elfBuffer = await readBuffer(elf, phase(0, elfReadEnd, '读取 ELF'))

  let mapText = null
  let configText
  if (map) {
    emit(elfReadEnd, '读取 map')
    mapText = await readText(map, phase(elfReadEnd, 50, '读取 map'))
    emit(50, '读取 rtconfig.h')
    configText = await readText(config, phase(50, 58, '读取 rtconfig.h'))
  } else {
    emit(elfReadEnd, '读取 rtconfig.h')
    configText = await readText(config, phase(elfReadEnd, 60, '读取 rtconfig.h'))
  }

  await nextFrame()
  emit(map ? 58 : 60, '解析 ELF 节表')
  const size = parseElfSize(elfBuffer)

  await nextFrame()
  emit(map ? 66 : 78, '解析配置')
  const configData = parseConfig(configText)

  const firmware = {
    elf: elf.name,
    map: map ? map.name : null,
    config: config.name,
    ...size,
    bin: bin ? bin.size : null,
    flashLoad: size.text + size.data,
    staticRam: size.data + size.bss,
  }

  const report = {
    schema: REPORT_SCHEMA,
    schemaVersion: 1,
    generatedAt: new Date().toISOString().replace(/\.\d{3}Z$/, 'Z'),
    generator: { name: 'planner-web', version: '1.0.0' },
    firmware,
    config: configData,
    runtime: null,
    notes: [
      'Firmware 来自浏览器内解析的 ELF 节表，flashLoad 为 text+data，staticRam 为 data+bss。',
      `Agent 归属只统计 map 中路径含 ${packageName} 的对象，不含 paho、lwIP 等外部依赖。`,
      'config 只保留白名单里的开关与数值，不含任何字符串配置。',
      '文件只在浏览器里解析，没有上传到控制台。',
    ],
  }

  if (mapText !== null) {
    await nextFrame()
    emit(72, '解析 map 归属')
    report.agent = { package: packageName, ...parseMap(mapText, packageName, phase(72, 97, '解析 map 归属')) }
  }

  emit(100, '完成')
  return report
}
