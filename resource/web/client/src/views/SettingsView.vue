<script setup>
import { ref, onMounted, computed } from 'vue'
import { api } from '../api.js'

const items = ref({})
const labels = ref({})
const defaults = ref({})
const form = ref({})
const error = ref('')
const notice = ref('')
const busy = ref(false)

// 密钥用密码框，系统提示词用多行
const secretKeys = new Set(['llm.apiKey'])
const longKeys = new Set(['agent.systemPrompt'])
const choiceKeys = new Set(['llm.protocol'])

const protocols = [
  { value: 'openai', label: 'OpenAI 兼容（POST /chat/completions）' },
  { value: 'anthropic', label: 'Anthropic（POST /v1/messages）' },
]

const hints = {
  'llm.protocol': '网站侧 agent 用哪种协议调模型，默认跟 rtconfig.h 里选中的那个',
  'llm.apiKey': '服务商的密钥，默认取 rtconfig.h 里的 PKG_AGENT_API_KEY',
  'llm.model': '模型名，默认取 rtconfig.h 里的 PKG_AGENT_MODEL_NAME',
  'llm.baseUrl': '服务根地址，不含协议路径，默认取 rtconfig.h 里当前协议对应的那份',
  'agent.systemPrompt': '留空就用内置的默认提示词',
}

const entries = computed(() =>
  Object.keys(labels.value).map((key) => ({
    key,
    label: labels.value[key],
    hint: hints[key] || '',
    secret: secretKeys.has(key),
    long: longKeys.has(key),
    choice: choiceKeys.has(key),
    fallback: defaults.value[key] || '',
  })),
)

onMounted(load)

function displayValue(key) {
  const value = form.value[key]
  if (!value) return '（空，用默认）'
  if (!secretKeys.has(key)) return value
  return value.length <= 4 ? '****' : `****${value.slice(-4)}`
}

async function load() {
  error.value = ''
  try {
    const data = await api.getSettings()
    items.value = data.items || {}
    labels.value = data.labels || {}
    defaults.value = data.defaults || {}
    form.value = { ...items.value }
  } catch (e) {
    error.value = e.message
  }
}

async function save() {
  error.value = ''
  notice.value = ''
  busy.value = true
  try {
    await api.saveSettings(form.value)
    notice.value = '已保存，下一次对话生效'
    await load()
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}

async function reset(key) {
  if (!confirm(`把「${labels.value[key] || key}」恢复成默认值？`)) return
  error.value = ''
  notice.value = ''
  try {
    await api.resetSetting(key)
    notice.value = '已恢复默认'
    await load()
  } catch (e) {
    error.value = e.message
  }
}
</script>

<template>
  <div class="panel">
    <h3>网站侧 agent 配置</h3>
    <p class="muted">
      按账号保存，存在服务端 SQLite 里，不进版本库。留空的项回退到 rtconfig.h
      （编译进端侧固件的那份配置），再回退到环境变量。这些配置只影响网站侧 agent，
      端侧设备的 LLM 配置在设备上单独设。
    </p>

    <p v-if="error" class="error">{{ error }}</p>
    <p v-if="notice" class="ok">{{ notice }}</p>

    <form @submit.prevent="save">
      <div v-for="f in entries" :key="f.key" class="field">
        <label :for="f.key">
          {{ f.label }}
          <span class="muted">（{{ f.key }}）</span>
        </label>

        <select v-if="f.choice" :id="f.key" v-model="form[f.key]" class="input">
          <option v-for="p in protocols" :key="p.value" :value="p.value">{{ p.label }}</option>
        </select>
        <textarea v-else-if="f.long" :id="f.key" v-model="form[f.key]" class="input"
                  rows="6" :placeholder="f.hint"></textarea>
        <input v-else-if="f.secret" :id="f.key" v-model="form[f.key]" class="input"
               type="password" autocomplete="off" :placeholder="f.hint" />
        <input v-else :id="f.key" v-model="form[f.key]" class="input" :placeholder="f.hint" />

        <div class="row-actions">
          <span class="muted">当前值：{{ displayValue(f.key) }}</span>
          <span v-if="f.fallback" class="muted">rtconfig.h 默认：{{ f.fallback }}</span>
          <button v-if="items[f.key]" class="btn small ghost" type="button"
                  @click="reset(f.key)">恢复默认</button>
        </div>
      </div>

      <div class="row-actions">
        <button class="btn" type="submit" :disabled="busy">
          {{ busy ? '保存中……' : '保存' }}
        </button>
        <button class="btn ghost" type="button" @click="load">还原</button>
      </div>
    </form>
  </div>

  <div class="panel">
    <h3>rtconfig.h 与环境变量</h3>
    <p class="muted">
      取值优先级：这里填的值 → 环境变量 → 工程 rtconfig.h。rtconfig.h 找不到时，
      服务端会按下面的路径去找；也可以用 AGENT_RTCONFIG_H 直接指定。
    </p>
    <table>
      <thead>
        <tr><th>来源</th><th>作用</th></tr>
      </thead>
      <tbody>
        <tr><td class="mono">PKG_AGENT_API_PROTOCOL_*</td><td>采用的协议</td></tr>
        <tr><td class="mono">PKG_AGENT_API_BASE_URL_*</td><td>协议地址</td></tr>
        <tr><td class="mono">PKG_AGENT_API_KEY</td><td>API Key</td></tr>
        <tr><td class="mono">PKG_AGENT_MODEL_NAME</td><td>模型名称</td></tr>
        <tr><td class="mono">AGENT_RTCONFIG_H</td><td>rtconfig.h 的路径，默认自动向上找</td></tr>
        <tr><td class="mono">AGENT_LLM_PROTOCOL</td><td>覆盖采用的协议</td></tr>
        <tr><td class="mono">AGENT_LLM_BASE_URL</td><td>覆盖协议地址</td></tr>
        <tr><td class="mono">AGENT_LLM_API_KEY</td><td>覆盖 API Key</td></tr>
        <tr><td class="mono">AGENT_LLM_MODEL</td><td>覆盖模型名</td></tr>
        <tr><td class="mono">AGENT_JWT_SECRET</td><td>登录令牌签名密钥，改了所有人要重新登录</td></tr>
        <tr><td class="mono">AGENT_CONSOLE_DB</td><td>SQLite 文件路径</td></tr>
      </tbody>
    </table>
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
  gap: 10px;
  align-items: center;
  margin-top: 6px;
  flex-wrap: wrap;
}
</style>
