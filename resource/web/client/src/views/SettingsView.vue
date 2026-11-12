<script setup>
import { ref, onMounted, computed } from 'vue'
import {
  NCard,
  NForm,
  NFormItem,
  NInput,
  NSelect,
  NButton,
  NIcon,
  NTag,
  NText,
  NAlert,
  NSpace,
  NTable,
  useMessage,
} from 'naive-ui'
import { RefreshOutline, SaveOutline, ArrowUndoOutline } from '@vicons/ionicons5'
import { api } from '../api.js'

const message = useMessage()

const items = ref({})
const labels = ref({})
const defaults = ref({})
const form = ref({})
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

const sources = [
  { key: 'PKG_AGENT_API_PROTOCOL_*', desc: '采用的协议' },
  { key: 'PKG_AGENT_API_BASE_URL_*', desc: '协议地址' },
  { key: 'PKG_AGENT_API_KEY', desc: 'API Key' },
  { key: 'PKG_AGENT_MODEL_NAME', desc: '模型名称' },
  { key: 'AGENT_RTCONFIG_H', desc: 'rtconfig.h 的路径，默认自动向上找' },
  { key: 'AGENT_LLM_PROTOCOL', desc: '覆盖采用的协议' },
  { key: 'AGENT_LLM_BASE_URL', desc: '覆盖协议地址' },
  { key: 'AGENT_LLM_API_KEY', desc: '覆盖 API Key' },
  { key: 'AGENT_LLM_MODEL', desc: '覆盖模型名' },
  { key: 'AGENT_JWT_SECRET', desc: '登录令牌签名密钥，改了所有人要重新登录' },
  { key: 'AGENT_CONSOLE_DB', desc: 'SQLite 文件路径' },
]

onMounted(load)

function displayValue(key) {
  const value = form.value[key]
  if (!value) return '（空，用默认）'
  if (!secretKeys.has(key)) return value
  return value.length <= 4 ? '****' : `****${value.slice(-4)}`
}

async function load() {
  try {
    const data = await api.getSettings()
    items.value = data.items || {}
    labels.value = data.labels || {}
    defaults.value = data.defaults || {}
    form.value = { ...items.value }
  } catch (e) {
    message.error(e.message)
  }
}

async function save() {
  busy.value = true
  try {
    await api.saveSettings(form.value)
    message.success('已保存，下一次对话生效')
    await load()
  } catch (e) {
    message.error(e.message)
  } finally {
    busy.value = false
  }
}

async function reset(key) {
  try {
    await api.resetSetting(key)
    message.success('已恢复默认')
    await load()
  } catch (e) {
    message.error(e.message)
  }
}
</script>

<template>
  <div>
    <div class="page-head">
      <div>
        <h2>设置</h2>
        <div class="muted">按账号保存，存在服务端 SQLite 里，不进版本库。</div>
      </div>
      <div class="spacer"></div>
      <NSpace>
        <NButton secondary @click="load">
          <template #icon>
            <NIcon><RefreshOutline /></NIcon>
          </template>
          还原
        </NButton>
        <NButton type="primary" :loading="busy" @click="save">
          <template #icon>
            <NIcon><SaveOutline /></NIcon>
          </template>
          保存
        </NButton>
      </NSpace>
    </div>

    <NAlert type="info" :show-icon="false" class="note">
      留空的项回退到 rtconfig.h（编译进端侧固件的那份配置），再回退到环境变量。
      这些配置只影响网站侧 agent，端侧设备的 LLM 配置在设备上单独设。
    </NAlert>

    <NCard title="网站侧 agent 配置" size="small">
      <NForm label-placement="top">
        <div v-for="f in entries" :key="f.key" class="entry">
          <div class="entry-head">
            <div class="entry-label">
              {{ f.label }}
              <span class="key mono">{{ f.key }}</span>
            </div>
            <NButton
              v-if="items[f.key]"
              size="tiny"
              quaternary
              type="primary"
              @click="reset(f.key)"
            >
              <template #icon>
                <NIcon :size="13"><ArrowUndoOutline /></NIcon>
              </template>
              恢复默认
            </NButton>
          </div>

          <NSelect
            v-if="f.choice"
            v-model:value="form[f.key]"
            :options="protocols"
            :placeholder="f.hint"
          />
          <NInput
            v-else-if="f.long"
            v-model:value="form[f.key]"
            type="textarea"
            :rows="6"
            :placeholder="f.hint"
          />
          <NInput
            v-else-if="f.secret"
            v-model:value="form[f.key]"
            type="password"
            show-password-on="click"
            autocomplete="off"
            :placeholder="f.hint"
          />
          <NInput v-else v-model:value="form[f.key]" :placeholder="f.hint" />

          <div class="entry-foot">
            <NTag size="small" :bordered="false" type="info">当前：{{ displayValue(f.key) }}</NTag>
            <NTag v-if="f.fallback" size="small" :bordered="false">
              rtconfig.h 默认：{{ f.fallback }}
            </NTag>
          </div>
        </div>
      </NForm>
    </NCard>

    <NCard title="rtconfig.h 与环境变量" size="small" class="src-card">
      <NText depth="3" class="src-note">
        取值优先级：这里填的值 → 环境变量 → 工程 rtconfig.h。rtconfig.h 找不到时，
        服务端会按下面的路径去找；也可以用 AGENT_RTCONFIG_H 直接指定。
      </NText>
      <NTable :bordered="false" size="small">
        <thead>
          <tr>
            <th style="width: 260px">来源</th>
            <th>作用</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="s in sources" :key="s.key">
            <td class="mono">{{ s.key }}</td>
            <td>{{ s.desc }}</td>
          </tr>
        </tbody>
      </NTable>
    </NCard>
  </div>
</template>

<style scoped>
.note {
  margin-bottom: 16px;
}

.entry {
  padding-bottom: 18px;
  margin-bottom: 18px;
  border-bottom: 1px dashed #eef0f4;
}

.entry:last-child {
  border-bottom: none;
  margin-bottom: 0;
  padding-bottom: 0;
}

.entry-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  margin-bottom: 8px;
}

.entry-label {
  display: flex;
  align-items: baseline;
  gap: 8px;
  font-weight: 500;
}

.entry-foot {
  display: flex;
  gap: 8px;
  margin-top: 8px;
  flex-wrap: wrap;
}

.key {
  font-size: 12px;
  color: #8a93a6;
}

.src-card {
  margin-top: 16px;
}

.src-note {
  display: block;
  margin-bottom: 14px;
  font-size: 13px;
  line-height: 1.6;
}
</style>
