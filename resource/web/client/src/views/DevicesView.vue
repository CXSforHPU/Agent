<script setup>
import { ref, onMounted, computed, h } from 'vue'
import {
  NCard,
  NButton,
  NIcon,
  NTag,
  NSpace,
  NInput,
  NSelect,
  NModal,
  NForm,
  NFormItem,
  NSwitch,
  NDataTable,
  NPopconfirm,
  NStatistic,
  NEmpty,
  NText,
  useMessage,
} from 'naive-ui'
import {
  AddOutline,
  RefreshOutline,
  SearchOutline,
  CloudUploadOutline,
  CreateOutline,
  TrashOutline,
  PulseOutline,
} from '@vicons/ionicons5'
import { api } from '../api.js'

const message = useMessage()

const devices = ref([])
const loading = ref(false)
const keyword = ref('')
const groupFilter = ref(null)

const empty = { id: 0, name: '', address: '', groupName: '', note: '', enabled: true }
const form = ref({ ...empty })
const editing = ref(false)
const formOpen = ref(false)
const saving = ref(false)

const configForm = ref({ id: 0, name: '', apiKey: '', modelName: '', baseUrl: '' })
const configOpen = ref(false)
const configSaving = ref(false)

const formRules = {
  name: [{ required: true, message: '请填设备名', trigger: ['blur', 'input'] }],
  address: [{ required: true, message: '请填地址', trigger: ['blur', 'input'] }],
}

const groupOptions = computed(() => {
  const set = new Set()
  for (const d of devices.value) {
    if (d.groupName) set.add(d.groupName)
  }
  return [...set].map((g) => ({ label: g, value: g }))
})

const filtered = computed(() => {
  const kw = keyword.value.trim().toLowerCase()
  return devices.value.filter((d) => {
    if (groupFilter.value && d.groupName !== groupFilter.value) return false
    if (!kw) return true
    return [d.name, d.address, d.groupName, d.note]
      .filter(Boolean)
      .some((v) => String(v).toLowerCase().includes(kw))
  })
})

const onlineCount = computed(() => devices.value.filter((d) => d.online).length)

async function load() {
  loading.value = true
  try {
    devices.value = await api.listDevices()
  } catch (e) {
    message.error(e.message)
  } finally {
    loading.value = false
  }
}

onMounted(load)

function openCreate() {
  form.value = { ...empty }
  editing.value = false
  formOpen.value = true
}

function openEdit(d) {
  form.value = {
    id: d.id,
    name: d.name,
    address: d.address,
    groupName: d.groupName,
    note: d.note,
    enabled: d.enabled,
  }
  editing.value = true
  formOpen.value = true
}

async function save() {
  saving.value = true
  try {
    const body = {
      name: form.value.name.trim(),
      address: form.value.address.trim(),
      groupName: form.value.groupName,
      note: form.value.note,
      enabled: form.value.enabled,
    }
    if (editing.value) {
      await api.updateDevice(form.value.id, body)
      message.success('已保存')
    } else {
      await api.createDevice(body)
      message.success('已新增')
    }
    formOpen.value = false
    await load()
  } catch (e) {
    message.error(e.message)
  } finally {
    saving.value = false
  }
}

async function remove(d) {
  try {
    await api.deleteDevice(d.id)
    message.success(`已删除「${d.name}」`)
    await load()
  } catch (e) {
    message.error(e.message)
  }
}

async function probe(d) {
  try {
    const result = await api.probeDevice(d.id)
    if (result.online) {
      message.success(`${d.name} 在线，能力卡：${result.card ? result.card.name : '（空）'}`)
    } else {
      message.warning(`${d.name} 不在线：${result.error || '未知原因'}`)
    }
    await load()
  } catch (e) {
    message.error(e.message)
  }
}

function openConfig(d) {
  configForm.value = { id: d.id, name: d.name, apiKey: '', modelName: '', baseUrl: '' }
  configOpen.value = true
}

async function sendConfig() {
  const body = {}
  if (configForm.value.apiKey) body.apiKey = configForm.value.apiKey
  if (configForm.value.modelName) body.modelName = configForm.value.modelName
  if (configForm.value.baseUrl) body.baseUrl = configForm.value.baseUrl
  if (!Object.keys(body).length) {
    message.warning('至少填一项')
    return
  }
  configSaving.value = true
  try {
    await api.pushDeviceConfig(configForm.value.id, body)
    message.success(`已下发到 ${configForm.value.name}`)
    configOpen.value = false
  } catch (e) {
    message.error(e.message)
  } finally {
    configSaving.value = false
  }
}

const columns = computed(() => [
  {
    title: '设备',
    key: 'name',
    minWidth: 120,
    render: (row) =>
      h('div', { style: 'display:flex;flex-direction:column;gap:2px' }, [
        h('span', { style: 'font-weight:500' }, row.name),
        row.note
          ? h('span', { style: 'color:#8a93a6;font-size:12px' }, row.note)
          : null,
      ]),
  },
  {
    title: '地址',
    key: 'address',
    minWidth: 140,
    render: (row) =>
      h('span', { style: 'font-family:Consolas,monospace;font-size:13px' }, row.address),
  },
  {
    title: '分组',
    key: 'groupName',
    width: 100,
    render: (row) =>
      row.groupName
        ? h(NTag, { size: 'small', type: 'info', bordered: false }, { default: () => row.groupName })
        : h('span', { style: 'color:#a0a7b5' }, '未分组'),
  },
  {
    title: '状态',
    key: 'online',
    width: 120,
    render: (row) =>
      h('div', { style: 'display:flex;flex-direction:column;gap:2px' }, [
        h(
          NTag,
          { size: 'small', type: row.online ? 'success' : 'default', bordered: false },
          { default: () => (row.online ? '在线' : '离线') },
        ),
        h(
          'span',
          { style: 'color:#8a93a6;font-size:12px' },
          row.lastSeenAt || '从未探活',
        ),
      ]),
  },
  {
    title: '操作',
    key: 'actions',
    width: 172,
    render: (row) =>
      h(NSpace, { size: 4, wrap: false }, {
        default: () => [
          h(
            NButton,
            { size: 'small', secondary: true, title: '探活', onClick: () => probe(row) },
            { icon: () => h(NIcon, null, { default: () => h(PulseOutline) }) },
          ),
          h(
            NButton,
            { size: 'small', secondary: true, title: '下发配置', onClick: () => openConfig(row) },
            { icon: () => h(NIcon, null, { default: () => h(CloudUploadOutline) }) },
          ),
          h(
            NButton,
            { size: 'small', secondary: true, title: '编辑', onClick: () => openEdit(row) },
            { icon: () => h(NIcon, null, { default: () => h(CreateOutline) }) },
          ),
          h(
            NPopconfirm,
            {
              positiveText: '删除',
              negativeText: '取消',
              onPositiveClick: () => remove(row),
            },
            {
              trigger: () =>
                h(
                  NButton,
                  { size: 'small', quaternary: true, type: 'error', title: '删除' },
                  { icon: () => h(NIcon, null, { default: () => h(TrashOutline) }) },
                ),
              default: () => `删除设备「${row.name}」？关联的会话不会一并删除。`,
            },
          ),
        ],
      }),
  },
])
</script>

<template>
  <div>
    <div class="page-head">
      <div>
        <h2>设备管理</h2>
        <div class="muted">登记端侧板子，探活在线状态，把 LLM 配置下发到设备。</div>
      </div>
      <div class="spacer"></div>
      <NSpace>
        <NButton secondary :loading="loading" @click="load">
          <template #icon>
            <NIcon><RefreshOutline /></NIcon>
          </template>
          刷新
        </NButton>
        <NButton type="primary" @click="openCreate">
          <template #icon>
            <NIcon><AddOutline /></NIcon>
          </template>
          新建设备
        </NButton>
      </NSpace>
    </div>

    <div class="grid-3" style="margin-bottom: 16px">
      <NCard size="small">
        <NStatistic label="设备总数">
          <span class="stat-value">{{ devices.length }}</span>
        </NStatistic>
      </NCard>
      <NCard size="small">
        <NStatistic label="在线">
          <span class="stat-value ok">{{ onlineCount }}</span>
        </NStatistic>
      </NCard>
      <NCard size="small">
        <NStatistic label="离线">
          <span class="stat-value muted">{{ devices.length - onlineCount }}</span>
        </NStatistic>
      </NCard>
    </div>

    <NCard title="设备列表" size="small">
      <template #header-extra>
        <NSpace align="center">
          <NInput
            v-model:value="keyword"
            placeholder="搜名称、地址、备注"
            clearable
            style="width: 180px"
          >
            <template #prefix>
              <NIcon :size="16"><SearchOutline /></NIcon>
            </template>
          </NInput>
          <NSelect
            v-model:value="groupFilter"
            :options="groupOptions"
            placeholder="全部分组"
            clearable
            style="width: 150px"
          />
        </NSpace>
      </template>

      <NEmpty v-if="!filtered.length" description="没有匹配的设备">
        <template #extra>
          <NButton size="small" secondary @click="openCreate">新建设备</NButton>
        </template>
      </NEmpty>
      <NDataTable
        v-else
        :columns="columns"
        :data="filtered"
        :loading="loading"
        :row-key="(row) => row.id"
        :bordered="false"
        size="small"
      />
    </NCard>

    <NModal
      v-model:show="formOpen"
      preset="card"
      :title="editing ? '编辑设备' : '新建设备'"
      style="width: 520px"
      :mask-closable="!saving"
    >
      <NForm :rules="formRules" label-placement="top">
        <div class="grid-2">
          <NFormItem label="设备名" path="name">
            <NInput v-model:value="form.name" placeholder="例如：实验室 A 板" />
          </NFormItem>
          <NFormItem label="分组" path="groupName">
            <NInput v-model:value="form.groupName" placeholder="可留空" />
          </NFormItem>
        </div>
        <NFormItem label="地址" path="address">
          <NInput
            v-model:value="form.address"
            placeholder="http://192.168.1.10 或 192.168.1.10"
          />
        </NFormItem>
        <NFormItem label="备注" path="note">
          <NInput v-model:value="form.note" type="textarea" placeholder="可留空" :rows="2" />
        </NFormItem>
        <NFormItem label="启用" path="enabled">
          <NSwitch v-model:value="form.enabled" />
        </NFormItem>
      </NForm>

      <template #footer>
        <NSpace justify="end">
          <NButton :disabled="saving" @click="formOpen = false">取消</NButton>
          <NButton type="primary" :loading="saving" @click="save">
            {{ editing ? '保存' : '新增' }}
          </NButton>
        </NSpace>
      </template>
    </NModal>

    <NModal
      v-model:show="configOpen"
      preset="card"
      :title="`下发配置到 ${configForm.name}`"
      style="width: 520px"
      :mask-closable="!configSaving"
    >
      <NText depth="3" class="tip">
        走设备的 WebNet 通道 POST /config。只开了 A2A 通道的设备没有这个端点，会返回 502。留空表示不改。
      </NText>
      <NForm label-placement="top">
        <NFormItem label="API Key">
          <NInput
            v-model:value="configForm.apiKey"
            type="password"
            show-password-on="click"
            placeholder="留空则不改"
          />
        </NFormItem>
        <NFormItem label="模型名">
          <NInput v-model:value="configForm.modelName" placeholder="留空则不改" />
        </NFormItem>
        <NFormItem label="base URL">
          <NInput v-model:value="configForm.baseUrl" placeholder="留空则不改" />
        </NFormItem>
      </NForm>

      <template #footer>
        <NSpace justify="end">
          <NButton :disabled="configSaving" @click="configOpen = false">取消</NButton>
          <NButton type="primary" :loading="configSaving" @click="sendConfig">下发</NButton>
        </NSpace>
      </template>
    </NModal>
  </div>
</template>

<style scoped>
.stat-value {
  font-size: 26px;
  font-weight: 600;
}

.tip {
  display: block;
  margin-bottom: 16px;
  font-size: 13px;
  line-height: 1.6;
}
</style>
