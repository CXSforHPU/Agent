<script setup>
import { ref, onMounted, onBeforeUnmount, computed, nextTick } from 'vue'
import { api } from '../api.js'

const status = ref({})
const jobs = ref([])
const error = ref('')
const notice = ref('')
const busy = ref('')

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

const fileServerPortInput = ref('')
const fileServerPortDirty = ref(false)
const mcpPortInput = ref('')
const mcpPortDirty = ref(false)
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
}

async function refreshStatus() {
  try {
    status.value = await api.opsStatus()
    // 用户正在改端口的时候不要被轮询刷回去
    if (!fileServerPortDirty.value && status.value.fileServerPort) {
      fileServerPortInput.value = String(status.value.fileServerPort)
    }
    if (!mcpPortDirty.value && status.value.mcpPort) {
      mcpPortInput.value = String(status.value.mcpPort)
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
    pollJob(data.jobId)
  } catch (e) {
    report(e)
  } finally {
    busy.value = ''
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
      return
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
    return null
  }
  return port
}

// 只发改过的端口，没动的那项不动
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
  if (!Object.keys(body).length) {
    notice.value = '端口没改'
    return
  }
  portBusy.value = true
  try {
    const data = await api.opsSaveSettings(body)
    fileServerPortInput.value = String(data.fileServerPort)
    mcpPortInput.value = String(data.mcpPort)
    fileServerPortDirty.value = false
    mcpPortDirty.value = false
    notice.value = data.message || '端口已保存'
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
  <div class="panel">
    <h3>组件状态</h3>
    <p class="muted">
      端口探活为准，不看是不是本控制台起的进程。装了依赖、起了服务，这里会立刻变。
    </p>
    <p v-if="error" class="error">{{ error }}</p>
    <p v-if="notice" class="ok">{{ notice }}</p>

    <table>
      <thead>
        <tr><th>组件</th><th>状态</th><th>说明</th></tr>
      </thead>
      <tbody>
        <tr v-for="row in statusRows" :key="row.key">
          <td>{{ row.key }}</td>
          <td>
            <span class="dot" :class="{ on: row.on }"></span>
            {{ row.on ? '就绪' : '未就绪' }}
          </td>
          <td class="mono muted">{{ row.note }}</td>
        </tr>
      </tbody>
    </table>

    <div class="row-actions">
      <button class="btn ghost small" @click="refreshStatus">刷新状态</button>
    </div>
  </div>

  <div class="panel">
    <h3>安装依赖</h3>
    <p class="muted">
      装依赖是后台任务，进度写到 .run/install.log。按组件分开装：语音合成那一条会拉
      GPT-SoVITS 的推理代码、预训练模型和音色权重，第一次要下载几个 GB。
    </p>

    <div class="row-actions">
      <button class="btn" :disabled="Boolean(busy)" @click="install('web', [], '安装前端依赖')">
        {{ busy === '安装前端依赖' ? '安装中……' : '安装前端依赖' }}
      </button>
      <button class="btn" :disabled="Boolean(busy)" @click="install('file-server', [], '安装文件服务器依赖')">
        {{ busy === '安装文件服务器依赖' ? '安装中……' : '安装文件服务器依赖' }}
      </button>
      <button class="btn ghost" :disabled="Boolean(busy)" @click="install('file-server', ['mcp'], '安装 MCP 网关依赖')">
        {{ busy === '安装 MCP 网关依赖' ? '安装中……' : '安装 MCP 网关依赖' }}
      </button>
      <button class="btn ghost" :disabled="Boolean(busy)" @click="install('file-server', ['tts'], '安装语音合成依赖')">
        {{ busy === '安装语音合成依赖' ? '安装中……' : '安装语音合成依赖' }}
      </button>
      <button class="btn ghost" :disabled="Boolean(busy)" @click="install('file-server', ['stt'], '安装语音转写依赖')">
        {{ busy === '安装语音转写依赖' ? '安装中……' : '安装语音转写依赖' }}
      </button>
    </div>

    <table v-if="jobs.length" class="jobs">
      <thead>
        <tr><th>任务</th><th>状态</th><th>说明</th><th>退出码</th></tr>
      </thead>
      <tbody>
        <tr v-for="job in jobs" :key="job.id">
          <td>{{ job.label }}</td>
          <td>{{ job.status }}</td>
          <td class="muted">{{ job.message }}</td>
          <td class="mono">{{ job.exitCode === null ? '—' : job.exitCode }}</td>
        </tr>
      </tbody>
    </table>
  </div>

  <div class="panel">
    <h3>组件依赖明细</h3>
    <p class="muted">
      探测 file_server/components.py 的结果，只看文件和已装的包，不下载任何东西。
      探测要起子进程，第一次可能要几十秒。
    </p>
    <p v-if="componentsError" class="error">{{ componentsError }}</p>

    <div class="row-actions">
      <button class="btn ghost small" :disabled="componentsBusy" @click="loadComponents(true)">
        {{ componentsBusy ? '探测中……' : '重新探测' }}
      </button>
    </div>

    <table class="deps">
      <thead>
        <tr><th>组件</th><th>状态</th><th>依赖</th></tr>
      </thead>
      <tbody>
        <tr v-for="c in componentList" :key="c.key">
          <td>{{ c.name }}</td>
          <td>
            <span class="dot" :class="{ on: c.ready }"></span>
            {{ c.ready ? '就绪' : '缺依赖' }}
          </td>
          <td>
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
            <span v-else class="muted">没有可探测的依赖。</span>
          </td>
        </tr>
      </tbody>
    </table>
  </div>

  <div class="panel">
    <h3>文件服务器与 MCP 网关</h3>
    <p class="muted">
      启动走 file_server/deploy 脚本，只起勾选的组件：没勾的组件不注册端点，调用它会返回 404。
      停止会先杀本控制台起的进程，还占着端口的话按端口反查 pid 再杀一次，连 MCP 网关一起带走。
    </p>

    <div class="field">
      <label>启动哪些组件</label>
      <div class="row-actions">
        <label v-for="c in componentList" :key="c.key" class="check">
          <input
            type="checkbox"
            :value="c.key"
            v-model="startSelection"
            :disabled="c.required"
            @change="selectionDirty = true"
          />
          {{ c.name }}
        </label>
      </div>
      <p class="muted">
        文件服务器是本体，不能关。勾选保存后，下次启动生效。
      </p>
      <div class="row-actions">
        <button class="btn ghost small" :disabled="selectionBusy || !selectionDirty" @click="saveComponents">
          {{ selectionBusy ? '保存中……' : '保存勾选' }}
        </button>
      </div>
    </div>

    <div class="row-actions">
      <button class="btn" :disabled="Boolean(busy)" @click="startServer">
        {{ busy === 'start' ? '启动中……' : '按勾选启动' }}
      </button>
      <button class="btn danger" :disabled="Boolean(busy)" @click="stopServer">
        {{ busy === 'stop' ? '停止中……' : '停止' }}
      </button>
    </div>

    <div class="field">
      <label for="ops-port">文件服务器端口</label>
      <div class="row-actions">
        <input
          id="ops-port"
          v-model="fileServerPortInput"
          class="input inline port"
          type="number"
          min="1"
          max="65535"
          @input="fileServerPortDirty = true"
        />
      </div>
      <p class="muted">
        改端口前要先停掉文件服务器，保存后下次启动生效。MCP 网关会自动跟着指向新端口。
      </p>
    </div>

    <div class="field">
      <label for="ops-mcp-port">MCP 网关端口</label>
      <div class="row-actions">
        <input
          id="ops-mcp-port"
          v-model="mcpPortInput"
          class="input inline port"
          type="number"
          min="1"
          max="65535"
          @input="mcpPortDirty = true"
        />
      </div>
      <p class="muted">
        板端用这个端口连网关。改完保存，下次启动文件服务器时连带生效，两个端口不能填一样。
      </p>
    </div>

    <div class="row-actions">
      <button class="btn ghost small" :disabled="portBusy" @click="savePorts">
        {{ portBusy ? '保存中……' : '保存端口' }}
      </button>
    </div>
  </div>

  <div class="panel">
    <h3>语音合成试听</h3>
    <p class="muted">
      需要文件服务器在跑，并且装过 TTS 依赖与权重。没装的话合成会返回 503。
    </p>
    <p v-if="ttsError" class="error">{{ ttsError }}</p>

    <div class="field">
      <label for="tts-text">文本</label>
      <textarea id="tts-text" v-model="ttsText" class="input" rows="3"></textarea>
    </div>

    <div class="field">
      <label for="tts-speaker">音色</label>
      <div class="row-actions">
        <select id="tts-speaker" v-model="ttsSpeaker" class="input inline">
          <option value="">（用默认音色）</option>
          <option v-for="s in speakers" :key="s" :value="s">{{ s }}</option>
        </select>
        <button class="btn ghost small" @click="loadSpeakers">读取音色列表</button>
      </div>
    </div>

    <div class="row-actions">
      <button class="btn" :disabled="ttsBusy" @click="synthesize">
        {{ ttsBusy ? '合成中……' : '合成并播放' }}
      </button>
    </div>

    <div v-if="ttsProgress" class="progress-wrap">
      <div class="progress-bar">
        <div
          class="progress-fill"
          :class="{ indeterminate: ttsIndeterminate }"
          :style="ttsIndeterminate ? {} : { width: ttsPercent + '%' }"
        ></div>
      </div>
      <p class="muted">{{ ttsProgressText }}</p>
    </div>

    <audio v-if="audioUrl" :src="audioUrl" controls class="player"></audio>
  </div>

  <div class="panel">
    <h3>语音转写</h3>
    <p class="muted">
      选一个音频文件丢过来，服务端转成文本。依赖文件服务器上的 ASR 能力。
    </p>
    <p v-if="sttError" class="error">{{ sttError }}</p>

    <div class="field">
      <label for="stt-file">音频文件</label>
      <input id="stt-file" class="input" type="file" accept="audio/*" @change="pickFile" />
    </div>

    <div class="row-actions">
      <button class="btn" :disabled="sttBusy" @click="transcribe">
        {{ sttBusy ? '转写中……' : '转写' }}
      </button>
    </div>

    <p v-if="sttText" class="result">{{ sttText }}</p>
  </div>

  <div class="panel">
    <h3>日志</h3>
    <p class="muted">
      每 3 秒拉一次尾部，有新行就自动滚到底。往上翻过就停跟随，滚回底部或者点「回到最新」再跟上。
    </p>
    <div class="row-actions">
      <select v-model="logName" class="input inline" @change="loadLog">
        <option value="file-server">文件服务器</option>
        <option value="install">装依赖</option>
      </select>
      <button class="btn ghost small" @click="loadLog">读取日志尾部</button>
      <button v-if="!followLog" class="btn ghost small" @click="jumpToLatest">回到最新</button>
    </div>

    <pre v-if="logLines.length" ref="logBox" class="log" @scroll="onLogScroll">{{ logLines.join('\n') }}</pre>
    <p v-else class="muted">还没有日志。</p>
  </div>
</template>

<style scoped>
.muted {
  color: var(--muted);
  font-size: 13px;
}

.mono {
  font-family: Consolas, monospace;
  font-size: 13px;
}

.row-actions {
  display: flex;
  gap: 8px;
  align-items: center;
  margin-top: 8px;
  flex-wrap: wrap;
}

.input.inline {
  width: auto;
  min-width: 220px;
}

.input.port {
  min-width: 120px;
  width: 120px;
}

.progress-wrap {
  margin-top: 12px;
}

.progress-bar {
  height: 8px;
  border-radius: 4px;
  background: var(--bg);
  overflow: hidden;
}

.progress-fill {
  height: 100%;
  background: var(--primary);
  transition: width 0.25s ease;
}

.progress-fill.indeterminate {
  width: 35%;
  animation: progress-slide 1.4s ease-in-out infinite;
}

@keyframes progress-slide {
  0% {
    margin-left: -35%;
  }
  100% {
    margin-left: 100%;
  }
}

.jobs {
  margin-top: 14px;
}

.check {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  font-size: 13px;
}

.deps {
  margin-top: 12px;
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
  color: var(--danger, #e06c75);
}

.dep-list li.optional .dep-mark {
  color: var(--muted);
}

.dep-name {
  min-width: 150px;
}

.player {
  display: block;
  margin-top: 12px;
  width: 100%;
}

.result {
  margin-top: 12px;
  padding: 10px;
  background: var(--bg);
  border-radius: 6px;
  white-space: pre-wrap;
}

.log {
  margin-top: 12px;
  padding: 10px;
  max-height: 320px;
  overflow: auto;
  background: #1f2430;
  color: #e3e6ea;
  border-radius: 6px;
  font-family: Consolas, monospace;
  font-size: 12px;
  line-height: 1.5;
  white-space: pre-wrap;
}
</style>
