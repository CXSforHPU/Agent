<script setup>
import { ref, onMounted, computed } from 'vue'
import { api } from '../api.js'

const devices = ref([])
const error = ref('')
const notice = ref('')
const busy = ref(false)

const empty = { id: 0, name: '', address: '', groupName: '', note: '', enabled: true }
const form = ref({ ...empty })
const editing = ref(false)

const configForm = ref({ id: 0, name: '', apiKey: '', modelName: '', baseUrl: '' })
const configOpen = ref(false)

const groups = computed(() => {
  const map = new Map()
  for (const d of devices.value) {
    const key = d.groupName || '未分组'
    if (!map.has(key)) map.set(key, [])
    map.get(key).push(d)
  }
  return [...map.entries()]
})

async function load() {
  error.value = ''
  try {
    devices.value = await api.listDevices()
  } catch (e) {
    error.value = e.message
  }
}

onMounted(load)

function resetForm() {
  form.value = { ...empty }
  editing.value = false
}

function edit(d) {
  form.value = { id: d.id, name: d.name, address: d.address, groupName: d.groupName,
                 note: d.note, enabled: d.enabled }
  editing.value = true
}

async function save() {
  error.value = ''
  notice.value = ''
  if (!form.value.name.trim() || !form.value.address.trim()) {
    error.value = '设备名和地址都要填'
    return
  }
  busy.value = true
  try {
    const body = { name: form.value.name, address: form.value.address,
                   groupName: form.value.groupName, note: form.value.note,
                   enabled: form.value.enabled }
    if (editing.value) {
      await api.updateDevice(form.value.id, body)
      notice.value = '已保存'
    } else {
      await api.createDevice(body)
      notice.value = '已新增'
    }
    resetForm()
    await load()
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}

async function remove(d) {
  if (!confirm(`删除设备「${d.name}」？关联的会话不会一并删除。`)) return
  error.value = ''
  try {
    await api.deleteDevice(d.id)
    if (form.value.id === d.id) resetForm()
    await load()
  } catch (e) {
    error.value = e.message
  }
}

async function probe(d) {
  error.value = ''
  notice.value = `正在探活 ${d.name}……`
  try {
    const result = await api.probeDevice(d.id)
    notice.value = result.online
      ? `${d.name} 在线，能力卡：${result.card ? result.card.name : '（空）'}`
      : `${d.name} 不在线：${result.error || '未知原因'}`
    await load()
  } catch (e) {
    error.value = e.message
    notice.value = ''
  }
}

function openConfig(d) {
  configForm.value = { id: d.id, name: d.name, apiKey: '', modelName: '', baseUrl: '' }
  configOpen.value = true
  error.value = ''
  notice.value = ''
}

async function sendConfig() {
  error.value = ''
  try {
    const body = {}
    if (configForm.value.apiKey) body.apiKey = configForm.value.apiKey
    if (configForm.value.modelName) body.modelName = configForm.value.modelName
    if (configForm.value.baseUrl) body.baseUrl = configForm.value.baseUrl
    if (!Object.keys(body).length) {
      error.value = '至少填一项'
      return
    }
    await api.pushDeviceConfig(configForm.value.id, body)
    notice.value = `已下发到 ${configForm.value.name}`
    configOpen.value = false
  } catch (e) {
    error.value = e.message
  }
}
</script>

<template>
  <div class="panel">
    <h3>{{ editing ? '编辑设备' : '新建设备' }}</h3>
    <div class="grid">
      <div class="field">
        <label>设备名</label>
        <input v-model="form.name" class="input" placeholder="例如：实验室 A 板" />
      </div>
      <div class="field">
        <label>地址</label>
        <input v-model="form.address" class="input" placeholder="http://192.168.1.10 或 192.168.1.10" />
      </div>
      <div class="field">
        <label>分组</label>
        <input v-model="form.groupName" class="input" placeholder="可留空" />
      </div>
      <div class="field">
        <label>备注</label>
        <input v-model="form.note" class="input" placeholder="可留空" />
      </div>
    </div>
    <p v-if="error" class="error">{{ error }}</p>
    <p v-if="notice" class="ok">{{ notice }}</p>
    <div class="row-actions">
      <button class="btn" :disabled="busy" @click="save">
        {{ editing ? '保存' : '新增' }}
      </button>
      <button v-if="editing" class="btn ghost" @click="resetForm">取消</button>
      <button v-if="!editing" class="btn ghost" @click="load">刷新</button>
    </div>
  </div>

  <div class="panel">
    <h3>设备列表</h3>
    <p v-if="!devices.length" class="empty">还没有设备，先在上面新增一个。</p>
    <template v-else>
      <div v-for="[group, list] in groups" :key="group" class="group">
        <div class="group-name">{{ group }}</div>
        <table>
          <thead>
            <tr>
              <th style="width: 22%">名称</th>
              <th style="width: 30%">地址</th>
              <th style="width: 12%">状态</th>
              <th>操作</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="d in list" :key="d.id">
              <td>
                {{ d.name }}
                <div class="muted">{{ d.note }}</div>
              </td>
              <td class="mono">{{ d.address }}</td>
              <td>
                <span class="dot" :class="{ on: d.online }"></span>{{ d.online ? '在线' : '离线' }}
                <div class="muted">{{ d.lastSeenAt || '从未探活' }}</div>
              </td>
              <td>
                <div class="row-actions">
                  <button class="btn small" @click="probe(d)">探活</button>
                  <button class="btn small ghost" @click="openConfig(d)">下发配置</button>
                  <button class="btn small ghost" @click="edit(d)">编辑</button>
                  <button class="btn small danger" @click="remove(d)">删除</button>
                </div>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </template>
  </div>

  <div v-if="configOpen" class="panel">
    <h3>下发配置到 {{ configForm.name }}</h3>
    <p class="muted">走设备的 WebNet 通道 <code>POST /config</code>。只开了 A2A 通道的设备没有这个端点，会返回 502。</p>
    <div class="field">
      <label>API Key</label>
      <input v-model="configForm.apiKey" class="input" placeholder="留空则不改" />
    </div>
    <div class="field">
      <label>模型名</label>
      <input v-model="configForm.modelName" class="input" placeholder="留空则不改" />
    </div>
    <div class="field">
      <label>base URL</label>
      <input v-model="configForm.baseUrl" class="input" placeholder="留空则不改" />
    </div>
    <p v-if="error" class="error">{{ error }}</p>
    <div class="row-actions">
      <button class="btn" @click="sendConfig">下发</button>
      <button class="btn ghost" @click="configOpen = false">取消</button>
    </div>
  </div>
</template>

<style scoped>
.grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
  gap: 0 16px;
}

.group {
  margin-bottom: 18px;
}

.group-name {
  font-weight: 600;
  margin-bottom: 6px;
}

.muted {
  color: var(--muted);
  font-size: 12px;
}

.mono {
  font-family: Consolas, monospace;
  font-size: 13px;
}
</style>
