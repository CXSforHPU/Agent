<script setup>
import { ref, onMounted, onUnmounted, computed, h } from 'vue'
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
  NAlert,
  useMessage,
} from 'naive-ui'
import {
  AddOutline,
  RefreshOutline,
  SearchOutline,
  CreateOutline,
  TrashOutline,
  PulseOutline,
  LinkOutline,
  KeyOutline,
} from '@vicons/ionicons5'
import { api } from '../api.js'

const message = useMessage()

const devices = ref([])
const cards = ref([])
const loading = ref(false)
const keyword = ref('')
const groupFilter = ref(null)
let refreshTimer = null

const connectionOptions = [{ label: 'WebSocket 主动连接', value: 'ws' }]
const isWs = (device) => device.address?.startsWith('ws://')

/** 四态：未激活 / 激活中 / 在线 / 未在线。后端按实时连接算，刷新即最新。 */
const STATUS_META = {
  unactivated: { label: '未激活', type: 'default' },
  activating: { label: '激活中', type: 'warning' },
  online: { label: '在线', type: 'success' },
  offline: { label: '未在线', type: 'info' },
}
const statusOf = (device) => STATUS_META[device.status] || STATUS_META.unactivated
const isOnline = (device) => device.status === 'online'

const empty = {
  id: 0,
  name: '',
  address: '',
  connection: 'ws',
  wsId: '',
  groupName: '',
  note: '',
  enabled: true,
  username: '',
  password: '',
}
const form = ref({ ...empty })
const editing = ref(false)
const formOpen = ref(false)
const saving = ref(false)

/** 一次性激活配置：创建/设置凭据后只展示这一次，关掉就再也拿不到密码。 */
const activation = ref(null)
const activationCopied = ref(false)

const configForm = ref({ id: 0, name: '', apiKey: '', modelName: '', baseUrl: '' })
const configOpen = ref(false)
const configSaving = ref(false)

const bindForm = ref({ id: 0, deviceName: '', cardId: null })
const bindOpen = ref(false)
const bindSaving = ref(false)

const formRules = {
  name: [{ required: true, message: '请填设备名', trigger: ['blur', 'input'] }],
  wsId: [{ required: true, message: '请填设备 ID', trigger: ['blur', 'input'] }],
}

const groupOptions = computed(() => {
  const set = new Set()
  for (const d of devices.value) {
    if (d.groupName) set.add(d.groupName)
  }
  return [...set].map((g) => ({ label: g, value: g }))
})

const cardOptions = computed(() =>
  cards.value.map((c) => ({
    label: `${c.name}（已绑 ${c.deviceCount || 0} 台）`,
    value: c.id,
  })),
)

const filtered = computed(() => {
  const kw = keyword.value.trim().toLowerCase()
  return devices.value.filter((d) => {
    if (groupFilter.value && d.groupName !== groupFilter.value) return false
    if (!kw) return true
    return [d.name, d.address, d.groupName, d.note, d.cardName]
      .filter(Boolean)
      .some((v) => String(v).toLowerCase().includes(kw))
  })
})

const onlineCount = computed(() => devices.value.filter(isOnline).length)
const activatingCount = computed(
  () => devices.value.filter((d) => d.status === 'activating').length,
)
const unactivatedCount = computed(
  () => devices.value.filter((d) => d.status === 'unactivated').length,
)

async function load() {
  loading.value = true
  try {
    const [deviceList, cardList] = await Promise.all([api.listDevices(), api.listCards()])
    devices.value = deviceList
    cards.value = cardList
  } catch (e) {
    message.error(e.message)
  } finally {
    loading.value = false
  }
}

onMounted(() => {
  load()
  // 每 5 秒静默刷新状态：在线/未在线是实时连接，断开后不用手动点刷新
  refreshTimer = setInterval(() => {
    api.listDevices().then((list) => (devices.value = list)).catch(() => {})
  }, 5000)
})

onUnmounted(() => {
  if (refreshTimer) clearInterval(refreshTimer)
})

function openCreate() {
  form.value = { ...empty, connection: 'ws' }
  editing.value = false
  formOpen.value = true
}

function openEdit(d) {
  form.value = {
    id: d.id,
    name: d.name,
    address: isWs(d) ? '' : d.address,
    connection: 'ws',
    wsId: isWs(d) ? d.address.substring(5) : '',
    groupName: d.groupName,
    note: d.note,
    enabled: d.enabled,
    // 密码永远不回显；编辑时留空表示不改
    username: '',
    password: '',
    // 未激活/激活中的设备还能补凭据，已激活的锁死（禁止换密码换 token）
    canCredential: d.status === 'unactivated' || d.status === 'activating',
  }
  editing.value = true
  formOpen.value = true
}

async function save() {
  if (!form.value.name.trim()) {
    message.warning('请填设备名')
    return
  }
  if (!/^[A-Za-z0-9_-]{1,64}$/.test(form.value.wsId)) {
    message.warning('设备 ID 须为 1–64 位字母、数字、下划线或连字符')
    return
  }
  const username = form.value.username.trim()
  const password = form.value.password
  if ((username ? 1 : 0) !== (password ? 1 : 0)) {
    message.warning('设备账号与密码要一起填，或者都不填')
    return
  }
  if (password && password.length < 6) {
    message.warning('设备密码至少 6 位')
    return
  }
  saving.value = true
  try {
    const body = {
      name: form.value.name.trim(),
      address: `ws://${form.value.wsId}`,
      groupName: form.value.groupName,
      note: form.value.note,
      enabled: form.value.enabled,
    }
    if (editing.value) {
      await api.updateDevice(form.value.id, body)
      if (username) {
        const result = await api.setDeviceCredential(form.value.id, { username, password })
        showActivation(result.activation)
      }
      message.success('已保存')
    } else {
      if (username) {
        body.username = username
        body.password = password
      }
      const result = await api.createDevice(body)
      if (result.activation) showActivation(result.activation)
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

/** 一次性激活配置弹窗：密码只在这里出现一次，关闭后不再保存在任何地方。 */
function showActivation(config) {
  activation.value = config
  activationCopied.value = false
}

async function copyActivation() {
  if (!activation.value) return
  const text = [
    `设备ID: ${activation.value.deviceId}`,
    `账号: ${activation.value.username}`,
    `密码: ${activation.value.password}`,
  ].join('\n')
  try {
    await navigator.clipboard.writeText(text)
    activationCopied.value = true
    message.success('已复制激活配置')
  } catch {
    message.error('复制失败，请手动选择文本')
  }
}

async function remove(d) {
  try {
    await api.deleteDevice(d.id)
    message.success(`已删除「${d.name}」。设备已彻底注销，同 ID 可重新登记并再次激活。`)
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

/** 绑定能力卡：替换原来的「下发能力卡」，真正写库，不做假成功。 */
function openBind(d) {
  bindForm.value = { id: d.id, deviceName: d.name, cardId: d.cardId }
  bindOpen.value = true
}

async function sendBind() {
  bindSaving.value = true
  try {
    await api.bindDeviceCard(bindForm.value.id, bindForm.value.cardId)
    message.success(
      bindForm.value.cardId
        ? `已为 ${bindForm.value.deviceName} 绑定能力卡`
        : `已解绑 ${bindForm.value.deviceName} 的能力卡`,
    )
    bindOpen.value = false
    await load()
  } catch (e) {
    // 换卡冲突（409）等后端原因原样展示
    message.warning(e.message)
  } finally {
    bindSaving.value = false
  }
}

const columns = computed(() => [
  {
    title: '设备',
    key: 'name',
    minWidth: 130,
    render: (row) =>
      h('div', { style: 'display:flex;flex-direction:column;gap:2px' }, [
        h('span', { style: 'font-weight:500' }, row.name),
        row.note
          ? h('span', { style: 'color:#8a93a6;font-size:12px' }, row.note)
          : null,
      ]),
  },
  {
    title: '设备 ID',
    key: 'address',
    minWidth: 130,
    render: (row) =>
      h('span', { style: 'font-family:Consolas,monospace;font-size:13px' }, row.address),
  },
  {
    title: '状态',
    key: 'status',
    width: 150,
    render: (row) => {
      const meta = statusOf(row)
      return h('div', { style: 'display:flex;flex-direction:column;gap:2px' }, [
        h(
          NTag,
          { size: 'small', type: meta.type, bordered: false },
          { default: () => meta.label },
        ),
        h(
          'span',
          { style: 'color:#8a93a6;font-size:12px' },
          row.status === 'online'
            ? '实时 WebSocket 连接'
            : row.status === 'activating'
              ? '等待板端连接激活'
              : row.status === 'unactivated'
                ? '未配置激活凭据'
                : '已激活，当前未连接',
        ),
      ])
    },
  },
  {
    title: '能力卡',
    key: 'cardName',
    width: 130,
    render: (row) =>
      row.cardId
        ? h(
            NTag,
            { size: 'small', type: 'info', bordered: false },
            { default: () => row.cardName || `#${row.cardId}` },
          )
        : h('span', { style: 'color:#a0a7b5' }, '未绑定'),
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
    title: '操作',
    key: 'actions',
    width: 230,
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
            { size: 'small', secondary: true, title: '绑定能力卡', onClick: () => openBind(row) },
            { icon: () => h(NIcon, null, { default: () => h(LinkOutline) }) },
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
              default: () =>
                `删除设备「${row.name}」？设备会彻底注销，同 ID 之后可重新登记并再次激活。`,
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
        <div class="muted">登记设备 ID 与激活凭据，查看实时连接状态并绑定能力卡。</div>
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
        <NStatistic label="激活中 / 未激活">
          <span class="stat-value muted">{{ activatingCount }} / {{ unactivatedCount }}</span>
        </NStatistic>
      </NCard>
    </div>

    <NCard title="设备列表" size="small">
      <template #header-extra>
        <NSpace align="center">
          <NInput
            v-model:value="keyword"
            placeholder="搜名称、ID、备注、能力卡"
            clearable
            style="width: 200px"
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
      style="width: 560px"
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
        <NFormItem label="连接方式">
          <NSelect v-model:value="form.connection" :options="connectionOptions" />
        </NFormItem>
        <NFormItem label="设备 ID" path="wsId">
          <NInput v-model:value="form.wsId" placeholder="例如：qemu-agent（1–64 位字母、数字、_ 或 -）" />
        </NFormItem>
        <NText depth="3" class="tip">
          保存为 ws://设备ID。这是逻辑标识，板端用同一 ID 连接服务器完成激活。
        </NText>

        <div class="grid-2">
          <NFormItem label="激活账号">
            <NInput
              v-model:value="form.username"
              placeholder="板端连接时使用的账号"
              :disabled="editing && !form.canCredential"
            />
          </NFormItem>
          <NFormItem label="激活密码">
            <NInput
              v-model:value="form.password"
              type="password"
              show-password-on="click"
              placeholder="至少 6 位；只存哈希，不回显"
              :disabled="editing && !form.canCredential"
            />
          </NFormItem>
        </div>
        <NAlert v-if="editing && !form.canCredential" type="warning" :bordered="false" style="margin-bottom: 12px">
          设备已激活，激活凭据与设备 ID 已锁定；要换凭据请先删除设备再登记。
        </NAlert>
        <NText v-else depth="3" class="tip">
          创建成功会弹出一次性激活配置（设备 ID、账号、密码），请复制到板端 menuconfig 或
          a2a_activate 命令。密码只显示这一次，服务端只保存哈希。
        </NText>

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
      v-model:show="activation"
      preset="card"
      title="一次性激活配置"
      style="width: 520px"
      :mask-closable="false"
      :close-on-esc="false"
      :closable="false"
    >
      <NAlert type="warning" :bordered="false" style="margin-bottom: 16px">
        请立即复制并配置到板端。关闭本窗口后密码不再显示（服务端只保存哈希，无法找回）。
      </NAlert>
      <div class="activation-box">
        <div class="activation-row">
          <span class="label">设备 ID</span>
          <code>{{ activation?.deviceId }}</code>
        </div>
        <div class="activation-row">
          <span class="label">账号</span>
          <code>{{ activation?.username }}</code>
        </div>
        <div class="activation-row">
          <span class="label">密码</span>
          <code class="secret">{{ activation?.password }}</code>
        </div>
        <div class="activation-row">
          <span class="label">连接地址</span>
          <code>ws://&lt;服务器地址&gt;/api/a2a/ws</code>
        </div>
      </div>
      <NText depth="3" class="tip" style="margin-top: 12px">
        板端配置：menuconfig 里填 PKG_AGENT_A2A_USERNAME / PKG_AGENT_A2A_PASSWORD，
        或在 msh 执行 a2a_activate &lt;地址&gt; &lt;端口&gt; 激活。激活成功后板端会保存设备 token，
        之后只用 token 重连，不再发送密码。
      </NText>

      <template #footer>
        <NSpace justify="end">
          <NButton type="primary" :secondary="activationCopied" @click="copyActivation">
            {{ activationCopied ? '已复制' : '复制激活配置' }}
          </NButton>
          <NButton type="primary" @click="activation = null">我已保存，关闭</NButton>
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
        WebSocket 迁移后配置下发暂未提供，提交会返回明确的不支持提示，不会假装成功。
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

    <NModal
      v-model:show="bindOpen"
      preset="card"
      :title="`绑定能力卡 · ${bindForm.deviceName}`"
      style="width: 520px"
      :mask-closable="!bindSaving"
    >
      <NText depth="3" class="tip">
        一台设备只能绑一张能力卡；换卡要先解绑。一张卡可以绑多台设备。
      </NText>
      <NForm label-placement="top">
        <NFormItem label="能力卡">
          <NSelect
            v-model:value="bindForm.cardId"
            :options="cardOptions"
            clearable
            placeholder="选择能力卡（清除选择即解绑）"
          />
        </NFormItem>
      </NForm>
      <NEmpty v-if="!cards.length" description="还没有能力卡，先去能力卡页面新建">
        <template #extra>
          <NButton size="small" secondary @click="$router.push('/cards')">去新建能力卡</NButton>
        </template>
      </NEmpty>

      <template #footer>
        <NSpace justify="end">
          <NButton :disabled="bindSaving" @click="bindOpen = false">取消</NButton>
          <NButton type="primary" :loading="bindSaving" @click="sendBind">
            {{ bindForm.cardId ? '绑定' : '解绑' }}
          </NButton>
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

.stat-value.ok {
  color: #18a058;
}

.stat-value.muted {
  color: #8a93a6;
}

.tip {
  display: block;
  margin-bottom: 16px;
  font-size: 13px;
  line-height: 1.6;
}

.activation-box {
  border: 1px solid #e5e7eb;
  border-radius: 8px;
  padding: 12px 16px;
  background: #f7f8fa;
}

.activation-row {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 6px 0;
}

.activation-row .label {
  width: 72px;
  color: #8a93a6;
  font-size: 13px;
}

.activation-row code {
  font-family: Consolas, monospace;
  font-size: 14px;
}

.activation-row code.secret {
  color: #d03050;
  font-weight: 600;
  letter-spacing: 1px;
}
</style>
