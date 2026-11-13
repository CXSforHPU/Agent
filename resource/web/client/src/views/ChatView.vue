<script setup>
import { ref, reactive, onMounted, computed, nextTick, watch, onBeforeUnmount } from 'vue'
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
import { SendOutline, AddOutline, HardwareChipOutline, CloseOutline } from '@vicons/ionicons5'
import { api } from '../api.js'

const message = useMessage()

const devices = ref([])
const sideDeviceId = ref(0) // 左侧栏选中的对话对象
const conversations = ref([])
const panels = ref([])
const search = ref('')
const enterToSend = ref(true)
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

let panelSeq = 0

function openConversationMenu(event, conversation) {
  if (saving.value) return
  selectedConversation.value = conversation
  menuX.value = event.clientX
  menuY.value = event.clientY
  menuVisible.value = true
}
function chooseConversationAction(action) {
  menuVisible.value = false
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
      for (const panel of panels.value) {
        if (panel.conversationId === selected.id) {
          panel.conversationId = 0
          panel.messages = []
        }
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
function handleKeydown(event, panel) {
  if (event.isComposing || event.key !== 'Enter' || event.shiftKey) return
  if (enterToSend.value || event.ctrlKey || event.metaKey) {
    event.preventDefault()
    sendToPanel(panel)
  }
}
async function copyMessage(text) {
  try { await navigator.clipboard.writeText(text); message.success('已复制') }
  catch { message.error('复制失败，请手动选择文本') }
}

const deviceOptions = computed(() => [
  { label: '网站侧 agent', value: 0 },
  ...devices.value.map((d) => ({ label: `端侧：${d.name}`, value: d.id })),
])

/** 面板的对话对象名称。 */
function targetNameOf(deviceId) {
  if (!deviceId) return '网站侧 agent'
  const d = devices.value.find((x) => x.id === deviceId)
  return d ? `端侧：${d.name}` : '端侧 agent'
}

/* ==========================================================================
 * 设备上报流：同一台设备只开一条流，面板各自订阅
 * ========================================================================== */
const deviceStreams = new Map()

function subscribeDevice(deviceId, handler) {
  if (!deviceId) return () => {}
  let entry = deviceStreams.get(deviceId)
  if (!entry) {
    const controller = new AbortController()
    entry = { controller, handlers: new Set() }
    deviceStreams.set(deviceId, entry)
    pumpDeviceStream(deviceId, entry)
  }
  entry.handlers.add(handler)
  return () => {
    entry.handlers.delete(handler)
    if (!entry.handlers.size) {
      entry.controller.abort()
      deviceStreams.delete(deviceId)
    }
  }
}

async function pumpDeviceStream(deviceId, entry) {
  for (;;) {
    try {
      await api.deviceEventStream(deviceId, (event) => {
        if (event.kind === 'error' || !event.text) return
        for (const handler of entry.handlers) handler(event)
      }, entry.controller.signal)
    } catch (e) {
      if (entry.controller.signal.aborted) return
      console.warn('设备上报流断开：', e.message)
    }
    if (entry.controller.signal.aborted) return
    await new Promise((resolve) => setTimeout(resolve, 3000))
    if (entry.controller.signal.aborted) return
  }
}

/* ==========================================================================
 * 会话面板
 * ========================================================================== */

/** 新开一个面板。传 conversation 时把历史消息装进来。 */
async function addPanel(deviceId, conversation) {
  /* 面板用 reactive：上报流回调里直接改面板字段才能触发重绘 */
  const panel = reactive({
    key: `panel-${++panelSeq}`,
    deviceId: deviceId || 0,
    conversationId: conversation ? conversation.id : 0,
    title: conversation ? (conversation.title || '未命名会话') : '',
    messages: [],
    input: '',
    sending: false,
    loading: false,
    abort: null,
    logRef: null,
    unsubscribe: null,
  })
  panels.value.push(panel)

  if (panel.deviceId) {
    panel.unsubscribe = subscribeDevice(panel.deviceId, (event) => {
      panel.messages.push({
        id: `evt-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
        role: 'assistant',
        content: event.text,
        report: true,
      })
      scrollPanel(panel)
    })
  }

  if (conversation) {
    panel.loading = true
    try {
      const detail = await api.getConversation(conversation.id)
      panel.messages = detail.messages
      scrollPanel(panel)
    } catch (e) {
      message.error(e.message)
    } finally {
      panel.loading = false
    }
  }
  return panel
}

/** 关闭面板：停掉上报订阅与正在进行的请求。 */
function closePanel(panel) {
  if (panel.unsubscribe) panel.unsubscribe()
  if (panel.abort) panel.abort.abort()
  panels.value = panels.value.filter(p => p.key !== panel.key)
}

/** 会话列表里点一条：这个会话落到哪个面板。 */
async function openConversation(c) {
  const opened = panels.value.find(p => p.conversationId === c.id)
  if (opened) {
    opened.loading = true
    try {
      const detail = await api.getConversation(c.id)
      opened.messages = detail.messages
      scrollPanel(opened)
    } catch (e) {
      message.error(e.message)
    } finally {
      opened.loading = false
    }
    return
  }
  await addPanel(c.deviceId || 0, c)
}

/** 面板里开新会话：清掉当前会话的绑定，历史归零。 */
function newConversationInPanel(panel) {
  panel.conversationId = 0
  panel.messages = []
}

/** 这个会话是不是已经在某个面板里打开了。 */
function isOpened(c) {
  return panels.value.some(p => p.conversationId === c.id)
}

async function loadConversations() {
  const result = await api.listConversations(
    sideDeviceId.value ? sideDeviceId.value : undefined,
  )
  conversations.value = result.filter(c => (c.deviceId || 0) === sideDeviceId.value)
}

function setLogRef(panel, el) {
  panel.logRef = el
}

function scrollPanel(panel) {
  nextTick(() => {
    if (panel.logRef) panel.logRef.scrollTop = panel.logRef.scrollHeight
  })
}

async function sendToPanel(panel) {
  const text = panel.input.trim()
  if (!text || panel.sending) return
  panel.sending = true

  // 先把用户这条显示出来，回填失败也能看到自己说了什么
  panel.messages.push({ id: `tmp-${Date.now()}`, role: 'user', content: text })
  panel.input = ''
  scrollPanel(panel)

  // agent 的中间过程实时灌进这条消息
  const stream = reactive({
    id: `tmp-s-${Date.now()}`,
    role: 'assistant',
    content: '',
    reasoning: '',
    reasoningOpen: false,
    tools: [],
    status: '',
    error: '',
    streaming: true,
  })
  panel.messages.push(stream)
  scrollPanel(panel)

  const controller = new AbortController()
  panel.abort = controller

  try {
    const body = { message: text }
    if (panel.conversationId) body.conversationId = panel.conversationId
    if (panel.deviceId) body.deviceId = panel.deviceId

    await api.chatStream(body, (event) => {
      if (event.kind === 'status') stream.status = event.text
      else if (event.kind === 'reasoning') stream.reasoning += event.text
      else if (event.kind === 'tool') stream.tools.push(event.text)
      else if (event.kind === 'content') stream.content += event.text
      else if (event.kind === 'final') {
        stream.content = event.text
        if (event.conversationId) panel.conversationId = event.conversationId
      } else if (event.kind === 'error') stream.error = event.text
      scrollPanel(panel)
    }, controller.signal)

    stream.streaming = false
    if (stream.error) message.error(stream.error)
    try { await loadConversations() } catch { message.warning("回复已收到，会话列表刷新失败") }
    scrollPanel(panel)
  } catch (e) {
    if (e.name === 'AbortError') {
      stream.streaming = false
      stream.error = '已取消'
      return
    }
    panel.input = text
    message.error(e.message)
    stream.streaming = false
    stream.error = e.message
    if (!stream.content) stream.content = `出错了：${e.message}`
    scrollPanel(panel)
  } finally {
    panel.sending = false
    panel.abort = null
  }
}

function toggleReasoning(m) {
  m.reasoningOpen = !m.reasoningOpen
}

onMounted(async () => {
  try {
    devices.value = await api.listDevices()
    await loadConversations()
  } catch (e) {
    message.error(e.message)
  }
  await addPanel(0)
})

onBeforeUnmount(() => {
  for (const panel of panels.value) {
    if (panel.unsubscribe) panel.unsubscribe()
    if (panel.abort) panel.abort.abort()
  }
  deviceStreams.clear()
})

watch(sideDeviceId, async () => {
  try { await loadConversations() } catch (e) { message.error(e.message) }
})
</script>

<template>
  <div>
    <div class="page-head">
      <div>
        <h2>对话</h2>
        <div class="muted">一个面板一个会话，互不干扰：端侧 agent 与网站侧 agent 可以同时聊。</div>
      </div>
      <div class="spacer"></div>
      <NButton secondary @click="addPanel(sideDeviceId)">
        <template #icon>
          <NIcon><AddOutline /></NIcon>
        </template>
        新会话面板
      </NButton>
    </div>

    <div class="chat-workspace">
      <aside class="chat-sidebar">
        <label class="sidebar-label">对话对象</label>
        <NSelect v-model:value="sideDeviceId" :options="deviceOptions" />
        <NButton secondary block class="open-button" @click="addPanel(sideDeviceId)">
          <template #icon>
            <NIcon><AddOutline /></NIcon>
          </template>
          打开「{{ targetNameOf(sideDeviceId) }}」
        </NButton>
        <div class="sidebar-label history-label">会话记录</div>
        <NInput v-model:value="search" placeholder="搜索会话" clearable />
        <div class="conversation-list">
          <button v-for="c in visibleConversations" :key="c.id" class="conversation-item"
            :class="{ active: isOpened(c) }" @click="openConversation(c)"
            @contextmenu.prevent="openConversationMenu($event, c)"
            @keydown.shift.f10.prevent="openConversationMenu({ clientX: $event.currentTarget.getBoundingClientRect().right, clientY: $event.currentTarget.getBoundingClientRect().bottom }, c)"
            title="右键打开会话操作">
            <span class="conversation-title">{{ c.title || '未命名会话' }}</span>
            <span v-if="isOpened(c)" class="conversation-flag">已打开</span>
          </button>
          <div v-if="!visibleConversations.length" class="muted empty">暂无匹配的会话</div>
        </div>
        <div class="send-setting"><span>Enter 发送</span><NSwitch v-model:value="enterToSend" /></div>
      </aside>

      <div class="panel-area">
        <NEmpty v-if="!panels.length" class="panel-empty"
          description="还没有会话面板，点左边的「新会话面板」开一个" />
        <div class="panel-grid">
          <NCard v-for="p in panels" :key="p.key" size="small" class="chat-card">
            <div class="conversation-header">
              <NIcon :size="22"><HardwareChipOutline /></NIcon>
              <strong>{{ targetNameOf(p.deviceId) }}</strong>
              <NTag :bordered="false" size="small">{{ p.deviceId ? '设备对话' : '智能助手' }}</NTag>
              <div class="spacer"></div>
              <NButton text size="tiny" @click="newConversationInPanel(p)">新会话</NButton>
              <NButton text size="tiny" class="close-action" @click="closePanel(p)">
                <template #icon>
                  <NIcon :size="16"><CloseOutline /></NIcon>
                </template>
              </NButton>
            </div>
            <div :ref="el => setLogRef(p, el)" class="log" role="log"
              :aria-label="targetNameOf(p.deviceId) + ' 的对话消息'">
              <NEmpty v-if="!p.messages.length" description="开始新的对话，输入你想了解的问题" />
              <div v-for="m in p.messages" :key="m.id" class="msg" :class="[m.role, m.report ? 'report' : '']">
                <div class="who">{{ m.role === 'user' ? '我' : m.report ? '设备上报' : targetNameOf(p.deviceId) }}</div>
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

            <div v-if="p.sending" class="reply-status" role="status">正在等待 {{ targetNameOf(p.deviceId) }} 回复…</div>
            <div class="composer">
              <NInput
                v-model:value="p.input"
                type="textarea"
                :autosize="{ minRows: 2, maxRows: 6 }"
                :maxlength="4096"
                placeholder="输入消息，Shift + Enter 换行"
                @keydown="handleKeydown($event, p)"
              />
              <NButton type="primary" :loading="p.sending" :disabled="p.sending || !p.input.trim()" @click="sendToPanel(p)">
                <template #icon>
                  <NIcon><SendOutline /></NIcon>
                </template>
                发送
              </NButton>
            </div>
          </NCard>
        </div>
      </div>
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
.open-button { margin-top: 2px; }
.conversation-list { flex: 1; overflow: auto; max-height: 420px; min-height: 120px; }
.conversation-item { display: flex; align-items: center; justify-content: space-between; gap: 8px; width: 100%; text-align: left; padding: 12px; margin-bottom: 5px; border: 0; border-radius: 9px; background: transparent; color: #596174; cursor: pointer; }
.conversation-title { overflow: hidden; white-space: nowrap; text-overflow: ellipsis; }
.conversation-item:hover, .conversation-item.active { background: #eeefff; color: #5759d6; }
.conversation-flag { flex: none; font-size: 11px; color: #5759d6; }
.send-setting { display: flex; justify-content: space-between; padding-top: 18px; border-top: 1px solid #eef0f4; font-size: 13px; }
.panel-area { min-width: 0; }
.panel-empty { padding: 60px 0; }
.panel-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(340px, 1fr)); gap: 16px; align-items: start; }
.conversation-header { display: flex; align-items: center; gap: 12px; padding-bottom: 14px; margin-bottom: 14px; border-bottom: 1px solid #eef0f4; }
.close-action { color: #8890a2; }
.copy-action { margin-top: 6px; color: #8890a2; }
.reply-status { padding: 10px 0; color: #6366f1; font-size: 13px; }
@media (max-width: 1000px) {
  .chat-workspace { grid-template-columns: minmax(0, 1fr); }
  .conversation-list { min-height: 0; max-height: 150px; }
}

.chat-card {
  display: flex;
  flex-direction: column;
  height: clamp(420px, 72vh, 780px);
}

.log {
  flex: 1;
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
