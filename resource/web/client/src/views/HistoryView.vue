<script setup>
import { ref, onMounted, computed, nextTick, watch } from 'vue'
import {
  NCard,
  NButton,
  NIcon,
  NSelect,
  NTag,
  NEmpty,
  NPopconfirm,
  useMessage,
} from 'naive-ui'
import { RefreshOutline, TrashBinOutline } from '@vicons/ionicons5'
import { api } from '../api.js'

const message = useMessage()

const conversations = ref([])
const devices = ref([])
const filterDeviceId = ref(0)
const currentId = ref(0)
const messages = ref([])
const loading = ref(false)
const logRef = ref(null)

const current = computed(() =>
  conversations.value.find((c) => c.id === currentId.value) || null,
)

const filterOptions = computed(() => [
  { label: '全部', value: 0 },
  { label: '网站侧', value: -1 },
  ...devices.value.map((d) => ({ label: `端侧：${d.name}`, value: d.id })),
])

// 后端只按 deviceId 过滤，网站侧会话的 deviceId 为空，这里在前端一并筛
const visible = computed(() => {
  if (filterDeviceId.value === 0) return conversations.value
  if (filterDeviceId.value === -1) {
    return conversations.value.filter((c) => c.agentKind !== 'edge')
  }
  return conversations.value.filter((c) => c.deviceId === filterDeviceId.value)
})

onMounted(async () => {
  try {
    devices.value = await api.listDevices()
    await load()
  } catch (e) {
    message.error(e.message)
  }
})

watch(filterDeviceId, () => {
  if (currentId.value && !visible.value.some((c) => c.id === currentId.value)) {
    currentId.value = 0
    messages.value = []
  }
})

async function load() {
  loading.value = true
  try {
    conversations.value = await api.listConversations()
  } catch (e) {
    message.error(e.message)
  } finally {
    loading.value = false
  }
}

async function open(c) {
  try {
    const detail = await api.getConversation(c.id)
    currentId.value = c.id
    messages.value = detail.messages
    nextTick(() => {
      if (logRef.value) logRef.value.scrollTop = logRef.value.scrollHeight
    })
  } catch (e) {
    message.error(e.message)
  }
}

async function remove(c) {
  try {
    await api.deleteConversation(c.id)
    message.success(`已删除会话 #${c.id}`)
    if (currentId.value === c.id) {
      currentId.value = 0
      messages.value = []
    }
    await load()
  } catch (e) {
    message.error(e.message)
  }
}

function agentLabel(c) {
  if (c.agentKind !== 'edge') return '网站侧'
  const d = devices.value.find((x) => x.id === c.deviceId)
  return d ? `端侧：${d.name}` : '端侧'
}
</script>

<template>
  <div>
    <div class="page-head">
      <div>
        <h2>会话历史</h2>
        <div class="muted">网站侧 agent 与端侧设备的全部对话记录，按对象筛选。</div>
      </div>
      <div class="spacer"></div>
      <NSelect
        v-model:value="filterDeviceId"
        :options="filterOptions"
        style="width: 200px"
      />
      <NButton secondary :loading="loading" @click="load">
        <template #icon>
          <NIcon><RefreshOutline /></NIcon>
        </template>
        刷新
      </NButton>
    </div>

    <div class="wrap">
      <NCard size="small" title="会话列表">
        <NEmpty v-if="!visible.length" description="没有会话" />
        <div v-else class="items">
          <div
            v-for="c in visible"
            :key="c.id"
            class="item"
            :class="{ active: c.id === currentId }"
            @click="open(c)"
          >
            <div class="line1">
              <span class="title">#{{ c.id }} {{ c.title }}</span>
              <NTag size="small" :bordered="false" type="info">
                {{ agentLabel(c) }}
              </NTag>
            </div>
            <div class="line2">
              <span>{{ c.updatedAt }}</span>
              <NPopconfirm
                positive-text="删除"
                negative-text="取消"
                @positive-click="remove(c)"
              >
                <template #trigger>
                  <NButton
                    quaternary
                    size="tiny"
                    type="error"
                    @click.stop
                  >
                    <template #icon>
                      <NIcon :size="14"><TrashBinOutline /></NIcon>
                    </template>
                  </NButton>
                </template>
                删除会话 #{{ c.id }}？消息会一并删除。
              </NPopconfirm>
            </div>
          </div>
        </div>
      </NCard>

      <NCard size="small" :title="current ? `#${current.id} ${current.title}` : '详情'">
        <div ref="logRef" class="log">
          <NEmpty v-if="!messages.length" description="从左边选一个会话查看消息" />
          <div v-for="m in messages" :key="m.id" class="msg" :class="m.role">
            <div class="who">
              {{ m.role === 'user' ? '用户' : '助手' }}
              <span class="time">{{ m.createdAt }}</span>
            </div>
            <div class="text">{{ m.content }}</div>
          </div>
        </div>
      </NCard>
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

.items {
  max-height: 560px;
  overflow-y: auto;
}

.item {
  padding: 10px 12px;
  border: 1px solid #eef0f4;
  border-radius: 10px;
  margin-bottom: 8px;
  cursor: pointer;
  transition: border-color 0.15s ease, background-color 0.15s ease;
}

.item:hover {
  border-color: #2f6bff;
}

.item.active {
  border-color: #2f6bff;
  background: #eef3ff;
}

.line1,
.line2 {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 8px;
}

.line1 .title {
  font-weight: 500;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.line2 {
  margin-top: 4px;
  color: #8a93a6;
  font-size: 12px;
}

.log {
  height: 560px;
  overflow-y: auto;
}

.msg {
  margin-bottom: 16px;
  max-width: 85%;
}

.msg.user {
  margin-left: auto;
}

.who {
  font-size: 12px;
  color: #6b7280;
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

@media (max-width: 900px) {
  .wrap {
    grid-template-columns: 1fr;
  }
}
</style>
