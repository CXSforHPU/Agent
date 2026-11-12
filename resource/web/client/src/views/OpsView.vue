<script setup>
import { ref, onMounted, onBeforeUnmount, computed, nextTick } from 'vue'
import {
  NCard,
  NButton,
  NIcon,
  NTag,
  NAlert,
  NSpace,
  NSelect,
  NInput,
  NInputNumber,
  NCheckbox,
  NCheckboxGroup,
  NProgress,
  NTable,
  NText,
  useMessage,
} from 'naive-ui'
import {
  RefreshOutline,
  PlayOutline,
  StopOutline,
  DownloadOutline,
  BuildOutline,
  TerminalOutline,
  SaveOutline,
  CheckmarkCircleOutline,
  CloseCircleOutline,
  ArrowDownOutline,
} from '@vicons/ionicons5'
import { api } from '../api.js'

const message = useMessage()

const status = ref({})
const emqx = ref({})
const emqxBusy = ref(false)
async function refreshEmqx() {
  try { emqx.value = await api.emqxStatus() } catch (e) { report(e) }
}
async function emqxAction(action) {
  emqxBusy.value = true
  try { await api.emqxAction(action); await refreshEmqx() }
  catch (e) { report(e) }
  finally { emqxBusy.value = false }
}
onMounted(refreshEmqx)

const jobs = ref([])
const error = ref('')
const notice = ref('')
const busy = ref('')
const installComponents = ref(['file-server'])
const installLocked = computed(() => Boolean(busy.value) ||
  jobs.value.some(job => job.status === 'pending' || job.status === 'running'))

async function installSelected() {
  if (installLocked.value || !installComponents.value.length) return
  const selected = [...installComponents.value]
  busy.value = '安装选中依赖'
  try {
    if (selected.includes('web')) {
      if (!await install('web', [], '前端依赖')) return
    }
    if (selected.includes('emqx')) {
      if (!await install('emqx', [], 'EMQX')) return
      await refreshEmqx()
    }
    const components = selected.filter(item => !['web', 'emqx'].includes(item))
    if (components.length) await install('file-server', components, '服务组件依赖')
  } finally {
    busy.value = ''
  }
}

const logName = ref('file-server')
const logLines = ref([])
const logBox = ref(null)
const followLog = ref(true)

const ttsText = ref('这是 Agent 管理控制台合成的语音。')
const ttsSpeaker = ref('')
const speakers = ref([])
const audioUrl = ref('')
const ttsBusy = ref(false)
const ttsError = ref('')
const ttsProgress = ref(null)

const fileServerPortInput = ref(null)
const fileServerPortDirty = ref(false)
const mcpPortInput = ref(null)
const mcpPortDirty = ref(false)
const deviceHostInput = ref('')
const deviceHostDirty = ref(false)
const portBusy = ref(false)

// 组件明细。探测要几秒，所以单独拉，不跟着状态一起轮询。
const components = ref([])
const componentsError = ref('')
const componentsBusy = ref(false)
// 勾选要启动哪些组件。file-server 是本体，不给取消。
const startSelection = ref(['file-server'])
const selectionDirty = ref(false)
const selectionBusy = ref(false)

const sttFile = ref(null)
const sttText = ref('')
const sttBusy = ref(false)
const sttError = ref('')

// 探测没跑起来时的兜底清单，保证勾选框一直在。
const FALLBACK_COMPONENTS = [
  { key: 'file-server', name: '文件服务器', required: true },
  { key: 'mcp', name: 'MCP 网关' },
  { key: 'tts', name: '语音合成' },
  { key: 'stt', name: '语音转写' },
]

const componentList = computed(() =>
  components.value.length ? components.value : FALLBACK_COMPONENTS,
)

const startComponentNames = computed(() =>
  componentList.value
    .filter((c) => startSelection.value.includes(c.key))
    .map((c) => c.name),
)

const speakerOptions = computed(() => [
  { label: '（用默认音色）', value: '' },
  ...speakers.value.map((s) => ({ label: s, value: s })),
])

const logOptions = [
  { label: '文件服务器', value: 'file-server' },
  { label: '装依赖', value: 'install' },
]

let timer = null
let progressTimer = null
let logTimer = null

const statusRows = computed(() => [
  {
    key: '文件服务器',
    on: status.value.fileServer,
    note: status.value.fileServerPort
      ? `端口 ${status.value.fileServerPort} · ${status.value.fileServerUrl || ''}`
      : (status.value.fileServerUrl || ''),
  },
  {
    key: 'MCP 网关',
    on: status.value.mcpGateway,
    note: status.value.mcpPort
      ? `端口 ${status.value.mcpPort} · ${status.value.mcpUrl || ''}`
      : (status.value.mcpUrl || ''),
  },
  {
    key: '板端地址',
    on: Boolean(status.value.deviceHost),
    note: status.value.deviceHost
      ? `${status.value.deviceHost} · ${status.value.deviceFileServerUrl || ''} · ${status.value.deviceMcpUrl || ''}`
      : '（没设）',
  },
  { key: 'Python 环境', on: status.value.venvReady, note: '文件服务器的 .venv' },
  { key: 'TTS 权重', on: status.value.ttsReady, note: 'GPT-SoVITS 推理代码与音色权重' },
  {
    key: '启动组件',
    on: true,
    note: startComponentNames.value.join('、') || '（还没选）',
  },
])

const running = computed(() => jobs.value.filter((j) => j.status === 'running'))

const ttsPercent = computed(() => {
  const p = ttsProgress.value
  if (!p) {
    return 0
  }
  if (p.stage === 'done' || p.stage === 'error') {
    return 100
  }
  if (!p.total || !p.index) {
    return 0
  }
  return Math.min(100, Math.round((p.index / p.total) * 100))
})

// 引擎一次性交回整段音频，句子级进度要等它返回才有。中间这段时间用一条来回走的
// 条表示还在跑，配合服务端报的已用时长。
const ttsIndeterminate = computed(() => {
  const p = ttsProgress.value
  if (!p) {
    return false
  }
  if (p.stage !== 'running' && p.stage !== 'loading') {
    return false
  }
  return !p.index || !p.total
})

const ttsProgressText = computed(() => {
  const p = ttsProgress.value
  if (!p) {
    return ''
  }
  if (p.stage === 'done') {
    return p.message || '合成完成'
  }
  if (p.stage === 'error') {
    return `合成出错：${p.message || '未知原因'}`
  }
  if (p.stage === 'running' && p.index && p.total) {
    return `${p.message || '合成中'}，已生成 ${p.audioSeconds || 0} 秒音频`
  }
  return p.message || '模型加载中，第一次合成要等一会儿'
})

onMounted(() => {
  refreshStatus()
  refreshJobs()
  loadLog()
  loadComponents()
  timer = setInterval(refreshStatus, 5000)
  logTimer = setInterval(refreshLog, 3000)
})

onBeforeUnmount(() => {
  if (timer) clearInterval(timer)
  if (logTimer) clearInterval(logTimer)
  stopProgressPolling()
  if (audioUrl.value) URL.revokeObjectURL(audioUrl.value)
})

function report(e) {
  error.value = e.message
  notice.value = ''
  message.error(e.message)
}

async function refreshStatus() {
  try {
    status.value = await api.opsStatus()
    // 用户正在改端口的时候不要被轮询刷回去
    if (!fileServerPortDirty.value && status.value.fileServerPort) {
      fileServerPortInput.value = status.value.fileServerPort
    }
    if (!mcpPortDirty.value && status.value.mcpPort) {
      mcpPortInput.value = status.value.mcpPort
    }
    if (!deviceHostDirty.value && status.value.deviceHost) {
      deviceHostInput.value = status.value.deviceHost
    }
    // 同理，勾选没保存之前也不刷回去
    if (!selectionDirty.value && Array.isArray(status.value.startComponents)) {
      startSelection.value = [...status.value.startComponents]
    }
  } catch (e) {
    error.value = e.message
  }
}

// 组件依赖明细。探测要几秒到几十秒，单独一个按钮触发。
async function loadComponents(refresh = false) {
  componentsBusy.value = true
  componentsError.value = ''
  try {
    const data = await api.opsComponents(refresh)
    components.value = data.components || []
    if (data.error) {
      componentsError.value = data.error
    }
  } catch (e) {
    componentsError.value = e.message
  } finally {
    componentsBusy.value = false
  }
}

// 勾选要启动的组件，存到宿主级设置里，下次启动生效。
async function saveComponents() {
  error.value = ''
  notice.value = ''
  selectionBusy.value = true
  try {
    const data = await api.opsSaveSettings({ startComponents: [...startSelection.value] })
    selectionDirty.value = false
    notice.value = data.message || '启动组件已保存'
    message.success(notice.value)
    await refreshStatus()
  } catch (e) {
    report(e)
  } finally {
    selectionBusy.value = false
  }
}

async function refreshJobs() {
  try {
    jobs.value = await api.opsJobs()
  } catch (e) {
    error.value = e.message
  }
}

async function install(target, components, label) {
  error.value = ''
  notice.value = ''
  busy.value = label
  try {
    const data = await api.opsInstall(target, components)
    notice.value = `${label}已开始，日志写到 .run/install.log`
    await refreshJobs()
    return await pollJob(data.jobId)
  } catch (e) {
    report(e)
    return false
  }
}

// 装依赖是后台任务，轮询到结束为止
async function pollJob(id) {
  for (;;) {
    await new Promise((resolve) => setTimeout(resolve, 2000))
    let job
    try {
      job = await api.opsJob(id)
    } catch (e) {
      error.value = e.message
      return
    }
    const index = jobs.value.findIndex((j) => j.id === id)
    if (index >= 0) {
      jobs.value[index] = job
    } else {
      jobs.value.unshift(job)
    }
    if (job.status === 'done' || job.status === 'failed') {
      notice.value = job.status === 'done'
        ? `${job.label}完成`
        : `${job.label}失败：${job.message}`
      refreshStatus()
      // 装完依赖依赖明细变了，重新探一次
      loadComponents(true)
      return job.status === 'done'
    }
  }
}

async function startServer() {
  error.value = ''
  notice.value = ''
  busy.value = 'start'
  try {
    const data = await api.opsStart()
    notice.value = data.message
    message.success(data.message)
    await refreshStatus()
  } catch (e) {
    report(e)
  } finally {
    busy.value = ''
  }
}

function checkPort(value, label) {
  const port = Number(value)
  if (!Number.isInteger(port) || port < 1 || port > 65535) {
    error.value = `${label}要填 1~65535 之间的整数`
    message.error(error.value)
    return null
  }
  return port
}

// 板端地址只收主机名或 IP，协议和端口由端口设置拼
function checkDeviceHost(value) {
  const host = String(value || '').trim()
  if (!host) {
    error.value = '板端地址不能为空'
    message.error(error.value)
    return null
  }
  if (host.includes('://') || host.includes('/') || host.includes(':') || host.includes(' ')) {
    error.value = '板端地址只填主机名或 IP，不要带协议、端口和路径'
    message.error(error.value)
    return null
  }
  return host
}

// 只发改过的端口与板端地址，没动的那项不动
async function savePorts() {
  error.value = ''
  notice.value = ''
  const body = {}
  if (fileServerPortDirty.value) {
    const port = checkPort(fileServerPortInput.value, '文件服务器端口')
    if (port === null) {
      return
    }
    body.fileServerPort = port
  }
  if (mcpPortDirty.value) {
    const port = checkPort(mcpPortInput.value, 'MCP 网关端口')
    if (port === null) {
      return
    }
    body.mcpPort = port
  }
  if (deviceHostDirty.value) {
    const host = checkDeviceHost(deviceHostInput.value)
    if (host === null) {
      return
    }
    body.deviceHost = host
  }
  if (!Object.keys(body).length) {
    notice.value = '端口没改'
    return
  }
  portBusy.value = true
  try {
    const data = await api.opsSaveSettings(body)
    fileServerPortInput.value = data.fileServerPort
    mcpPortInput.value = data.mcpPort
    if (data.deviceHost) {
      deviceHostInput.value = data.deviceHost
    }
    fileServerPortDirty.value = false
    mcpPortDirty.value = false
    deviceHostDirty.value = false
    notice.value = data.message || '端口已保存'
    message.success(notice.value)
    await refreshStatus()
  } catch (e) {
    report(e)
  } finally {
    portBusy.value = false
  }
}

async function stopServer() {
  error.value = ''
  notice.value = ''
  busy.value = 'stop'
  try {
    const data = await api.opsStop()
    notice.value = data.message
    message.success(data.message)
    await refreshStatus()
  } catch (e) {
    report(e)
  } finally {
    busy.value = ''
  }
}

async function loadSpeakers() {
  ttsError.value = ''
  try {
    const data = await api.opsSpeakers()
    speakers.value = data.speakers || []
    if (data.default && !ttsSpeaker.value) {
      ttsSpeaker.value = data.default
    }
  } catch (e) {
    ttsError.value = e.message
    speakers.value = []
  }
}

// 进度是文件服务器那边报的，本控制台只做转发。文件服务器没重启过就没有这个接口，
// 那就让合成照常走，只是看不到进度条。
async function pollProgress() {
  try {
    ttsProgress.value = await api.opsTtsProgress()
  } catch (e) {
    // 进度拿不到不算错，合成本身可能还是好的
  }
}

function stopProgressPolling() {
  if (progressTimer) {
    clearInterval(progressTimer)
    progressTimer = null
  }
}

async function synthesize() {
  ttsError.value = ''
  ttsBusy.value = true
  ttsProgress.value = { stage: 'loading', message: '准备合成……' }
  stopProgressPolling()
  progressTimer = setInterval(pollProgress, 700)
  try {
    const blob = await api.opsTts(ttsText.value, ttsSpeaker.value)
    if (audioUrl.value) URL.revokeObjectURL(audioUrl.value)
    audioUrl.value = URL.createObjectURL(blob)
  } catch (e) {
    ttsError.value = e.message
    ttsProgress.value = { stage: 'error', message: e.message }
  } finally {
    stopProgressPolling()
    ttsBusy.value = false
    // 收尾再拉一次，把最后一帧的进度留在页面上
    pollProgress()
  }
}

function pickFile(event) {
  sttFile.value = event.target.files[0] || null
  sttText.value = ''
  sttError.value = ''
}

async function transcribe() {
  sttError.value = ''
  if (!sttFile.value) {
    sttError.value = '先选一个音频文件'
    message.warning(sttError.value)
    return
  }
  sttBusy.value = true
  try {
    sttText.value = await api.opsStt(sttFile.value)
  } catch (e) {
    sttError.value = e.message
  } finally {
    sttBusy.value = false
  }
}

async function loadLog() {
  error.value = ''
  try {
    logLines.value = await api.opsLog(logName.value, 200)
    await scrollLogToEnd()
  } catch (e) {
    report(e)
  }
}

// 日志自动跟随：定时拉一次尾部，有新行就滚到底。
// 用户往上翻过就停跟随，滚回底部再跟上。
async function refreshLog() {
  try {
    const lines = await api.opsLog(logName.value, 200)
    const el = logBox.value
    const before = el ? el.scrollHeight : 0
    const last = lines.length ? lines[lines.length - 1] : ''
    const lastBefore = logLines.value.length ? logLines.value[logLines.value.length - 1] : ''
    if (lines.length === logLines.value.length && last === lastBefore) {
      return
    }
    logLines.value = lines
    await nextTick()
    if (!el) {
      return
    }
    if (followLog.value) {
      el.scrollTop = el.scrollHeight
    } else {
      // 尾部挤掉了几行，把滚动位置补回去，视线不要跳
      el.scrollTop += el.scrollHeight - before
    }
  } catch (e) {
    // 日志拉不到不算错，不打断页面
  }
}

async function scrollLogToEnd() {
  await nextTick()
  const el = logBox.value
  if (el) {
    el.scrollTop = el.scrollHeight
  }
}

function onLogScroll() {
  const el = logBox.value
  if (!el) {
    return
  }
  // 离底部 16 像素以内就算贴着底部
  followLog.value = el.scrollHeight - el.scrollTop - el.clientHeight < 16
}

function jumpToLatest() {
  followLog.value = true
  scrollLogToEnd()
}
</script>

<template>
  <div>
    <div class="page-head">
      <div>
        <h2>运维</h2>
        <div class="muted">端口探活为准，不看是不是本控制台起的进程。</div>
      </div>
      <div class="spacer"></div>
      <NSpace>
        <NButton secondary @click="refreshStatus">
          <template #icon>
            <NIcon><RefreshOutline /></NIcon>
          </template>
          刷新状态
        </NButton>
        <NButton
          type="primary"
          :loading="busy === 'start'"
          :disabled="Boolean(busy)"
          @click="startServer"
        >
          <template #icon>
            <NIcon><PlayOutline /></NIcon>
          </template>
          按勾选启动
        </NButton>
        <NButton
          type="error"
          secondary
          :loading="busy === 'stop'"
          :disabled="Boolean(busy)"
          @click="stopServer"
        >
          <template #icon>
            <NIcon><StopOutline /></NIcon>
          </template>
          停止
        </NButton>
      </NSpace>
    </div>

    <NAlert v-if="error" type="error" class="alert" closable @close="error = ''">
      {{ error }}
    </NAlert>
    <NAlert v-if="notice" type="success" class="alert" closable @close="notice = ''">
      {{ notice }}
    </NAlert>

    <NCard title="服务地址与端侧接入" size="small" class="block">
      <div class="endpoint-list">
        <div><strong>文件服务器</strong><code>{{ status.fileServerUrl || '—' }}</code></div>
        <div><strong>MCP 网关</strong><code>{{ status.mcpUrl || '—' }}</code></div>
        <div><strong>EMQX MQTT</strong><code>{{ status.emqxUrl || '—' }}</code></div>
        <div><strong>EMQX 管理台（服务器本机）</strong><a :href="status.emqxDashboardUrl" target="_blank" rel="noopener">{{ status.emqxDashboardUrl || '—' }}</a></div>
      </div>
      <p>端侧自动分发地址</p>
      <div class="endpoint-list">
        <div><strong>文件服务器</strong><code>{{ status.deviceFileServerUrl || '—' }}</code></div>
        <div><strong>MCP 网关</strong><code>{{ status.deviceMcpUrl || '—' }}</code></div>
        <div><strong>EMQX MQTT</strong><code>{{ status.deviceEmqxUrl || '—' }}</code></div>
      </div>
      <p class="muted">在端侧 menuconfig → External Server 中，文件服务器、MCP 和 EMQX 各自选择 Manual 或 Auto。Manual 显示 URL 输入项；Auto 使用公共 Web 控制台 URL，从 /api/ops/endpoints 获取对应地址。下方“板端地址”决定分发给设备的主机地址。</p>
    </NCard>
    <nav class="section-nav" aria-label="运维分区">
      <a href="#ops-components">组件与依赖</a>
      <a href="#ops-services">服务部署</a>
      <a href="#ops-tts">语音测试</a>
      <a href="#ops-logs">运行日志</a>
    </nav>
    <div id="ops-components" class="ops-grid">
    <NCard title="组件状态" size="small" class="block">
      <div class="stat-grid">
        <div v-for="row in statusRows" :key="row.key" class="stat">
          <div class="stat-head">
            <NIcon v-if="row.on" :size="16" class="ok"><CheckmarkCircleOutline /></NIcon>
            <NIcon v-else :size="16" class="off"><CloseCircleOutline /></NIcon>
            <span class="stat-name">{{ row.key }}</span>
            <NTag size="small" :bordered="false" :type="row.on ? 'success' : 'default'">
              {{ row.on ? '就绪' : '未就绪' }}
            </NTag>
          </div>
          <div class="stat-note mono">{{ row.note }}</div>
        </div>
      </div>
    </NCard>

    <NCard title="安装依赖" size="small" class="block">
      <NText depth="3" class="tip">勾选需要安装的组件后统一执行。服务组件会自动安装文件服务器基础依赖；语音合成首次需下载数 GB 模型。</NText>
      <NCheckboxGroup v-model:value="installComponents" :disabled="installLocked">
        <div class="dependency-options">
          <NCheckbox value="emqx">EMQX MQTT 服务</NCheckbox>
          <NCheckbox value="web">Web 前端</NCheckbox>
          <NCheckbox value="file-server">文件服务器</NCheckbox>
          <NCheckbox value="mcp">MCP 网关</NCheckbox>
          <NCheckbox value="tts">语音合成 TTS</NCheckbox>
          <NCheckbox value="stt">语音转写 STT</NCheckbox>
        </div>
      </NCheckboxGroup>
      <div class="row-actions">
        <NButton type="primary" :disabled="installLocked || !installComponents.length"
          :loading="Boolean(busy)" @click="installSelected">
          <template #icon><NIcon><DownloadOutline /></NIcon></template>
          安装选中依赖
        </NButton>
        <NText depth="3">{{ installLocked ? '安装任务执行中，请等待完成' : '已选择 ' + installComponents.length + ' 个组件' }}</NText>
      </div>

      <NTable v-if="jobs.length" :bordered="false" size="small" class="jobs">
        <thead>
          <tr>
            <th>任务</th>
            <th style="width: 100px">状态</th>
            <th>说明</th>
            <th style="width: 90px">退出码</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="job in jobs" :key="job.id">
            <td>{{ job.label }}</td>
            <td>
              <NTag
                size="small"
                :bordered="false"
                :type="job.status === 'done' ? 'success' : (job.status === 'failed' ? 'error' : 'warning')"
              >
                {{ job.status }}
              </NTag>
            </td>
            <td class="muted">{{ job.message }}</td>
            <td class="mono">{{ job.exitCode === null ? '—' : job.exitCode }}</td>
          </tr>
        </tbody>
      </NTable>
    </NCard>

    </div>

    <NCard title="组件依赖明细" size="small" class="block">
      <template #header-extra>
        <NButton size="small" secondary :loading="componentsBusy" @click="loadComponents(true)">
          <template #icon>
            <NIcon><RefreshOutline /></NIcon>
          </template>
          重新探测
        </NButton>
      </template>

      <NText depth="3" class="tip">
        探测 file_server/components.py 的结果，只看文件和已装的包，不下载任何东西。
        探测要起子进程，第一次可能要几十秒。
      </NText>

      <NAlert v-if="componentsError" type="warning" class="alert" :show-icon="false">
        {{ componentsError }}
      </NAlert>

      <div v-for="c in componentList" :key="c.key" class="comp">
        <div class="comp-head">
          <span class="comp-name">{{ c.name }}</span>
          <NTag size="small" :bordered="false" :type="c.ready ? 'success' : 'error'">
            {{ c.ready ? '就绪' : '缺依赖' }}
          </NTag>
        </div>
        <ul v-if="c.deps && c.deps.length" class="dep-list">
          <li
            v-for="d in c.deps"
            :key="d.name"
            :class="{ missing: !d.ok, optional: !d.required }"
          >
            <span class="dep-mark">{{ d.ok ? '✓' : (d.required ? '✗' : '○') }}</span>
            <span class="dep-name">{{ d.name }}</span>
            <span class="mono muted">{{ d.detail }}</span>
          </li>
        </ul>
        <div v-else class="muted">没有可探测的依赖。</div>
      </div>
    </NCard>

    <NCard title="EMQX MQTT 服务" size="small" class="block">
      <template #header-extra><NTag :type="emqx.running ? 'success' : 'default'">{{ emqx.running ? '运行中' : '未运行' }}</NTag></template>
      <p>{{ emqx.platform }} · {{ emqx.arch }} · 安装方式 {{ emqx.method }}</p>
      <NAlert v-if="emqx.message" type="warning">{{ emqx.message }}</NAlert>
      <p class="muted">MQTT 端口 1883 · 管理台端口 18083 · 数据使用持久化卷。勾选安装 EMQX 时会自动补装 Docker，系统可能要求管理员授权或重启。</p>
      <NSpace>
        <NButton type="primary" :disabled="!emqx.runtimeReady || installLocked || emqxBusy" @click="emqxAction('start')">启动 EMQX</NButton>
        <NButton :disabled="!emqx.running || emqxBusy" @click="emqxAction('stop')">停止</NButton>
        <NButton :disabled="emqxBusy" @click="refreshEmqx">刷新状态</NButton>
        <NButton tag="a" href="http://127.0.0.1:18083" target="_blank" rel="noopener" :disabled="!emqx.running">打开本机管理台</NButton>
        <NButton tag="a" href="https://docs.docker.com/get-started/get-docker/" target="_blank" rel="noopener">安装 Docker</NButton>
      </NSpace>
    </NCard>

    <NCard id="ops-services" title="文件服务器与 MCP 网关" size="small" class="block">
      <NText depth="3" class="tip">
        启动走 file_server/deploy 脚本，只起勾选的组件：没勾的组件不注册端点，调用它会返回 404。
        停止会先杀本控制台起的进程，还占着端口的话按端口反查 pid 再杀一次，连 MCP 网关一起带走。
      </NText>

      <div class="section">
        <div class="section-label">启动哪些组件</div>
        <NCheckboxGroup
          v-model:value="startSelection"
          class="checks"
          @update:value="selectionDirty = true"
        >
          <NCheckbox
            v-for="c in componentList"
            :key="c.key"
            :value="c.key"
            :disabled="c.required"
          >
            {{ c.name }}
          </NCheckbox>
        </NCheckboxGroup>
        <div class="row-actions">
          <span class="muted">文件服务器是本体，不能关。勾选保存后，下次启动生效。</span>
          <div class="spacer"></div>
          <NButton
            size="small"
            type="primary"
            secondary
            :loading="selectionBusy"
            :disabled="!selectionDirty"
            @click="saveComponents"
          >
            <template #icon>
              <NIcon><SaveOutline /></NIcon>
            </template>
            保存勾选
          </NButton>
        </div>
      </div>

      <div class="grid-2 section">
        <div>
          <div class="section-label">文件服务器端口</div>
          <NInputNumber
            v-model:value="fileServerPortInput"
            :min="1"
            :max="65535"
            :show-button="false"
            style="width: 140px"
            @update:value="fileServerPortDirty = true"
          />
          <div class="muted">
            改端口前要先停掉文件服务器，保存后下次启动生效。MCP 网关会自动跟着指向新端口。
          </div>
        </div>
        <div>
          <div class="section-label">MCP 网关端口</div>
          <NInputNumber
            v-model:value="mcpPortInput"
            :min="1"
            :max="65535"
            :show-button="false"
            style="width: 140px"
            @update:value="mcpPortDirty = true"
          />
          <div class="muted">
            板端用这个端口连网关。改完保存，下次启动文件服务器时连带生效，两个端口不能填一样。
          </div>
        </div>
        <div>
          <div class="section-label">板端地址</div>
          <NInput
            v-model:value="deviceHostInput"
            placeholder="192.168.137.1"
            style="width: 180px"
            @update:value="deviceHostDirty = true"
          />
          <div class="muted">
            板子访问文件服务器与 MCP 网关用的主机名，只填主机名或 IP。QEMU 走 TAP 网桥时填
            192.168.137.1，真板子填局域网 IP。固件开了自动获取地址时，板端就按这个来连。
          </div>
        </div>
      </div>

      <div class="row-actions">
        <NButton size="small" secondary :loading="portBusy" @click="savePorts">
          <template #icon>
            <NIcon><SaveOutline /></NIcon>
          </template>
          保存端口
        </NButton>
      </div>
    </NCard>

    <NCard id="ops-tts" title="语音合成试听" size="small" class="block">
      <NText depth="3" class="tip">
        需要文件服务器在跑，并且装过 TTS 依赖与权重。没装的话合成会返回 503。
      </NText>
      <NAlert v-if="ttsError" type="error" class="alert" :show-icon="false">
        {{ ttsError }}
      </NAlert>

      <div class="section">
        <div class="section-label">文本</div>
        <NInput v-model:value="ttsText" type="textarea" :rows="3" />
      </div>

      <div class="section">
        <div class="section-label">音色</div>
        <NSpace align="center">
          <NSelect
            v-model:value="ttsSpeaker"
            :options="speakerOptions"
            style="width: 240px"
          />
          <NButton size="small" secondary @click="loadSpeakers">读取音色列表</NButton>
        </NSpace>
      </div>

      <div class="row-actions">
        <NButton type="primary" :loading="ttsBusy" @click="synthesize">
          <template #icon>
            <NIcon><PlayOutline /></NIcon>
          </template>
          合成并播放
        </NButton>
      </div>

      <div v-if="ttsProgress" class="section">
        <NProgress
          type="line"
          :percentage="ttsPercent"
          :indeterminate="ttsIndeterminate"
          :status="ttsProgress.stage === 'error' ? 'error' : (ttsProgress.stage === 'done' ? 'success' : 'default')"
          :processing="ttsProgress.stage === 'running' || ttsProgress.stage === 'loading'"
        />
        <div class="muted">{{ ttsProgressText }}</div>
      </div>

      <audio v-if="audioUrl" :src="audioUrl" controls class="player"></audio>
    </NCard>

    <NCard title="语音转写" size="small" class="block">
      <NText depth="3" class="tip">
        选一个音频文件丢过来，服务端转成文本。依赖文件服务器上的 ASR 能力。
      </NText>
      <NAlert v-if="sttError" type="error" class="alert" :show-icon="false">
        {{ sttError }}
      </NAlert>

      <div class="section">
        <div class="section-label">音频文件</div>
        <input class="file-input" type="file" accept="audio/*" @change="pickFile" />
      </div>

      <div class="row-actions">
        <NButton type="primary" :loading="sttBusy" @click="transcribe">转写</NButton>
      </div>

      <div v-if="sttText" class="result">{{ sttText }}</div>
    </NCard>

    <NCard id="ops-logs" title="日志" size="small" class="block">
      <template #header-extra>
        <NSpace align="center">
          <NSelect v-model:value="logName" :options="logOptions" style="width: 150px" @update:value="loadLog" />
          <NButton size="small" secondary @click="loadLog">
            <template #icon>
              <NIcon><TerminalOutline /></NIcon>
            </template>
            读取日志尾部
          </NButton>
          <NButton v-if="!followLog" size="small" secondary @click="jumpToLatest">
            <template #icon>
              <NIcon><ArrowDownOutline /></NIcon>
            </template>
            回到最新
          </NButton>
        </NSpace>
      </template>

      <NText depth="3" class="tip">
        每 3 秒拉一次尾部，有新行就自动滚到底。往上翻过就停跟随，滚回底部或者点「回到最新」再跟上。
      </NText>

      <pre v-if="logLines.length" ref="logBox" class="log" @scroll="onLogScroll">{{ logLines.join('\n') }}</pre>
      <div v-else class="muted">还没有日志。</div>
    </NCard>
  </div>
</template>

<style scoped>
.endpoint-list { display: grid; gap: 12px; }
.endpoint-list > div { display: flex; flex-wrap: wrap; gap: 12px; justify-content: space-between; }
.endpoint-list code, .endpoint-list a { overflow-wrap: anywhere; color: #6366f1; }
.dependency-options { display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 16px; padding: 16px; background: #f7f8fc; border-radius: 12px; }
.section-nav {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin: 0 0 24px;
  padding: 6px;
  width: fit-content;
  background: #e9ecf5;
  border-radius: 12px;
}
.section-nav a {
  padding: 9px 18px;
  border-radius: 8px;
  text-decoration: none;
  color: #5b647e;
  font-weight: 500;
}
.section-nav a:hover, .section-nav a:focus-visible {
  background: white;
  color: #5759d6;
  box-shadow: 0 2px 6px #242c4a0a;
}
.block { scroll-margin-top: 24px; }

.ops-grid {
  display: grid;
  grid-template-columns: minmax(0, 2fr) minmax(0, 3fr);
  gap: 16px;
}

@media (max-width: 1100px) {
  .ops-grid {
    grid-template-columns: minmax(0, 1fr);
    gap: 0;
  }
}

.block {
  margin-bottom: 16px;
}

.alert {
  margin-bottom: 16px;
}

.tip {
  display: block;
  margin-bottom: 14px;
  font-size: 13px;
  line-height: 1.7;
}

.stat-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
  gap: 12px;
}

.stat {
  border: 1px solid #eef0f4;
  border-radius: 10px;
  padding: 12px 14px;
  background: #fafbfd;
}

.stat-head {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 6px;
}

.stat-head .ok {
  color: #12a150;
}

.stat-head .off {
  color: #b0b6c3;
}

.stat-name {
  font-weight: 500;
  flex: 1;
}

.stat-note {
  color: #8a93a6;
  font-size: 12px;
  word-break: break-all;
}

.jobs {
  margin-top: 16px;
}

.comp {
  border-top: 1px solid #eef0f4;
  padding: 12px 0;
}

.comp:first-of-type {
  border-top: none;
  padding-top: 0;
}

.comp-head {
  display: flex;
  align-items: center;
  gap: 10px;
  margin-bottom: 8px;
}

.comp-name {
  font-weight: 500;
}

.dep-list {
  margin: 0;
  padding: 0;
  list-style: none;
}

.dep-list li {
  display: flex;
  gap: 8px;
  align-items: baseline;
  font-size: 13px;
  padding: 2px 0;
}

.dep-list li.missing .dep-mark {
  color: #e5484d;
}

.dep-list li.optional .dep-mark {
  color: #8a93a6;
}

.dep-name {
  min-width: 150px;
}

.section {
  margin-top: 16px;
}

.section-label {
  font-size: 13px;
  color: #6b7280;
  margin-bottom: 8px;
}

.checks {
  display: flex;
  gap: 20px;
  flex-wrap: wrap;
}

.row-actions {
  display: flex;
  align-items: center;
  gap: 10px;
  margin-top: 12px;
  flex-wrap: wrap;
}

.row-actions .spacer {
  flex: 1;
}

.file-input {
  font-size: 13px;
  color: #6b7280;
}

.player {
  display: block;
  margin-top: 16px;
  width: 100%;
}

.result {
  margin-top: 12px;
  padding: 12px;
  background: #f7f8fa;
  border: 1px solid #eef0f4;
  border-radius: 8px;
  white-space: pre-wrap;
  line-height: 1.6;
}

.log {
  margin-top: 12px;
  padding: 12px;
  max-height: 340px;
  overflow: auto;
  background: #1b2130;
  color: #d5dae4;
  border-radius: 8px;
  font-family: Consolas, monospace;
  font-size: 12px;
  line-height: 1.6;
  white-space: pre-wrap;
}
</style>
