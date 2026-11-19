<script setup>
import { computed, onMounted, reactive, ref, watch } from 'vue'
import {
  NAlert,
  NButton,
  NCard,
  NCheckbox,
  NCheckboxGroup,
  NIcon,
  NInputNumber,
  NProgress,
  NSpace,
  NSwitch,
  NTag,
  useMessage,
} from 'naive-ui'
import { CopyOutline, DownloadOutline, RefreshOutline } from '@vicons/ionicons5'
import {
  BASELINE_STATE,
  DEPS,
  MODEL_INFO,
  MODULES,
  PRESETS,
  STACKS,
  TRIMS,
  applyBaseline,
  calculate,
  defaultModules,
  defaultStacks,
  defaultTrims,
  formatBytes,
  formatNumber,
  formatPercent,
  moduleRam,
  recommend,
  stackBytes,
  storageInfo,
} from '../resourceModel.js'
import { buildReport, selectFirmwareFiles } from '../firmwareReport.js'
import {
  baselineMeasurements,
  calibration,
  loadBaseline,
  parseReport,
  reportMeasurements,
  reportTrust,
  reportWhole,
  stackRows,
} from '../resourceReport.js'

const message = useMessage()
const STORAGE_KEY = 'agent-console-planner-v1'
const REPORT_KEY = 'agent-console-planner-report-v1'

const CAPACITY_PRESETS = [
  { label: '1 MB / 192 KB', flash: 1024, ram: 192 },
  { label: '2 MB / 256 KB', flash: 2048, ram: 256 },
  { label: '4 MB / 512 KB', flash: 4096, ram: 512 },
  { label: '8 MB / 1 MB', flash: 8192, ram: 1024 },
]

function defaults() {
  return {
    chipFlashKB: 2048,
    chipRamKB: 192,
    spiFlashMB: 16,
    sdCardGB: 8,
    existingFlashKB: 0,
    existingRamKB: 0,
    marginFlash: 15,
    marginRam: 20,
    existingDeps: [],
    modules: { ...defaultModules(), memory: true, mqtt: true, device: true },
    trims: defaultTrims(),
    stacks: defaultStacks(),
  }
}

const form = reactive(defaults())
const baseline = ref({ ...BASELINE_STATE })
const report = ref(null)
const reportSource = ref([])
const parsing = ref(false)
const parsePercent = ref(0)
const parseStage = ref('读取文件')
const fileInput = ref(null)
const firmwareInput = ref(null)
const firmwareDirInput = ref(null)

onMounted(async () => {
  try {
    const raw = localStorage.getItem(STORAGE_KEY)
    if (raw) {
      const saved = JSON.parse(raw)
      Object.assign(form, {
        ...defaults(),
        ...saved,
        modules: { ...defaultModules(), ...(saved.modules || {}) },
        trims: { ...defaultTrims(), ...(saved.trims || {}) },
        stacks: { ...defaultStacks(), ...(saved.stacks || {}) },
        existingDeps: Array.isArray(saved.existingDeps) ? saved.existingDeps : [],
      })
    }
  } catch {
    // 本地缓存坏了就用默认值
  }

  const previousStacks = Object.fromEntries(STACKS.map((stack) => [stack.key, stack.bytes / 1024]))
  const data = await loadBaseline()
  if (data && applyBaseline(data)) {
    for (const stack of STACKS) {
      if (Number(form.stacks[stack.key]) === previousStacks[stack.key]) {
        form.stacks[stack.key] = stack.bytes / 1024
      }
    }
    form.stacks = { ...form.stacks }
    baseline.value = { ...BASELINE_STATE }
  }

  try {
    const raw = localStorage.getItem(REPORT_KEY)
    if (raw) {
      const parsed = parseReport(raw)
      if (!parsed.error) {
        report.value = parsed.report
        reportSource.value = reportFileNames(parsed.report)
      }
    }
  } catch {
    // 报告缓存坏了就忽略
  }
})

watch(
  form,
  () => {
    try {
      localStorage.setItem(STORAGE_KEY, JSON.stringify(form))
    } catch {
      // 存不下不影响当前计算
    }
  },
  { deep: true },
)

watch(
  () => form.modules.memory,
  (on) => {
    if (!on) form.modules.memoryPersist = false
  },
)
watch(
  () => form.modules.multimodal,
  (on) => {
    if (!on) form.modules.voice = false
  },
)

const calc = computed(() => calculate(form))
const rec = computed(() => recommend(form))
const storage = computed(() => storageInfo(form, calc.value))
const best = computed(() => rec.value.results.find((item) => item.key === rec.value.bestKey))
const trust = computed(() => reportTrust(report.value, form))
const whole = computed(() => (report.value ? reportWhole(report.value) : null))
const measurements = computed(() => ({
  ...baselineMeasurements(baseline.value),
  ...reportMeasurements(report.value),
}))
const activeKeys = computed(() => new Set(MODULES.filter((m) => form.modules[m.key]).map((m) => m.key)))
const measureRows = computed(() => stackRows(measurements.value, form, activeKeys.value))
const hasMeasurements = computed(() => measureRows.value.some((row) => row.measured))

function shortDate(value) {
  return value ? String(value).slice(0, 10) : '未标注'
}

function reportFileNames(data) {
  const firmware = data?.firmware || {}
  return [firmware.elf, firmware.map, firmware.config].filter(Boolean)
}

function setReport(data, source) {
  report.value = data
  reportSource.value = source
  try {
    localStorage.setItem(REPORT_KEY, JSON.stringify(data))
  } catch {
    // 存不下只影响下次打开
  }
}

function pickReport() {
  fileInput.value?.click()
}

function pickFirmwareFiles() {
  firmwareInput.value?.click()
}

function pickFirmwareDir() {
  firmwareDirInput.value?.click()
}

async function onReportFile(event) {
  const file = event.target.files?.[0]
  if (!file) return
  const parsed = parseReport(await file.text())
  event.target.value = ''
  if (parsed.error) {
    message.error(parsed.error)
    return
  }
  setReport(parsed.report, reportFileNames(parsed.report))
  message.success('已导入实测报告')
}

async function onFirmwareFiles(event) {
  const files = [...(event.target.files || [])]
  event.target.value = ''
  if (!files.length) return
  parsing.value = true
  parsePercent.value = 0
  parseStage.value = '读取文件'
  try {
    const picked = selectFirmwareFiles(files)
    const built = await buildReport({
      files: picked,
      onProgress: ({ percent, stage }) => {
        parsePercent.value = percent
        parseStage.value = stage
      },
    })
    const parsed = parseReport(built)
    if (parsed.error) throw new Error(parsed.error)
    const source = [picked.elf, picked.map, picked.config, picked.bin].filter(Boolean).map((item) => item.name)
    setReport(parsed.report, source)
    await new Promise((resolve) => setTimeout(resolve, 250))
    message.success(parsed.report.agent ? '已从固件生成报告' : '已从固件生成报告，没选 map 就没包内归属')
  } catch (error) {
    message.error(error?.message || '固件解析失败')
  } finally {
    parsing.value = false
  }
}

function clearReport() {
  report.value = null
  reportSource.value = []
  localStorage.removeItem(REPORT_KEY)
  message.success('已清除实测报告')
}

function applyCalibration() {
  const result = calibration(report.value, calc.value)
  if (!result.ok) {
    message.warning(result.message)
    return
  }
  form.existingFlashKB = result.existingFlashKB
  form.existingRamKB = result.existingRamKB
  message.success(`现有固件校准为 Flash ${result.existingFlashKB} KB、RAM ${result.existingRamKB} KB`)
}

function canToggle(module) {
  return (module.depends || []).every((dep) => form.modules[dep])
}

function depNames(module) {
  return (module.depends || [])
    .map((dep) => MODULES.find((item) => item.key === dep)?.name || dep)
    .join('、')
}

function ramOf(module) {
  return moduleRam(module, form)
}

function stackOf(key) {
  return STACKS.find((stack) => stack.key === key)
}

function stackActive(stack) {
  return !stack.requires || Boolean(form.modules[stack.requires])
}

function stackChanged(stack) {
  return stackBytes(form, stack) !== stack.bytes
}

function stackDelta(stack) {
  return stackBytes(form, stack) - stack.bytes
}

function currentStackKB(stack) {
  return stackBytes(form, stack) / 1024
}

function resetStacks() {
  form.stacks = defaultStacks()
  message.success('线程栈已恢复实测配置')
}

function signed(bytes) {
  return `${bytes >= 0 ? '+' : '−'}${formatBytes(Math.abs(bytes))}`
}

function clampPercent(percent) {
  return Math.max(0, Math.min(100, Number(percent) || 0))
}

function usageStatus(percent, fit) {
  if (!fit) return 'error'
  if (percent >= 80) return 'warning'
  return 'success'
}

function smallPercentLabel(percent) {
  const value = Number(percent) || 0
  if (value > 0 && value < 0.1) return '<0.1%'
  return formatPercent(value)
}

function applyCapacity(preset) {
  form.chipFlashKB = preset.flash
  form.chipRamKB = preset.ram
}

function applyPreset(key) {
  const preset = PRESETS.find((item) => item.key === key)
  if (!preset) return
  form.modules = { ...defaultModules(), ...preset.modules }
  form.trims = { ...defaultTrims(), ...preset.trims }
  message.success(`已应用「${preset.name}」`)
}

function reset() {
  Object.assign(form, defaults())
  message.success('已恢复默认值')
}

function pushChoice(lines, prefix, on) {
  const yes = on ? 'ENABLE' : 'DISABLE'
  const no = on ? 'DISABLE' : 'ENABLE'
  lines.push(`CONFIG_${prefix}_${yes}=y`)
  lines.push(`# CONFIG_${prefix}_${no} is not set`)
}

function kconfigText() {
  const lines = [
    '# Agent 资源配置评估导出',
    `# 片内 Flash ${form.chipFlashKB} KB / RAM ${form.chipRamKB} KB`,
    `# 预计占用 Flash ${formatBytes(calc.value.flash.total)} / RAM ${formatBytes(calc.value.ram.total)}`,
    '',
    'CONFIG_PKG_USING_AGENT=y',
  ]
  const push = (key, on) => lines.push(on ? `CONFIG_${key}=y` : `# CONFIG_${key} is not set`)
  const persist = form.modules.memory && form.modules.memoryPersist

  push('PKG_AGENT_MEMORY_ENABLE', form.modules.memory)
  push('PKG_AGENT_MEMORY_TOOLS_ENABLE', form.modules.memory)
  push('PKG_AGENT_MEMORY_PERSIST', persist)
  if (persist) lines.push('CONFIG_PKG_AGENT_MEMORY_FILE="/agent_memory.dat"')
  push('PKG_AGENT_TOOL_MQTT_ENABLE', form.modules.mqtt)
  push('PKG_AGENT_TOOL_DEVICE', form.modules.device)
  push('PKG_AGENT_TOOL_MCP_ENABLE', form.modules.mcp)
  pushChoice(lines, 'PKG_AGENT_MULTIMODAL', form.modules.multimodal)
  pushChoice(lines, 'PKG_AGENT_STT', form.modules.multimodal)
  pushChoice(lines, 'PKG_AGENT_TTS', form.modules.voice)
  push('PKG_AGENT_DRIVER_AUDIO', form.modules.voice)

  lines.push('')
  lines.push(
    '# 线程栈：' +
      STACKS.filter(stackActive)
        .map((stack) => `${stack.name} ${currentStackKB(stack)} KB`)
        .join('、'),
  )
  if (form.modules.multimodal) {
    lines.push(`CONFIG_PKG_AGENT_FILE_OP_THREAD_SIZE=${Math.round(currentStackKB(stackOf('fileOp')) * 1024)}`)
  }
  if (form.modules.mqtt) {
    lines.push(`CONFIG_PKG_AGENT_TOOL_MQTT_RX_THREAD_STACK=${Math.round(currentStackKB(stackOf('mqttRx')) * 1024)}`)
    lines.push(
      `# paho 工作线程栈 ${Math.round(currentStackKB(stackOf('mqttWorker')) * 1024)} 字节，由 RT_PKG_MQTT_THREAD_STACK_SIZE 控制`,
    )
  }
  lines.push('# AgentLoop、A2A、播报与录音线程栈写死在源码的线程创建参数里，按上面的 KB 数改源码')

  lines.push('')
  if (form.trims.skillsOne) {
    lines.push('# 裁剪：把 PKG_AGENT_SKILL_DIR 指向只放 1 份技能的目录')
  }
  if (form.trims.singleProtocol) {
    lines.push('# 裁剪：Kconfig 没有单协议开关，改 SConscript 只编一种协议')
  }
  return lines.join('\n')
}

async function copyKconfig() {
  try {
    await navigator.clipboard.writeText(kconfigText())
    message.success('Kconfig 片段已复制')
  } catch {
    message.error('复制失败，可以用导出 JSON 保留配置')
  }
}

function download(text, filename, type) {
  const blob = new Blob([text], { type })
  const url = URL.createObjectURL(blob)
  const link = document.createElement('a')
  link.href = url
  link.download = filename
  link.click()
  URL.revokeObjectURL(url)
}

function exportJson() {
  const payload = {
    generatedAt: new Date().toISOString(),
    model: MODEL_INFO,
    input: JSON.parse(JSON.stringify(form)),
    result: {
      flash: calc.value.flash,
      ram: calc.value.ram,
      capacity: calc.value.capacity,
      storage: storage.value,
      recommendation: { bestKey: rec.value.bestKey, hasFit: rec.value.hasFit },
    },
  }
  download(JSON.stringify(payload, null, 2), 'agent-resource-plan.json', 'application/json')
}
</script>

<template>
  <div>
    <div class="page-head">
      <div>
        <h2>资源配置评估</h2>
        <div class="muted">输入设备容量，估算 Agent 方案的 Flash、RAM 与外部存储占用</div>
      </div>
      <div class="spacer"></div>
      <NSpace :wrap="true">
        <NButton secondary @click="reset">
          <template #icon><NIcon><RefreshOutline /></NIcon></template>
          恢复默认
        </NButton>
        <NButton secondary @click="copyKconfig">
          <template #icon><NIcon><CopyOutline /></NIcon></template>
          复制 Kconfig
        </NButton>
        <NButton type="primary" @click="exportJson">
          <template #icon><NIcon><DownloadOutline /></NIcon></template>
          导出 JSON
        </NButton>
      </NSpace>
    </div>

    <NAlert v-for="warning in calc.warnings" :key="warning" type="warning" class="alert">
      {{ warning }}
    </NAlert>

    <div class="planner-grid">
      <div class="stack">
        <NCard title="设备容量" size="small">
          <div class="field-grid">
            <label class="field">
              <span>片内 Flash（KB）</span>
              <NInputNumber v-model:value="form.chipFlashKB" :min="0" :max="65536" :step="128" :show-button="false" />
            </label>
            <label class="field">
              <span>片内 RAM（KB）</span>
              <NInputNumber v-model:value="form.chipRamKB" :min="0" :max="32768" :step="16" :show-button="false" />
            </label>
            <label class="field">
              <span>SPI Flash（MB）</span>
              <NInputNumber v-model:value="form.spiFlashMB" :min="0" :max="1024" :step="4" :show-button="false" />
            </label>
            <label class="field">
              <span>SD 卡（GB）</span>
              <NInputNumber v-model:value="form.sdCardGB" :min="0" :max="2048" :step="1" :show-button="false" />
            </label>
          </div>

          <div class="quick-row">
            <span class="muted">快速填充</span>
            <NButton
              v-for="preset in CAPACITY_PRESETS"
              :key="preset.label"
              size="small"
              secondary
              @click="applyCapacity(preset)"
            >
              {{ preset.label }}
            </NButton>
          </div>

          <div class="section-label">现有固件</div>
          <div class="field-grid">
            <label class="field">
              <span>Flash 占用（KB）</span>
              <NInputNumber v-model:value="form.existingFlashKB" :min="0" :max="65536" :step="64" :show-button="false" />
            </label>
            <label class="field">
              <span>静态 RAM 占用（KB）</span>
              <NInputNumber v-model:value="form.existingRamKB" :min="0" :max="32768" :step="8" :show-button="false" />
            </label>
          </div>
          <div class="muted">填 0 表示从零评估 Agent 新增占用。已有工程时填 Agent 之外的 text+data 占用，避免重复计算。</div>

          <div class="section-label">现有工程已包含</div>
          <NCheckboxGroup v-model:value="form.existingDeps">
            <NSpace>
              <NCheckbox v-for="dep in DEPS" :key="dep.key" :value="dep.key">{{ dep.name }}</NCheckbox>
            </NSpace>
          </NCheckboxGroup>
          <div class="muted">勾选后对应依赖从新增占用里扣除，请把它们的体积算进上面的现有固件。</div>

          <div class="section-label">安全余量</div>
          <div class="field-grid">
            <label class="field">
              <span>Flash 余量（%）</span>
              <NInputNumber v-model:value="form.marginFlash" :min="0" :max="40" :show-button="false" />
            </label>
            <label class="field">
              <span>RAM 余量（%）</span>
              <NInputNumber v-model:value="form.marginRam" :min="0" :max="40" :show-button="false" />
            </label>
          </div>
        </NCard>

        <NCard title="功能模块" size="small">
          <div
            v-for="module in MODULES"
            :key="module.key"
            class="module-row"
            :class="{ 'is-disabled': !canToggle(module) }"
          >
            <div class="module-main">
              <div class="module-title">
                <NSwitch v-model:value="form.modules[module.key]" :disabled="!canToggle(module)" size="small" />
                <strong>{{ module.name }}</strong>
                <NTag v-if="module.estimated" size="small" :bordered="false" type="warning">估算</NTag>
                <NTag v-if="module.depends?.length" size="small" :bordered="false">
                  需先开{{ depNames(module) }}
                </NTag>
              </div>
              <div class="module-note">{{ module.note }}</div>
              <code>{{ module.kconfig }}</code>
            </div>
            <div class="module-cost">
              <span>Flash {{ signed(module.flash) }}</span>
              <span>RAM {{ signed(ramOf(module)) }}</span>
            </div>
          </div>
        </NCard>

        <NCard title="线程栈" size="small">
          <template #header-extra>
            <NButton size="tiny" secondary @click="resetStacks">恢复实测值</NButton>
          </template>
          <div
            v-for="stack in STACKS"
            :key="stack.key"
            class="stack-row"
            :class="{ 'is-disabled': !stackActive(stack) }"
          >
            <div class="stack-head">
              <strong>{{ stack.name }}</strong>
              <NTag v-if="!stackActive(stack)" size="small" :bordered="false">未启用</NTag>
              <NTag v-else-if="stackChanged(stack)" size="small" :bordered="false" type="warning">
                {{ signed(stackDelta(stack)) }}
              </NTag>
              <div class="spacer"></div>
              <div class="stack-control">
                <NInputNumber
                  v-model:value="form.stacks[stack.key]"
                  :min="stack.minKB"
                  :max="stack.maxKB"
                  :step="1"
                  :disabled="!stackActive(stack)"
                  size="small"
                  :show-button="false"
                />
                <span class="muted">KB</span>
              </div>
            </div>
            <div class="stack-note">
              {{ stack.note }}
              <span class="muted">
                默认 {{ stack.bytes / 1024 }} KB · {{ stack.source }}<template v-if="stack.watermark != null">
                  · 水位 {{ Math.round(stack.watermark * 100) }}%</template>
              </span>
            </div>
            <code v-if="stack.kconfig">{{ stack.kconfig }}</code>
          </div>
          <div class="muted stack-tip">
            调高上下文栈会直接增加 RAM 占用，Flash 不变。改完按导出的值重新编译；AgentLoop、A2A、播报与录音栈写死在线程创建参数里，文件操作与 MQTT 接收栈走 Kconfig。
          </div>
        </NCard>

        <NCard title="裁剪选项" size="small">
          <div v-for="trim in TRIMS" :key="trim.key" class="module-row">
            <div class="module-main">
              <div class="module-title">
                <NSwitch v-model:value="form.trims[trim.key]" size="small" />
                <strong>{{ trim.name }}</strong>
                <NTag size="small" :bordered="false" type="warning">估算</NTag>
              </div>
              <div class="module-note">{{ trim.note }}</div>
              <code>{{ trim.kconfig }}</code>
            </div>
            <div class="module-cost">
              <span>Flash {{ signed(trim.flash) }}</span>
            </div>
          </div>
        </NCard>
      </div>

      <div class="stack">
        <NCard title="推荐配置" size="small">
          <template #header-extra>
            <NTag :type="rec.hasFit ? 'success' : 'error'" :bordered="false">
              {{ rec.hasFit ? '有可用方案' : '容量不足' }}
            </NTag>
          </template>

          <div class="recommend-main">
            <div>
              <div class="muted">按当前容量和余量</div>
              <h3>{{ best.name }}</h3>
              <div class="muted">{{ best.desc }}</div>
            </div>
            <NButton type="primary" :disabled="!rec.hasFit" @click="applyPreset(best.key)">应用推荐</NButton>
          </div>

          <div class="rec-metrics">
            <div>
              <span class="muted">Flash</span>
              <strong>{{ formatBytes(best.result.flash.total) }}</strong>
              <span class="muted">{{ formatPercent(best.result.capacity.flashPercent) }}</span>
            </div>
            <div>
              <span class="muted">RAM</span>
              <strong>{{ formatBytes(best.result.ram.total) }}</strong>
              <span class="muted">{{ formatPercent(best.result.capacity.ramPercent) }}</span>
            </div>
          </div>

          <div class="preset-list">
            <div v-for="preset in rec.results" :key="preset.key" class="preset-row">
              <div class="preset-main">
                <strong>{{ preset.name }}</strong>
                <span class="muted">{{ preset.desc }}</span>
              </div>
              <div class="preset-metrics">
                <span>{{ formatBytes(preset.result.flash.total) }}</span>
                <span>{{ formatBytes(preset.result.ram.total) }}</span>
              </div>
              <NTag
                size="small"
                :bordered="false"
                :type="preset.fit ? (preset.key === rec.bestKey ? 'success' : 'info') : 'error'"
              >
                {{ preset.fit ? (preset.key === rec.bestKey ? '推荐' : '可运行') : preset.reason }}
              </NTag>
              <NButton size="small" text type="primary" @click="applyPreset(preset.key)">应用</NButton>
            </div>
          </div>
        </NCard>

        <NCard title="资源占用" size="small">
          <div class="usage-block">
            <div class="usage-head">
              <strong>片内 Flash</strong>
              <span class="mono">
                {{ formatBytes(calc.flash.total) }} / {{ formatBytes(calc.capacity.flashCapacity) }}
              </span>
            </div>
            <NProgress
              type="line"
              :percentage="clampPercent(calc.capacity.flashPercent)"
              :status="usageStatus(calc.capacity.flashPercent, calc.capacity.flashFit)"
              :height="10"
              :show-indicator="false"
            />
            <div class="usage-foot">
              <span>占比 {{ formatPercent(calc.capacity.flashPercent) }}</span>
              <span :class="calc.capacity.flashFree >= 0 ? 'ok' : 'error'">
                余量 {{ formatBytes(calc.capacity.flashFree) }}
              </span>
              <span class="muted">推荐上限 {{ 100 - form.marginFlash }}%</span>
            </div>
          </div>

          <div class="usage-block">
            <div class="usage-head">
              <strong>片内 RAM</strong>
              <span class="mono">
                {{ formatBytes(calc.ram.total) }} / {{ formatBytes(calc.capacity.ramCapacity) }}
              </span>
            </div>
            <NProgress
              type="line"
              :percentage="clampPercent(calc.capacity.ramPercent)"
              :status="usageStatus(calc.capacity.ramPercent, calc.capacity.ramFit)"
              :height="10"
              :show-indicator="false"
            />
            <div class="usage-foot">
              <span>占比 {{ formatPercent(calc.capacity.ramPercent) }}</span>
              <span :class="calc.capacity.ramFree >= 0 ? 'ok' : 'error'">
                余量 {{ formatBytes(calc.capacity.ramFree) }}
              </span>
              <span class="muted">推荐上限 {{ 100 - form.marginRam }}%</span>
            </div>
          </div>

          <div class="section-label">Flash 明细</div>
          <table class="breakdown">
            <thead>
              <tr><th>项目</th><th class="num">字节</th></tr>
            </thead>
            <tbody>
              <tr v-for="row in calc.flash.rows" :key="row.key">
                <td>
                  <span>{{ row.name }}</span>
                  <NTag v-if="row.estimated" size="small" :bordered="false" type="warning">估算</NTag>
                  <div class="muted">{{ row.note }}</div>
                </td>
                <td class="num mono">{{ row.skipped ? '已包含' : formatNumber(row.bytes) }}</td>
              </tr>
            </tbody>
            <tfoot>
              <tr><td>合计</td><td class="num mono">{{ formatNumber(calc.flash.total) }}</td></tr>
            </tfoot>
          </table>

          <div class="section-label">RAM 明细</div>
          <table class="breakdown">
            <thead>
              <tr><th>项目</th><th class="num">字节</th></tr>
            </thead>
            <tbody>
              <tr v-for="row in calc.ram.rows" :key="row.key">
                <td>
                  <span>{{ row.name }}</span>
                  <div class="muted">{{ row.note }}</div>
                </td>
                <td class="num mono">{{ formatNumber(row.bytes) }}</td>
              </tr>
            </tbody>
            <tfoot>
              <tr><td>合计</td><td class="num mono">{{ formatNumber(calc.ram.total) }}</td></tr>
            </tfoot>
          </table>
        </NCard>

        <NCard title="实测与基线" size="small">
          <template #header-extra>
            <NTag :type="trust.type" :bordered="false">{{ trust.label }}</NTag>
          </template>

          <div class="report-actions">
            <input ref="fileInput" type="file" accept=".json,application/json" hidden @change="onReportFile" />
            <input
              ref="firmwareInput"
              type="file"
              multiple
              accept=".elf,.axf,.map,.h,.bin"
              hidden
              @change="onFirmwareFiles"
            />
            <input
              ref="firmwareDirInput"
              type="file"
              webkitdirectory
              multiple
              hidden
              @change="onFirmwareFiles"
            />
            <NButton size="small" type="primary" secondary :loading="parsing" @click="pickFirmwareDir">
              选择固件目录
            </NButton>
            <NButton size="small" secondary :loading="parsing" @click="pickFirmwareFiles">选择固件文件</NButton>
            <NButton size="small" secondary @click="pickReport">导入报告 JSON</NButton>
            <NButton size="small" secondary :disabled="!report" @click="applyCalibration">
              用报告校准现有固件
            </NButton>
            <NButton v-if="report" size="small" text type="error" @click="clearReport">清除</NButton>
          </div>
          <div class="muted report-hint">
            选工程根目录或 build 目录里带 <code>rtthread.elf</code>、<code>rtconfig.h</code> 的那一层，
            <code>.map</code> 与 <code>.bin</code> 一起选上更准。文件只在浏览器里解析，不上传。
          </div>
          <div class="muted report-hint">{{ trust.hint }}</div>

          <div v-if="parsing" class="parse-progress">
            <NProgress
              type="line"
              :percentage="parsePercent"
              :height="6"
              :show-indicator="false"
              :processing="parsePercent < 100"
            />
            <div class="parse-stage muted">
              <span>{{ parseStage }}</span>
              <span class="mono">{{ parsePercent }}%</span>
            </div>
          </div>

          <template v-if="report">
            <div class="rec-metrics report-metrics">
              <div>
                <span class="muted">整机 Flash</span>
                <strong>{{ formatBytes(whole.flash) }}</strong>
                <span class="muted">text + data</span>
              </div>
              <div>
                <span class="muted">整机静态 RAM</span>
                <strong>{{ formatBytes(whole.ram) }}</strong>
                <span class="muted">data + bss</span>
              </div>
              <div>
                <span class="muted">包内实测 Flash</span>
                <strong>{{ report.agent ? formatBytes(report.agent.flash) : '未统计' }}</strong>
                <span class="muted">{{ report.agent ? 'map 归属' : '采集时没给 map' }}</span>
              </div>
              <div>
                <span class="muted">包内实测 RAM</span>
                <strong>{{ report.agent ? formatBytes(report.agent.ram) : '未统计' }}</strong>
                <span class="muted">{{ report.agent ? 'map 归属' : '采集时没给 map' }}</span>
              </div>
            </div>
            <div v-if="reportSource.length" class="muted report-meta">来源 {{ reportSource.join(' · ') }}</div>
            <div class="muted report-meta">
              报告生成于 {{ shortDate(report.generatedAt) }}，通道 {{ report.config?.channel || '未知' }}<template
                v-if="whole.bin"
              >，镜像 {{ formatBytes(whole.bin) }}</template>。
            </div>
          </template>

          <template v-if="hasMeasurements">
            <div class="section-label">线程栈水位</div>
            <table class="breakdown">
              <thead>
                <tr>
                  <th>线程</th>
                  <th class="num">峰值 / 实测栈</th>
                  <th class="num">当前预算</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="row in measureRows" :key="row.key">
                  <td>
                    <span>{{ row.name }}</span>
                    <NTag v-if="row.over" size="small" :bordered="false" type="error">预算偏低</NTag>
                    <div class="muted">
                      {{ row.watermark !== null ? `水位 ${Math.round(row.watermark * 100)}%` : '无水位' }}
                      <template v-if="row.source"> · {{ row.source }}</template>
                    </div>
                  </td>
                  <td class="num mono">
                    {{ row.measured ? `${formatBytes(row.peak)} / ${formatBytes(row.bytes)}` : '—' }}
                  </td>
                  <td class="num mono">{{ formatBytes(row.budget) }}</td>
                </tr>
              </tbody>
            </table>
            <div class="muted report-note">
              峰值 = 实测栈大小 × 水位，未测到水位的线程显示为 —。预算取当前线程栈设置。
            </div>
          </template>

          <div class="muted baseline-line">
            <template v-if="baseline.loaded">
              基线 {{ baseline.version }}（{{ shortDate(baseline.generatedAt) }}），来源 {{ baseline.source }}
            </template>
            <template v-else>基线未加载，当前数字使用内置常量。</template>
          </div>
        </NCard>

        <NCard title="外部存储" size="small">
          <template #header-extra>
            <NTag :type="storage.persistent ? 'success' : 'default'" :bordered="false">
              记忆落盘{{ storage.persistent ? '已开' : '未开' }}
            </NTag>
          </template>

          <div class="storage-grid">
            <div class="storage-tile">
              <div class="storage-head">
                <strong>SPI Flash</strong>
                <span>{{ form.spiFlashMB }} MB</span>
              </div>
              <template v-if="storage.spi">
                <p>
                  固件镜像约 {{ formatBytes(storage.imageBytes) }}
                  <span class="muted">占 {{ smallPercentLabel(storage.spi.singlePercent) }}</span>
                </p>
                <p>
                  OTA 单备份
                  <NTag size="small" :bordered="false" :type="storage.spi.otaSingle ? 'success' : 'error'">
                    {{ storage.spi.otaSingle ? '够' : '不足' }}
                  </NTag>
                </p>
                <p>
                  OTA 双备份
                  <NTag size="small" :bordered="false" :type="storage.spi.otaDouble ? 'success' : 'error'">
                    {{ storage.spi.otaDouble ? '够' : '不足' }}
                  </NTag>
                  <span class="muted">占 {{ smallPercentLabel(storage.spi.doublePercent) }}</span>
                </p>
                <p v-if="storage.memoryFileBytes">
                  记忆文件 ≤ {{ formatBytes(storage.memoryFileBytes) }}
                  <NTag size="small" :bordered="false" :type="storage.spi.memory ? 'success' : 'error'">
                    {{ storage.spi.memory ? '够' : '不足' }}
                  </NTag>
                  <span class="muted">占 {{ smallPercentLabel(storage.spi.memoryPercent) }}</span>
                </p>
              </template>
              <p v-else class="muted">未配置 SPI Flash。</p>
            </div>

            <div class="storage-tile">
              <div class="storage-head">
                <strong>SD 卡</strong>
                <span>{{ form.sdCardGB }} GB</span>
              </div>
              <template v-if="storage.sd">
                <p v-if="storage.memoryFileBytes">
                  记忆文件 ≤ {{ formatBytes(storage.memoryFileBytes) }}
                  <NTag size="small" :bordered="false" :type="storage.sd.memory ? 'success' : 'error'">
                    {{ storage.sd.memory ? '够' : '不足' }}
                  </NTag>
                  <span class="muted">占 {{ smallPercentLabel(storage.sd.memoryPercent) }}</span>
                </p>
                <p>
                  32 MB 起步
                  <NTag size="small" :bordered="false" :type="storage.sd.recommended ? 'success' : 'warning'">
                    {{ storage.sd.recommended ? '满足' : '偏小' }}
                  </NTag>
                </p>
                <p class="muted">记忆文件按 40 个槽位上限估算，实际只写已用条目。</p>
              </template>
              <p v-else class="muted">未配置 SD 卡。</p>
            </div>
          </div>

          <p class="muted storage-tip">
            SPI Flash 与 SD 卡只放文件、记忆和 OTA 备份，不能直接扩容片内 Flash 或 RAM；除非芯片支持 XIP，否则程序仍在片内 Flash 执行。
          </p>
        </NCard>
      </div>
    </div>

    <NCard title="数据口径" size="small" class="block">
      <p>
        模型版本 {{ baseline.version }}，数据来自 <code>{{ baseline.source }}</code>。基线：{{ baseline.baseline }}。
      </p>
      <ul class="notes">
        <li>Flash 模块差值来自评测矩阵的整机差值；技能只留 1 份、只编译一种协议与语音播报仍是估算值，要改 SConscript、技能目录或音频配置后重新构建确认。</li>
        <li>RAM 含静态表、线程栈和堆预留。静态 RAM 按 map 归属拆分，线程栈按 KB 调整后立即计入；峰值见「实测与基线」，调高后按导出值重新编译。</li>
        <li>页面启动时加载 <code>public/agent-resource-baseline.json</code>，覆盖固定体积、模块差值和栈默认值。换版本后用 <code>scripts/resource_report.py baseline</code> 重新生成；加载失败就退回内置常量。</li>
        <li>「选择固件目录」或「选择固件文件」直接读 <code>rtthread.elf</code>、<code>rtthread.map</code> 与 <code>rtconfig.h</code>，解析都在浏览器里完成；「导入报告 JSON」读 <code>resource_report.py collect</code> 生成的报告。报告只含白名单开关和数值，不含地址、账号或密钥；「用报告校准现有固件」用整机实测减去当前方案估算。</li>
        <li>现有工程已包含 TLS、lwIP 或 newlib 时，勾选后把对应体积填进现有固件，避免重复计算。</li>
        <li>数据测于 QEMU vexpress-a9，BSP 不同会带来链接布局差异；按推荐余量做预算。</li>
      </ul>
    </NCard>
  </div>
</template>

<style scoped>
.alert { margin-bottom: 12px; }
.planner-grid {
  display: grid;
  grid-template-columns: minmax(0, 1.02fr) minmax(0, 1.2fr);
  gap: 16px;
  align-items: start;
}
.field-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 12px;
}
.field {
  display: flex;
  flex-direction: column;
  gap: 6px;
  font-size: 13px;
  color: #5b647e;
}
.field :deep(.n-input-number) { width: 100%; }
.quick-row {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
  margin-top: 14px;
}
.section-label {
  margin: 18px 0 8px;
  font-size: 13px;
  color: #6b7280;
}
.module-row {
  display: flex;
  justify-content: space-between;
  gap: 18px;
  padding: 13px 0;
  border-top: 1px solid #f0f1f6;
}
.module-row:first-child { border-top: 0; padding-top: 0; }
.module-row.is-disabled { opacity: .55; }
.module-main { min-width: 0; }
.module-title {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}
.module-note {
  margin-top: 5px;
  color: #6b7280;
  font-size: 12px;
  line-height: 1.6;
}
.module-main code {
  display: block;
  margin-top: 5px;
  color: #8991a2;
  font-size: 11px;
  overflow-wrap: anywhere;
}
.module-cost {
  display: flex;
  flex-direction: column;
  align-items: flex-end;
  gap: 4px;
  white-space: nowrap;
  font-size: 12px;
  color: #5b647e;
}
.stack-row {
  padding: 11px 0;
  border-top: 1px solid #f0f1f6;
}
.stack-row:first-of-type { border-top: 0; padding-top: 0; }
.stack-row.is-disabled { opacity: .55; }
.stack-head {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}
.stack-head .spacer { flex: 1; }
.stack-control {
  display: flex;
  align-items: center;
  gap: 6px;
}
.stack-control :deep(.n-input-number) { width: 96px; }
.stack-note {
  margin-top: 4px;
  color: #6b7280;
  font-size: 12px;
  line-height: 1.6;
}
.stack-note .muted { margin-left: 6px; }
.stack-row code {
  display: block;
  margin-top: 4px;
  color: #8991a2;
  font-size: 11px;
  overflow-wrap: anywhere;
}
.stack-tip { margin-top: 12px; }
.report-actions {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}
.report-hint { margin-top: 8px; }
.parse-progress { margin: 12px 0 2px; }
.parse-stage {
  display: flex;
  justify-content: space-between;
  font-size: 12px;
  margin-top: 2px;
}
.report-metrics { margin: 14px 0 8px; }
.report-metrics strong { font-size: 18px; }
.report-meta { font-size: 12px; }
.report-note { margin-top: 8px; font-size: 12px; }
.baseline-line {
  margin-top: 12px;
  padding-top: 10px;
  border-top: 1px solid #f0f1f6;
  font-size: 12px;
}
.recommend-main {
  display: flex;
  justify-content: space-between;
  gap: 16px;
  align-items: flex-start;
}
.recommend-main h3 {
  margin: 4px 0 6px;
  font-size: 24px;
  letter-spacing: -.4px;
}
.rec-metrics {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 12px;
  margin: 18px 0;
}
.rec-metrics > div {
  display: flex;
  flex-direction: column;
  gap: 4px;
  padding: 14px;
  border: 1px solid #eef0f4;
  border-radius: 12px;
  background: #fafbfd;
}
.rec-metrics strong { font-size: 22px; }
.preset-list { border-top: 1px solid #f0f1f6; }
.preset-row {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 10px 0;
  border-bottom: 1px solid #f0f1f6;
}
.preset-main {
  display: flex;
  flex-direction: column;
  gap: 3px;
  min-width: 0;
  flex: 1;
}
.preset-main .muted { font-size: 12px; }
.preset-metrics {
  display: flex;
  flex-direction: column;
  gap: 3px;
  text-align: right;
  font-size: 12px;
  color: #5b647e;
  white-space: nowrap;
}
.usage-block + .usage-block { margin-top: 22px; }
.usage-head {
  display: flex;
  justify-content: space-between;
  gap: 12px;
  flex-wrap: wrap;
  margin-bottom: 8px;
}
.usage-foot {
  display: flex;
  gap: 16px;
  flex-wrap: wrap;
  margin-top: 8px;
  font-size: 13px;
}
.breakdown {
  width: 100%;
  border-collapse: collapse;
  font-size: 13px;
}
.breakdown th {
  text-align: left;
  padding: 6px;
  color: #6b7280;
  font-weight: 500;
}
.breakdown td {
  padding: 8px 6px;
  border-top: 1px solid #f0f1f6;
  vertical-align: top;
}
.breakdown td .muted { margin-top: 3px; font-size: 12px; }
.breakdown .num { text-align: right; white-space: nowrap; }
.breakdown tfoot td {
  border-top: 1px solid #dfe3ec;
  font-weight: 600;
}
.storage-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 12px;
}
.storage-tile {
  padding: 14px;
  border: 1px solid #eef0f4;
  border-radius: 12px;
  background: #fafbfd;
}
.storage-head {
  display: flex;
  justify-content: space-between;
  gap: 12px;
  margin-bottom: 10px;
}
.storage-tile p {
  margin: 7px 0;
  font-size: 13px;
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}
.storage-tip { margin-top: 14px; }
.block { margin-top: 16px; }
.notes {
  margin: 10px 0 0;
  padding-left: 20px;
  color: #5b647e;
  font-size: 13px;
  line-height: 1.8;
}
@media (max-width: 1180px) {
  .planner-grid { grid-template-columns: minmax(0, 1fr); }
}
@media (max-width: 760px) {
  .field-grid, .storage-grid, .rec-metrics { grid-template-columns: minmax(0, 1fr); }
  .recommend-main { flex-direction: column; }
  .preset-row { flex-wrap: wrap; }
  .preset-main { flex-basis: 100%; }
}
</style>
