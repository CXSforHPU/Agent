<script setup>
import { ref, onMounted, onBeforeUnmount, computed } from 'vue'
import { api } from '../api.js'

const status = ref({})
const jobs = ref([])
const error = ref('')
const notice = ref('')
const busy = ref('')

const logName = ref('file-server')
const logLines = ref([])

const ttsText = ref('这是 Agent 管理控制台合成的语音。')
const ttsSpeaker = ref('')
const speakers = ref([])
const audioUrl = ref('')
const ttsBusy = ref(false)
const ttsError = ref('')
const ttsProgress = ref(null)

const portInput = ref('')
const portDirty = ref(false)
const portBusy = ref(false)

const sttFile = ref(null)
const sttText = ref('')
const sttBusy = ref(false)
const sttError = ref('')

let timer = null
let progressTimer = null

const statusRows = computed(() => [
  { key: '文件服务器', on: status.value.fileServer, note: status.value.fileServerUrl || '' },
  { key: 'MCP 网关', on: status.value.mcpGateway, note: status.value.mcpUrl || '' },
  { key: 'Python 环境', on: status.value.venvReady, note: '文件服务器的 .venv' },
  { key: 'TTS 权重', on: status.value.ttsReady, note: 'GPT-SoVITS 推理代码与音色权重' },
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
  timer = setInterval(refreshStatus, 5000)
})

onBeforeUnmount(() => {
  if (timer) clearInterval(timer)
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
    if (!portDirty.value && status.value.fileServerPort) {
      portInput.value = String(status.value.fileServerPort)
    }
  } catch (e) {
    error.value = e.message
  }
}

async function refreshJobs() {
  try {
    jobs.value = await api.opsJobs()
  } catch (e) {
    error.value = e.message
  }
}

async function install(target, withTts, label) {
  error.value = ''
  notice.value = ''
  busy.value = label
  try {
    const data = await api.opsInstall(target, withTts)
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

async function savePort() {
  error.value = ''
  notice.value = ''
  const port = Number(portInput.value)
  if (!Number.isInteger(port) || port < 1 || port > 65535) {
    error.value = '端口要填 1~65535 之间的整数'
    return
  }
  portBusy.value = true
  try {
    const data = await api.opsSaveSettings({ port })
    portInput.value = String(data.fileServerPort)
    portDirty.value = false
    notice.value = data.message || `端口改成 ${data.fileServerPort}`
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
  } catch (e) {
    report(e)
  }
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
      装依赖是后台任务，进度写到 .run/install.log。TTS 那条会拉 GPT-SoVITS 的推理代码、
      预训练模型和音色权重，第一次要下载几个 GB。
    </p>

    <div class="row-actions">
      <button class="btn" :disabled="Boolean(busy)" @click="install('web', false, '安装前端依赖')">
        {{ busy === '安装前端依赖' ? '安装中……' : '安装前端依赖' }}
      </button>
      <button class="btn" :disabled="Boolean(busy)" @click="install('file-server', false, '安装文件服务器依赖')">
        {{ busy === '安装文件服务器依赖' ? '安装中……' : '安装文件服务器依赖' }}
      </button>
      <button class="btn ghost" :disabled="Boolean(busy)" @click="install('file-server', true, '安装文件服务器依赖（含 TTS）')">
        {{ busy === '安装文件服务器依赖（含 TTS）' ? '安装中……' : '安装文件服务器依赖（含 TTS）' }}
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
    <h3>文件服务器与 MCP 网关</h3>
    <p class="muted">
      启动走 file_server/deploy 脚本，会连带把 MCP 网关拉起来。停止会先杀本控制台起的进程，
      还占着端口的话按端口反查 pid 再杀一次，连 MCP 网关一起带走。
    </p>

    <div class="row-actions">
      <button class="btn" :disabled="Boolean(busy)" @click="startServer">
        {{ busy === 'start' ? '启动中……' : '启动文件服务器与 MCP 网关' }}
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
          v-model="portInput"
          class="input inline port"
          type="number"
          min="1"
          max="65535"
          @input="portDirty = true"
        />
        <button class="btn ghost small" :disabled="portBusy" @click="savePort">
          {{ portBusy ? '保存中……' : '保存端口' }}
        </button>
      </div>
      <p class="muted">
        改端口前要先停掉文件服务器，保存后下次启动生效。MCP 网关会自动跟着指向新端口。
      </p>
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
    <div class="row-actions">
      <select v-model="logName" class="input inline">
        <option value="file-server">文件服务器</option>
        <option value="install">装依赖</option>
      </select>
      <button class="btn ghost small" @click="loadLog">读取日志尾部</button>
    </div>

    <pre v-if="logLines.length" class="log">{{ logLines.join('\n') }}</pre>
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
