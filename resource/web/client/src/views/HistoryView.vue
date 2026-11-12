<script setup>
import { ref, onMounted, computed, nextTick, watch } from 'vue'
import { api } from '../api.js'

const conversations = ref([])
const devices = ref([])
const filterDeviceId = ref(0)
const currentId = ref(0)
const messages = ref([])
const error = ref('')
const logRef = ref(null)

const current = computed(() =>
  conversations.value.find((c) => c.id === currentId.value) || null,
)

onMounted(async () => {
  try {
    devices.value = await api.listDevices()
    await load()
  } catch (e) {
    error.value = e.message
  }
})

watch(filterDeviceId, load)

async function load() {
  error.value = ''
  try {
    conversations.value = await api.listConversations(
      filterDeviceId.value ? filterDeviceId.value : undefined,
    )
    if (currentId.value && !conversations.value.some((c) => c.id === currentId.value)) {
      currentId.value = 0
      messages.value = []
    }
  } catch (e) {
    error.value = e.message
  }
}

async function open(c) {
  error.value = ''
  currentId.value = c.id
  try {
    const detail = await api.getConversation(c.id)
    messages.value = detail.messages
    nextTick(() => {
      if (logRef.value) logRef.value.scrollTop = logRef.value.scrollHeight
    })
  } catch (e) {
    error.value = e.message
  }
}

async function remove(c) {
  if (!confirm(`删除会话 #${c.id}「${c.title}」？消息会一并删除。`)) return
  try {
    await api.deleteConversation(c.id)
    if (currentId.value === c.id) {
      currentId.value = 0
      messages.value = []
    }
    await load()
  } catch (e) {
    error.value = e.message
  }
}

function agentLabel(c) {
  if (c.agentKind !== 'edge') return '网站侧'
  const d = devices.value.find((x) => x.id === c.deviceId)
  return d ? `端侧：${d.name}` : '端侧'
}
</script>

<template>
  <p v-if="error" class="error">{{ error }}</p>

  <div class="wrap">
    <div class="panel list">
      <div class="list-head">
        <h3>会话</h3>
        <select v-model.number="filterDeviceId" class="input small-select">
          <option :value="0">全部</option>
          <option :value="-1">网站侧</option>
          <option v-for="d in devices" :key="d.id" :value="d.id">端侧：{{ d.name }}</option>
        </select>
        <button class="btn ghost small" @click="load">刷新</button>
      </div>

      <p v-if="!conversations.length" class="empty">没有会话。</p>
      <div v-else class="items">
        <div v-for="c in conversations" :key="c.id"
             class="item" :class="{ active: c.id === currentId }"
             @click="open(c)">
          <div class="line1">
            <span class="title">#{{ c.id }} {{ c.title }}</span>
            <span class="badge">{{ agentLabel(c) }}</span>
          </div>
          <div class="line2">
            <span>{{ c.updatedAt }}</span>
            <button class="btn small danger" @click.stop="remove(c)">删除</button>
          </div>
        </div>
      </div>
    </div>

    <div class="panel detail">
      <h3>{{ current ? `#${current.id} ${current.title}` : '详情' }}</h3>
      <div ref="logRef" class="log">
        <p v-if="!messages.length" class="empty">从左边选一个会话查看消息。</p>
        <div v-for="m in messages" :key="m.id" class="msg" :class="m.role">
          <div class="who">
            {{ m.role === 'user' ? '用户' : '助手' }}
            <span class="time">{{ m.createdAt }}</span>
          </div>
          <div class="text">{{ m.content }}</div>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.wrap {
  display: grid;
  grid-template-columns: 340px 1fr;
  gap: 16px;
  align-items: start;
}

.list-head {
  display: flex;
  gap: 8px;
  align-items: center;
  margin-bottom: 10px;
}

.list-head h3 {
  margin: 0;
  flex: 1;
}

.small-select {
  width: auto;
  padding: 5px 8px;
}

.item {
  padding: 10px;
  border: 1px solid var(--line);
  border-radius: 6px;
  margin-bottom: 8px;
  cursor: pointer;
}

.item:hover {
  border-color: var(--primary);
}

.item.active {
  border-color: var(--primary);
  background: #eff6ff;
}

.line1,
.line2 {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 8px;
}

.line1 .title {
  font-weight: 600;
}

.line2 {
  margin-top: 6px;
  color: var(--muted);
  font-size: 12px;
}

.badge {
  font-size: 12px;
  color: var(--muted);
  border: 1px solid var(--line);
  border-radius: 10px;
  padding: 1px 8px;
  white-space: nowrap;
}

.log {
  height: 520px;
  overflow-y: auto;
}

.msg {
  margin-bottom: 14px;
  max-width: 85%;
}

.msg.user {
  margin-left: auto;
}

.who {
  font-size: 12px;
  color: var(--muted);
  margin-bottom: 4px;
}

.time {
  margin-left: 6px;
  opacity: 0.7;
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

@media (max-width: 860px) {
  .wrap {
    grid-template-columns: 1fr;
  }
}
</style>
