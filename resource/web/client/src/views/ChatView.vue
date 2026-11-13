<script setup>
import { ref, onMounted, computed, nextTick, watch, onBeforeUnmount } from 'vue'
import {
  NCard,
  NButton,
  NIcon,
  NSelect,
  NInput,
  NTag,
  NEmpty,
  NSwitch,
  NDropdown,
  NModal,
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
const search = ref('')
const enterToSend = ref(true)
const loadingConversation = ref(false)
const menuVisible = ref(false)
const menuX = ref(0)
const menuY = ref(0)
const selectedConversation = ref(null)
const editAction = ref('')
const editTitle = ref('')
const saving = ref(false)
const menuOptions = [
  { label: '修改会话名称', key: 'rename' },
  { label: '删除会话', key: 'delete' },
]
function openConversationMenu(event, conversation) {
  if (locked.value) return
  selectedConversation.value = conversation
  menuX.value = event.clientX
  menuY.value = event.clientY
  menuVisible.value = true
}
function chooseConversationAction(action) {
  menuVisible.value = false
  if (locked.value) return
  editTitle.value = selectedConversation.value.title || ''
  editAction.value = action
}
async function saveConversationAction() {
  if (saving.value || !selectedConversation.value) return
  const selected = selectedConversation.value
  saving.value = true
  try {
    if (editAction.value === 'rename') {
      const title = editTitle.value.trim()
      if (!title) return
      await api.renameConversation(selected.id, title)
      selected.title = title
    } else {
      await api.deleteConversation(selected.id)
      conversations.value = conversations.value.filter(c => c.id !== selected.id)
      if (conversationId.value === selected.id) {
        conversationId.value = 0
        messages.value = []
      }
    }
    message.success(editAction.value === 'rename' ? '会话名称已更新' : '会话已删除')
    editAction.value = ''
  } catch (error) {
    message.error(error.message)
  } finally {
    saving.value = false
  }
}
const visibleConversations = computed(() => conversations.value.filter(c =>
  (c.title || '').toLowerCase().includes(search.value.toLowerCase())))
const locked = computed(() => sending.value || loadingConversation.value || Boolean(editAction.value))
function handleKeydown(event) {
  if (event.isComposing || event.key !== 'Enter' || event.shiftKey) return
  if (enterToSend.value || event.ctrlKey || event.metaKey) {
    event.preventDefault()
    send()
  }
}
async function copyMessage(text) {
  try { await navigator.clipboard.writeText(text); message.success('已复制') }
  catch { message.error('复制失败，请手动选择文本') }
}

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

onMounted(async () => {
  try {
    devices.value = await api.listDevices()
    await loadConversations()
  } catch (e) {
    message.error(e.message)
  }
  startEventStream()
})

onBeforeUnmount(() => {
  stopEventStream()
})

watch(deviceId, async () => {
  conversationId.value = 0
  messages.value = []
  try { await loadConversations() } catch (e) { message.error(e.message) }
  startEventStream()
})

/* 端侧 agent 自发上报的结论（告警、传感器越限）从这条流过来 */
const eventController = ref(null)

function stopEventStream() {
  if (eventController.value) {
    eventController.value.abort()
    eventController.value = null
  }
}

async function startEventStream() {
  stopEventStream()
  if (!isEdge.value) return
  const controller = new AbortController()
  eventController.value = controller
  const id = deviceId.value
  for (;;) {
    try {
      await api.deviceEventStream(id, (event) => {
        if (event.kind === 'error' || !event.text) return
        messages.value.push({
          id: `evt-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
          role: 'assistant',
          content: event.text,
          report: true,
        })
        scrollToBottom()
      }, controller.signal)
    } catch (e) {
      if (controller.signal.aborted) return
      console.warn('设备上报流断开：', e.message)
    }
    if (controller.signal.aborted) return
    await new Promise((resolve) => setTimeout(resolve, 3000))
    if (controller.signal.aborted) return
  }
}

async function loadConversations() {
  const result = await api.listConversations(
    isEdge.value ? deviceId.value : undefined,
  )
  conversations.value = result.filter(c => (c.deviceId || 0) === deviceId.value)
}

async function pickConversation(c) {
  if (locked.value) return
  loadingConversation.value = true
  try {
    const detail = await api.getConversation(c.id)
    conversationId.value = c.id
    messages.value = detail.messages
    scrollToBottom()
  } catch (e) {
    message.error(e.message)
  } finally { loadingConversation.value = false }
}

function newConversation() {
  if (locked.value) return
  conversationId.value = 0
  messages.value = []
}

async function send() {
  const text = input.value.trim()
  if (!text || locked.value) return
  sending.value = true

  // 先把用户这条显示出来，回填失败也能看到自己说了什么
  messages.value.push({ id: `tmp-${Date.now()}`, role: 'user', content: text })
  input.value = ''
  scrollToBottom()

  // 端侧 agent 的中间过程实时灌进这条消息
  const stream = {
    id: `tmp-s-${Date.now()}`,
    role: 'assistant',
    content: '',
    reasoning: '',
    reasoningOpen: false,
    tools: [],
    status: '',
    error: '',
    streaming: true,
  }
  messages.value.push(stream)
  scrollToBottom()

  try {
    const body = { message: text }
    if (conversationId.value) body.conversationId = conversationId.value
    if (isEdge.value) body.deviceId = deviceId.value

    await api.chatStream(body, (event) => {
      if (event.kind === 'status') stream.status = event.text
      else if (event.kind === 'reasoning') stream.reasoning += event.text
      else if (event.kind === 'tool') stream.tools.push(event.text)
      else if (event.kind === 'content') stream.content += event.text
      else if (event.kind === 'final') {
        stream.content = event.text
        if (event.conversationId) conversationId.value = event.conversationId
      } else if (event.kind === 'error') stream.error = event.text
      scrollToBottom()
    })

    stream.streaming = false
    if (stream.error) message.error(stream.error)
    try { await loadConversations() } catch { message.warning("回复已收到，会话列表刷新失败") }
    scrollToBottom()
  } catch (e) {
    input.value = text
    message.error(e.message)
    stream.streaming = false
    stream.error = e.message
    if (!stream.content) stream.content = `出错了：${e.message}`
    scrollToBottom()
  } finally {
    sending.value = false
  }
}

function toggleReasoning(m) {
  m.reasoningOpen = !m.reasoningOpen
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
      <NButton secondary :disabled="locked" @click="newConversation">
        <template #icon>
          <NIcon><AddOutline /></NIcon>
        </template>
        新建会话
      </NButton>
    </div>

    <div class="chat-workspace">
      <aside class="chat-sidebar">
        <label class="sidebar-label">对话对象</label>
        <NSelect v-model:value="deviceId" :options="deviceOptions" :disabled="locked" />
        <div class="sidebar-label history-label">会话记录</div>
        <NInput v-model:value="search" placeholder="搜索会话" clearable />
        <div class="conversation-list">
          <button v-for="c in visibleConversations" :key="c.id" class="conversation-item"
            :class="{ active: conversationId === c.id }" :disabled="locked" @click="pickConversation(c)"
            @contextmenu.prevent="openConversationMenu($event, c)"
            @keydown.shift.f10.prevent="openConversationMenu({ clientX: $event.currentTarget.getBoundingClientRect().right, clientY: $event.currentTarget.getBoundingClientRect().bottom }, c)"
            title="右键打开会话操作">
            {{ c.title || '未命名会话' }}
          </button>
          <div v-if="!visibleConversations.length" class="muted empty">暂无匹配的会话</div>
        </div>
        <div class="send-setting"><span>Enter 发送</span><NSwitch v-model:value="enterToSend" /></div>
      </aside>
    <NCard size="small" class="chat-card">
      <div class="conversation-header">
        <NIcon :size="22"><HardwareChipOutline /></NIcon>
        <strong>{{ targetName }}</strong>
        <NTag :bordered="false" size="small">{{ isEdge ? '设备对话' : '智能助手' }}</NTag>
      </div>
      <div ref="logRef" class="log" role="log" aria-label="对话消息" :aria-busy="sending">
        <NEmpty v-if="!messages.length" description="开始新的对话，输入你想了解的问题" />
        <div v-for="m in messages" :key="m.id" class="msg" :class="m.role">
          <div class="who">{{ m.role === 'user' ? '我' : m.report ? '设备上报' : targetName }}</div>
          <div v-if="m.reasoning" class="reasoning">
            <button class="reasoning-head" type="button" @click="toggleReasoning(m)">
              {{ m.reasoningOpen ? '收起思考过程' : '思考过程' }}
            </button>
            <pre v-show="m.reasoningOpen" class="reasoning-body">{{ m.reasoning }}</pre>
          </div>
          <div v-if="m.tools && m.tools.length" class="tool-list">
            <div v-for="(t, i) in m.tools" :key="i" class="tool-call">{{ t }}</div>
          </div>
          <div class="text">{{ m.content }}<span v-if="m.streaming" class="caret">▍</span></div>
          <NButton text size="tiny" class="copy-action" @click="copyMessage(m.content)">复制</NButton>
        </div>
      </div>

      <div v-if="sending" class="reply-status" role="status">正在等待 {{ targetName }} 回复…</div>
      <div class="composer">
        <NInput
          v-model:value="input"
          type="textarea"
          :autosize="{ minRows: 3, maxRows: 8 }"
          :maxlength="4096"
          show-count
          placeholder="输入消息，Shift + Enter 换行"
          @keydown="handleKeydown"
        />
        <NButton type="primary" :loading="sending" :disabled="locked || !input.trim()" @click="send">
          <template #icon>
            <NIcon><SendOutline /></NIcon>
          </template>
          发送
        </NButton>
      </div>
      <div class="composer-hint">{{ enterToSend ? 'Enter 发送 · Shift + Enter 换行' : 'Ctrl / ⌘ + Enter 发送' }}</div>
    </NCard>
    </div>
    <NDropdown trigger="manual" :show="menuVisible" :x="menuX" :y="menuY"
      :options="menuOptions" placement="bottom-start"
      @select="chooseConversationAction" @clickoutside="menuVisible = false"
      @esc="menuVisible = false" />
    <NModal :show="Boolean(editAction)" preset="card" style="width: 420px"
      :title="editAction === 'rename' ? '修改会话名称' : '删除会话'"
      :mask-closable="!saving" :close-on-esc="!saving" :closable="!saving"
      @update:show="value => { if (!value && !saving) editAction = '' }">
      <NInput v-if="editAction === 'rename'" v-model:value="editTitle" autofocus
        :maxlength="64" show-count placeholder="输入会话名称" :disabled="saving"
        @keydown.enter="event => { if (!event.isComposing) saveConversationAction() }" />
      <p v-else>确定删除“{{ selectedConversation?.title || '未命名会话' }}”？该会话的消息记录也会删除。</p>
      <template #footer>
        <div class="dialog-actions">
          <NButton :disabled="saving" @click="editAction = ''">取消</NButton>
          <NButton :type="editAction === 'delete' ? 'error' : 'primary'"
            :loading="saving" :disabled="saving || (editAction === 'rename' && !editTitle.trim())"
            @click="saveConversationAction">{{ editAction === 'rename' ? '保存' : '删除' }}</NButton>
        </div>
      </template>
    </NModal>
  </div>
</template>

<style scoped>
.dialog-actions { display: flex; justify-content: flex-end; gap: 10px; }
.chat-workspace { display: grid; grid-template-columns: 260px minmax(0, 1fr); gap: 20px; }
.chat-sidebar { background: white; border: 1px solid #e6e9ef; border-radius: 16px; padding: 20px; display: flex; flex-direction: column; gap: 12px; }
.sidebar-label { color: #667085; font-size: 12px; font-weight: 600; }
.history-label { margin-top: 14px; }
.conversation-list { flex: 1; overflow: auto; max-height: 420px; min-height: 120px; }
.conversation-item { display: block; width: 100%; text-align: left; padding: 12px; margin-bottom: 5px; border: 0; border-radius: 9px; background: transparent; color: #596174; cursor: pointer; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.conversation-item:hover, .conversation-item.active { background: #eeefff; color: #5759d6; }
.conversation-item:disabled { cursor: wait; opacity: .6; }
.send-setting { display: flex; justify-content: space-between; padding-top: 18px; border-top: 1px solid #eef0f4; font-size: 13px; }
.conversation-header { display: flex; align-items: center; gap: 12px; padding-bottom: 20px; margin-bottom: 20px; border-bottom: 1px solid #eef0f4; }
.copy-action { margin-top: 6px; color: #8890a2; }
.reply-status { padding: 10px 0; color: #6366f1; font-size: 13px; }
.composer-hint { margin-top: 10px; color: #949bad; font-size: 12px; }
@media (max-width: 1000px) {
  .chat-workspace { grid-template-columns: minmax(0, 1fr); }
  .conversation-list { min-height: 0; max-height: 150px; }
}

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
  height: clamp(300px, 48vh, 640px);
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

.reasoning {
  margin-bottom: 6px;
  border: 1px solid #e6e9ef;
  border-radius: 10px;
  background: #fafbfd;
}

.reasoning-head {
  display: block;
  width: 100%;
  text-align: left;
  border: 0;
  background: transparent;
  padding: 6px 12px;
  color: #8890a2;
  font-size: 12px;
  cursor: pointer;
}

.reasoning-body {
  margin: 0;
  padding: 0 12px 10px;
  color: #667085;
  font-size: 12px;
  line-height: 1.6;
  white-space: pre-wrap;
  word-break: break-word;
  max-height: 260px;
  overflow-y: auto;
}

.tool-list {
  margin-bottom: 6px;
}

.tool-call {
  margin-bottom: 4px;
  padding: 5px 10px;
  border-radius: 8px;
  border: 1px dashed #d5d9e3;
  background: #fbfcfe;
  color: #667085;
  font-size: 12px;
  word-break: break-word;
}

.caret {
  color: #6366f1;
  animation: blink 1s steps(2, start) infinite;
}

@keyframes blink {
  to { visibility: hidden; }
}

.msg.user .text {
  background: #6366f1;
  color: #fff;
  border-color: #6366f1;
}

.msg.report .text {
  background: #fff8e6;
  border-color: #f5d78e;
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
