<script setup>
import { ref, onMounted, computed, nextTick, watch } from 'vue'
import {
  NCard,
  NButton,
  NIcon,
  NSelect,
  NInput,
  NTag,
  NEmpty,
  useMessage,
} from 'naive-ui'
import { SendOutline, AddOutline, HardwareChipOutline } from '@vicons/ionicons5'
import { api } from '../api.js'

const message = useMessage()

const devices = ref([])
const deviceId = ref(0) // 0 表示网站侧 agent
const conversations = ref([])
const conversationId = ref(0)
const messages = ref([])
const input = ref('')
const sending = ref(false)
const logRef = ref(null)

const isEdge = computed(() => deviceId.value > 0)
const targetName = computed(() => {
  if (!isEdge.value) return '网站侧 agent'
  const d = devices.value.find((x) => x.id === deviceId.value)
  return d ? d.name : '端侧 agent'
})

const deviceOptions = computed(() => [
  { label: '网站侧 agent', value: 0 },
  ...devices.value.map((d) => ({ label: `端侧：${d.name}`, value: d.id })),
])

const conversationOptions = computed(() => [
  { label: '新建会话', value: 0 },
  ...conversations.value.map((c) => ({ label: `#${c.id} ${c.title}`, value: c.id })),
])

onMounted(async () => {
  try {
    devices.value = await api.listDevices()
    await loadConversations()
  } catch (e) {
    message.error(e.message)
  }
})

watch(deviceId, async () => {
  conversationId.value = 0
  messages.value = []
  await loadConversations()
})

async function loadConversations() {
  conversations.value = await api.listConversations(
    isEdge.value ? deviceId.value : undefined,
  )
}

async function pickConversation(c) {
  try {
    const detail = await api.getConversation(c.id)
    conversationId.value = c.id
    deviceId.value = c.deviceId || 0
    messages.value = detail.messages
    scrollToBottom()
  } catch (e) {
    message.error(e.message)
  }
}

async function pickConversationById() {
  // 选 0 是新建，否则加载选中的会话
  if (!conversationId.value) {
    messages.value = []
    return
  }
  const c = conversations.value.find((x) => x.id === conversationId.value)
  if (c) await pickConversation(c)
}

function newConversation() {
  conversationId.value = 0
  messages.value = []
}

async function send() {
  const text = input.value.trim()
  if (!text || sending.value) return
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
    message.error(e.message)
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
  <div>
    <div class="page-head">
      <div>
        <h2>对话</h2>
        <div class="muted">跟网站侧 agent 或端侧设备直接对话，历史会落到会话列表。</div>
      </div>
      <div class="spacer"></div>
      <NButton secondary @click="newConversation">
        <template #icon>
          <NIcon><AddOutline /></NIcon>
        </template>
        新建会话
      </NButton>
    </div>

    <NCard size="small" class="toolbar-card">
      <div class="toolbar">
        <div class="field">
          <label>对话对象</label>
          <NSelect v-model:value="deviceId" :options="deviceOptions" style="width: 240px" />
        </div>
        <div class="field">
          <label>会话</label>
          <NSelect
            v-model:value="conversationId"
            :options="conversationOptions"
            style="width: 300px"
            @update:value="pickConversationById"
          />
        </div>
        <NTag :bordered="false" type="info" size="small" class="target-tag">
          <template #icon>
            <NIcon :size="13"><HardwareChipOutline /></NIcon>
          </template>
          当前对象：{{ targetName }}
        </NTag>
      </div>
    </NCard>

    <NCard size="small" class="chat-card">
      <div ref="logRef" class="log">
        <NEmpty v-if="!messages.length" description="还没有消息，输入后回车发送" />
        <div v-for="m in messages" :key="m.id" class="msg" :class="m.role">
          <div class="who">{{ m.role === 'user' ? '我' : targetName }}</div>
          <div class="text">{{ m.content }}</div>
        </div>
      </div>

      <div class="composer">
        <NInput
          v-model:value="input"
          type="textarea"
          :rows="2"
          placeholder="回车发送，Shift+回车换行"
          @keydown.enter.exact.prevent="send"
        />
        <NButton type="primary" :loading="sending" :disabled="!input.trim()" @click="send">
          <template #icon>
            <NIcon><SendOutline /></NIcon>
          </template>
          发送
        </NButton>
      </div>
    </NCard>
  </div>
</template>

<style scoped>
.toolbar-card {
  margin-bottom: 16px;
}

.toolbar {
  display: flex;
  gap: 16px;
  align-items: flex-end;
  flex-wrap: wrap;
}

.field {
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.field label {
  font-size: 13px;
  color: #6b7280;
}

.target-tag {
  margin-bottom: 4px;
}

.chat-card {
  display: flex;
  flex-direction: column;
}

.log {
  height: 480px;
  overflow-y: auto;
  padding: 4px 4px 4px 0;
}

.msg {
  margin-bottom: 16px;
  max-width: 78%;
}

.msg.user {
  margin-left: auto;
}

.who {
  font-size: 12px;
  color: #6b7280;
  margin-bottom: 4px;
}

.msg.user .who {
  text-align: right;
}

.text {
  padding: 10px 14px;
  border-radius: 12px;
  background: #f2f4f8;
  border: 1px solid #e6e9ef;
  white-space: pre-wrap;
  word-break: break-word;
  line-height: 1.6;
}

.msg.user .text {
  background: #2f6bff;
  color: #fff;
  border-color: #2f6bff;
}

.composer {
  display: flex;
  gap: 10px;
  align-items: flex-end;
  margin-top: 14px;
  padding-top: 14px;
  border-top: 1px solid #eef0f4;
}
</style>
