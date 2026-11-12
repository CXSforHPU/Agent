<script setup>
import { ref, onMounted, computed, nextTick, watch } from 'vue'
import { api } from '../api.js'

const devices = ref([])
const deviceId = ref(0)          // 0 表示网站侧 agent
const conversations = ref([])
const conversationId = ref(0)
const messages = ref([])
const input = ref('')
const error = ref('')
const sending = ref(false)
const logRef = ref(null)

const isEdge = computed(() => deviceId.value > 0)
const targetName = computed(() => {
  if (!isEdge.value) return '网站侧 agent'
  const d = devices.value.find((x) => x.id === deviceId.value)
  return d ? d.name : '端侧 agent'
})

onMounted(async () => {
  try {
    devices.value = await api.listDevices()
    await loadConversations()
  } catch (e) {
    error.value = e.message
  }
})

watch(deviceId, async () => {
  conversationId.value = 0
  messages.value = []
  error.value = ''
  await loadConversations()
})

async function loadConversations() {
  conversations.value = await api.listConversations(
    isEdge.value ? deviceId.value : undefined,
  )
}

async function pickConversation(c) {
  error.value = ''
  try {
    const detail = await api.getConversation(c.id)
    conversationId.value = c.id
    deviceId.value = c.deviceId || 0
    messages.value = detail.messages
    scrollToBottom()
  } catch (e) {
    error.value = e.message
  }
}

async function pickConversationById() {
  // select 改成 0 就是新建，否则加载选中会话
  if (!conversationId.value) {
    messages.value = []
    error.value = ''
    return
  }
  const c = conversations.value.find((x) => x.id === conversationId.value)
  if (c) await pickConversation(c)
}

function newConversation() {
  conversationId.value = 0
  messages.value = []
  error.value = ''
}

async function send() {
  const text = input.value.trim()
  if (!text || sending.value) return
  error.value = ''
  sending.value = true

  // 先把用户这条显示出来，回填失败也能看到自己说了什么
  messages.value.push({ id: `tmp-${Date.now()}`, role: 'user', content: text })
  input.value = ''
  scrollToBottom()

  try {
    const body = { message: text }
    if (conversationId.value) body.conversationId = conversationId.value
    if (isEdge.value) body.deviceId = deviceId.value

    const result = await api.chat(body)
    conversationId.value = result.conversationId
    messages.value.push({ id: `tmp-a-${Date.now()}`, role: 'assistant', content: result.reply })
    await loadConversations()
    scrollToBottom()
  } catch (e) {
    error.value = e.message
    // 失败时补一条可见的错误，避免只有一条孤立的用户消息
    messages.value.push({
      id: `tmp-e-${Date.now()}`,
      role: 'assistant',
      content: `出错了：${e.message}`,
    })
    scrollToBottom()
  } finally {
    sending.value = false
  }
}

function scrollToBottom() {
  nextTick(() => {
    if (logRef.value) logRef.value.scrollTop = logRef.value.scrollHeight
  })
}
</script>

<template>
  <div class="panel">
    <div class="head">
      <div class="field inline">
        <label>对话对象</label>
        <select v-model.number="deviceId" class="input">
          <option :value="0">网站侧 agent</option>
          <option v-for="d in devices" :key="d.id" :value="d.id">
            端侧：{{ d.name }}
          </option>
        </select>
      </div>

      <div class="field inline">
        <label>会话</label>
        <select v-model.number="conversationId" class="input" @change="pickConversationById">
          <option :value="0">新建会话</option>
          <option v-for="c in conversations" :key="c.id" :value="c.id">
            #{{ c.id }} {{ c.title }}
          </option>
        </select>
      </div>

      <button class="btn ghost" @click="newConversation">清空</button>
    </div>

    <p class="muted">当前对象：{{ targetName }}</p>
  </div>

  <div class="panel chat">
    <div ref="logRef" class="log">
      <p v-if="!messages.length" class="empty">还没有消息，输入后回车发送。</p>
      <div v-for="m in messages" :key="m.id" class="msg" :class="m.role">
        <div class="who">{{ m.role === 'user' ? '我' : targetName }}</div>
        <div class="text">{{ m.content }}</div>
      </div>
    </div>

    <p v-if="error" class="error">{{ error }}</p>

    <div class="composer">
      <textarea v-model="input" class="input" rows="2"
                placeholder="回车发送，Shift+回车换行"
                @keydown.enter.exact.prevent="send"></textarea>
      <button class="btn" :disabled="sending || !input.trim()" @click="send">
        {{ sending ? '发送中……' : '发送' }}
      </button>
    </div>
  </div>
</template>

<style scoped>
.head {
  display: flex;
  gap: 16px;
  align-items: flex-end;
  flex-wrap: wrap;
}

.field.inline {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 0;
}

.field.inline label {
  margin-bottom: 0;
  white-space: nowrap;
}

.field.inline .input {
  width: 220px;
}

.chat {
  display: flex;
  flex-direction: column;
}

.log {
  height: 460px;
  overflow-y: auto;
  padding: 4px;
}

.msg {
  margin-bottom: 14px;
  max-width: 78%;
}

.msg.user {
  margin-left: auto;
}

.who {
  font-size: 12px;
  color: var(--muted);
  margin-bottom: 4px;
}

.msg.user .who {
  text-align: right;
}

.text {
  padding: 10px 12px;
  border-radius: 8px;
  background: var(--bg);
  border: 1px solid var(--line);
  white-space: pre-wrap;
  word-break: break-word;
}

.msg.user .text {
  background: var(--primary);
  color: #fff;
  border-color: var(--primary);
}

.composer {
  display: flex;
  gap: 10px;
  align-items: flex-end;
  margin-top: 12px;
}

.composer textarea {
  resize: vertical;
  min-height: 56px;
}
</style>
