<script setup>
import { computed, onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import { NCard, NButton, NTag, NAlert, NEmpty, NSpin } from 'naive-ui'
import { api } from '../api.js'

const router = useRouter()
const devices = ref(null)
const conversations = ref(null)
const status = ref(null)
const jobs = ref(null)
const emqx = ref(null)
const loading = ref(false)
const errors = ref([])
const updated = ref('')
const stats = computed(() => [
  { label: '已接入设备', value: devices.value?.length ?? '—', note: '当前账号的设备', route: 'devices' },
  { label: '最近探活在线', value: devices.value?.filter(d => d.online && d.enabled).length ?? '—', note: '以最近一次探活结果为准', route: 'devices' },
  { label: '历史会话', value: conversations.value?.length ?? '—', note: '网站与端侧会话', route: 'history' },
  { label: '执行中的安装', value: jobs.value?.filter(j => ['pending', 'running'].includes(j.status)).length ?? '—', note: '后台依赖安装任务', route: 'ops' },
])
const services = computed(() => [
  { name: 'EMQX MQTT', ready: emqx.value?.running, address: emqx.value ? 'MQTT :1883 · Dashboard :18083' : undefined },
  { name: '文件服务器', ready: status.value?.fileServer, address: status.value?.fileServerUrl },
  { name: 'MCP 网关', ready: status.value?.mcpGateway, address: status.value?.mcpUrl },
  { name: 'Python 环境', ready: status.value?.venvReady },
  { name: '语音合成', ready: status.value?.ttsReady },
])
const recent = computed(() => [...(conversations.value || [])]
  .sort((a, b) => String(b.updatedAt || b.createdAt || '').localeCompare(String(a.updatedAt || a.createdAt || '')))
  .slice(0, 5))
async function refresh() {
  if (loading.value) return
  loading.value = true
  errors.value = []
  const sources = [
    [devices, api.listDevices, '设备'],
    [conversations, () => api.listConversations(), '会话'],
    [status, api.opsStatus, '服务'],
    [jobs, api.opsJobs, '安装任务'],
    [emqx, api.emqxStatus, 'EMQX'],
  ]
  const results = await Promise.allSettled(sources.map(source => source[1]()))
  results.forEach((result, index) => {
    if (result.status === 'fulfilled') sources[index][0].value = result.value
    else {
      sources[index][0].value = null
      errors.value.push(sources[index][2] + '加载失败：' + result.reason.message)
    }
  })
  updated.value = new Date().toLocaleTimeString('zh-CN')
  loading.value = false
}
onMounted(refresh)
</script>

<template>
  <div>
    <div class="page-head">
      <div><h2>工作台总览</h2><div class="muted">设备、对话与服务运行情况</div></div>
      <div class="spacer"></div>
      <span class="muted" v-if="updated">最近刷新 {{ updated }}</span>
      <NButton secondary :loading="loading" @click="refresh">刷新数据</NButton>
      <NButton type="primary" @click="router.push('/chat')">开始对话</NButton>
    </div>
    <NAlert v-for="error in errors" :key="error" type="warning" class="notice">{{ error }}</NAlert>
    <NSpin :show="loading">
      <div class="metrics">
        <button v-for="(stat, index) in stats" :key="stat.label" class="metric" @click="router.push({ name: stat.route })">
          <span class="metric-label">{{ stat.label }} <span class="metric-arrow">↗</span></span>
          <strong :class="'accent-' + index">{{ stat.value }}</strong>
          <span class="muted">{{ stat.note }}</span>
        </button>
      </div>
      <div class="overview-grid">
        <NCard title="服务与环境">
          <template #header-extra><NButton text type="primary" @click="router.push('/ops')">运维管理 →</NButton></template>
          <div v-for="service in services" :key="service.name" class="service-row">
            <div><strong>{{ service.name }}</strong><div v-if="service.address" class="address">{{ service.address }}</div></div>
            <NTag :bordered="false" :type="service.ready ? 'success' : 'default'">{{ service.ready == null ? '未知' : service.ready ? '就绪' : '未就绪' }}</NTag>
          </div>
        </NCard>
        <NCard title="最近会话">
          <template #header-extra><NButton text type="primary" @click="router.push('/history')">查看记录 →</NButton></template>
          <NEmpty v-if="!recent.length" :description="conversations === null ? '会话数据暂不可用' : '还没有会话，开始你的第一次对话'" />
          <div v-for="conversation in recent" :key="conversation.id" class="service-row">
            <div class="conversation-summary"><strong>{{ conversation.title || '未命名会话' }}</strong><div class="address">{{ conversation.updatedAt || conversation.createdAt }}</div></div>
            <NTag :bordered="false" size="small">{{ conversation.deviceId ? '端侧' : '网站' }}</NTag>
          </div>
        </NCard>
      </div>
      <NCard title="设备概览">
        <template #header-extra><NButton text type="primary" @click="router.push('/devices')">设备管理 →</NButton></template>
        <NEmpty v-if="!devices?.length" :description="devices === null ? '设备数据暂不可用' : '尚未接入设备'" />
        <div v-else class="device-grid">
          <div v-for="device in devices.slice(0, 6)" :key="device.id" class="device-tile">
            <div class="device-title"><strong>{{ device.name }}</strong><NTag size="small" :bordered="false" :type="device.enabled && device.online ? 'success' : 'default'">{{ !device.enabled ? '已停用' : device.online ? '最近在线' : '未在线' }}</NTag></div>
            <div class="address">{{ device.address }}</div>
            <div class="muted">{{ device.groupName || '未分组' }}</div>
          </div>
        </div>
      </NCard>
    </NSpin>
  </div>
</template>

<style scoped>
.notice { margin-bottom: 16px; }
.metrics { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 20px; margin-bottom: 24px; }
.metric { text-align: left; background: white; border: 1px solid #e6e9ef; border-radius: 16px; padding: 24px; cursor: pointer; display: flex; flex-direction: column; gap: 12px; font: inherit; }
.metric:hover { border-color: #a5a6f6; box-shadow: 0 6px 24px #6366f10c; }
.metric-label { display: flex; justify-content: space-between; color: #667085; }
.metric-arrow { color: #a0a6b7; }
.metric strong { font-size: 36px; font-weight: 650; line-height: 1.2; }
.accent-0 { color: #6366f1; } .accent-1 { color: #159b79; } .accent-2 { color: #4285cf; } .accent-3 { color: #bd8542; }
.overview-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 24px; margin-bottom: 24px; }
.service-row { display: flex; justify-content: space-between; align-items: center; gap: 16px; padding: 17px 0; border-bottom: 1px solid #f0f1f6; }
.service-row:last-child { border: 0; }
.service-row strong { font-weight: 500; }
.address { font-size: 12px; color: #8991a2; margin-top: 5px; overflow-wrap: anywhere; }
.conversation-summary { min-width: 0; overflow-wrap: anywhere; }
.device-grid { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 16px; }
.device-tile { padding: 18px; border: 1px solid #eaecf4; border-radius: 12px; }
.device-title { display: flex; justify-content: space-between; gap: 12px; }
.device-tile .muted { margin-top: 12px; }
@media (max-width: 1100px) { .metrics, .device-grid { grid-template-columns: repeat(2, minmax(0, 1fr)); } }
@media (max-width: 760px) { .overview-grid, .device-grid { grid-template-columns: 1fr; } .metrics { gap: 12px; } .metric { padding: 16px; } }
</style>
