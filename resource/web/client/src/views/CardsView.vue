<script setup>
import { ref, onMounted, computed, h } from 'vue'
import {
  NCard,
  NButton,
  NIcon,
  NTag,
  NSpace,
  NInput,
  NModal,
  NForm,
  NFormItem,
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
  CreateOutline,
  TrashOutline,
  LinkOutline,
  UnlinkOutline,
} from '@vicons/ionicons5'
import { api } from '../api.js'

const message = useMessage()

const cards = ref([])
const devices = ref([])
const loading = ref(false)
const keyword = ref('')

const form = ref({ id: 0, name: '', description: '', version: '1.0.0', skills: '' })
const editing = ref(false)
const formOpen = ref(false)
const saving = ref(false)

const bindOpen = ref(false)
const bindCard = ref(null)
const binding = ref(false)

const filtered = computed(() => {
  const kw = keyword.value.trim().toLowerCase()
  if (!kw) return cards.value
  return cards.value.filter((c) =>
    [c.name, c.description, c.version].some((v) => String(v || '').toLowerCase().includes(kw)),
  )
})

const totalBindings = computed(() => cards.value.reduce((sum, c) => sum + (c.deviceCount || 0), 0))

async function load() {
  loading.value = true
  try {
    const [cardList, deviceList] = await Promise.all([api.listCards(), api.listDevices()])
    cards.value = cardList
    devices.value = deviceList
  } catch (e) {
    message.error(e.message)
  } finally {
    loading.value = false
  }
}

onMounted(load)

function openCreate() {
  form.value = { id: 0, name: '', description: '', version: '1.0.0', skills: '' }
  editing.value = false
  formOpen.value = true
}

function openEdit(card) {
  form.value = {
    id: card.id,
    name: card.name,
    description: card.description || '',
    version: card.version || '1.0.0',
    // 技能一行一个：名称 | 描述
    skills: (card.skills || [])
      .map((s) => (s.description ? `${s.name} | ${s.description}` : s.name))
      .join('\n'),
  }
  editing.value = true
  formOpen.value = true
}

/** 把一行文本解析成技能对象，id 用序号保证稳定。 */
function parseSkills(text) {
  return text
    .split('\n')
    .map((line) => line.trim())
    .filter(Boolean)
    .map((line, index) => {
      const [name, description] = line.split('|').map((part) => part.trim())
      return { id: `skill-${index + 1}`, name, description: description || '' }
    })
}

async function save() {
  if (!form.value.name.trim()) {
    message.warning('请填能力卡名字')
    return
  }
  saving.value = true
  try {
    const body = {
      name: form.value.name.trim(),
      description: form.value.description.trim(),
      version: form.value.version.trim() || '1.0.0',
      skills: parseSkills(form.value.skills),
    }
    if (editing.value) {
      await api.updateCard(form.value.id, body)
      message.success('已保存')
    } else {
      await api.createCard(body)
      message.success('已创建')
    }
    formOpen.value = false
    await load()
  } catch (e) {
    message.error(e.message)
  } finally {
    saving.value = false
  }
}

async function remove(card) {
  try {
    await api.deleteCard(card.id)
    message.success(`已删除「${card.name}」，绑定它的设备已自动解绑`)
    await load()
  } catch (e) {
    message.error(e.message)
  }
}

function openBind(card) {
  bindCard.value = card
  bindOpen.value = true
}

/** 绑定设备。设备已绑别的卡时后端返回 409，这里把原因原样展示。 */
async function bind(deviceId) {
  if (!bindCard.value) return
  binding.value = true
  try {
    await api.bindCardDevice(bindCard.value.id, deviceId)
    message.success('已绑定')
    await load()
  } catch (e) {
    message.warning(e.message)
  } finally {
    binding.value = false
  }
}

async function unbind(deviceId) {
  if (!bindCard.value) return
  binding.value = true
  try {
    await api.unbindCardDevice(bindCard.value.id, deviceId)
    message.success('已解绑')
    await load()
  } catch (e) {
    message.error(e.message)
  } finally {
    binding.value = false
  }
}

/** 这台设备在这张卡上的绑定状态。 */
function boundState(device) {
  if (!bindCard.value) return 'none'
  if (device.cardId === bindCard.value.id) return 'this'
  return device.cardId ? 'other' : 'none'
}

const columns = computed(() => [
  {
    title: '能力卡',
    key: 'name',
    minWidth: 160,
    render: (row) =>
      h('div', { style: 'display:flex;flex-direction:column;gap:2px' }, [
        h('span', { style: 'font-weight:500' }, row.name),
        row.description
          ? h('span', { style: 'color:#8a93a6;font-size:12px' }, row.description)
          : null,
      ]),
  },
  {
    title: '版本',
    key: 'version',
    width: 90,
    render: (row) =>
      h(NTag, { size: 'small', bordered: false }, { default: () => row.version || '1.0.0' }),
  },
  {
    title: '技能',
    key: 'skills',
    width: 80,
    render: (row) => h('span', {}, `${(row.skills || []).length} 个`),
  },
  {
    title: '绑定设备',
    key: 'deviceCount',
    width: 100,
    render: (row) =>
      h(
        NTag,
        {
          size: 'small',
          bordered: false,
          type: row.deviceCount > 0 ? 'success' : 'default',
        },
        { default: () => `${row.deviceCount || 0} 台` },
      ),
  },
  {
    title: '更新时间',
    key: 'updatedAt',
    width: 160,
    render: (row) => h('span', { style: 'color:#8a93a6;font-size:13px' }, row.updatedAt || ''),
  },
  {
    title: '操作',
    key: 'actions',
    width: 200,
    render: (row) =>
      h(NSpace, { size: 4, wrap: false }, {
        default: () => [
          h(
            NButton,
            { size: 'small', secondary: true, title: '绑定设备', onClick: () => openBind(row) },
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
                `删除能力卡「${row.name}」？绑定它的 ${row.deviceCount || 0} 台设备会自动解绑。`,
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
        <h2>能力卡</h2>
        <div class="muted">管理能力卡并绑定设备。一张卡可绑多台设备，一台设备只能绑一张卡。</div>
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
          新建能力卡
        </NButton>
      </NSpace>
    </div>

    <div class="grid-3" style="margin-bottom: 16px">
      <NCard size="small">
        <NStatistic label="能力卡总数">
          <span class="stat-value">{{ cards.length }}</span>
        </NStatistic>
      </NCard>
      <NCard size="small">
        <NStatistic label="绑定关系">
          <span class="stat-value ok">{{ totalBindings }}</span>
        </NStatistic>
      </NCard>
      <NCard size="small">
        <NStatistic label="未绑定的卡">
          <span class="stat-value muted">{{ cards.filter((c) => !c.deviceCount).length }}</span>
        </NStatistic>
      </NCard>
    </div>

    <NCard title="能力卡列表" size="small">
      <template #header-extra>
        <NInput v-model:value="keyword" placeholder="搜名字、描述" clearable style="width: 180px">
          <template #prefix>
            <NIcon :size="16"><SearchOutline /></NIcon>
          </template>
        </NInput>
      </template>

      <NEmpty v-if="!filtered.length" description="没有能力卡">
        <template #extra>
          <NButton size="small" secondary @click="openCreate">新建能力卡</NButton>
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
      :title="editing ? '编辑能力卡' : '新建能力卡'"
      style="width: 560px"
      :mask-closable="!saving"
    >
      <NForm label-placement="top">
        <div class="grid-2">
          <NFormItem label="名字" required>
            <NInput v-model:value="form.name" placeholder="例如：环境监测卡" />
          </NFormItem>
          <NFormItem label="版本">
            <NInput v-model:value="form.version" placeholder="例如：1.0.0" />
          </NFormItem>
        </div>
        <NFormItem label="描述">
          <NInput
            v-model:value="form.description"
            type="textarea"
            :rows="3"
            placeholder="这张卡提供什么能力"
          />
        </NFormItem>
        <NFormItem label="技能（一行一个，格式：名称 | 描述）">
          <NInput
            v-model:value="form.skills"
            type="textarea"
            :rows="5"
            placeholder="温度上报 | 读取传感器并上报&#10;阈值告警 | 超限时推送通知"
          />
        </NFormItem>
      </NForm>

      <template #footer>
        <NSpace justify="end">
          <NButton :disabled="saving" @click="formOpen = false">取消</NButton>
          <NButton type="primary" :loading="saving" @click="save">
            {{ editing ? '保存' : '创建' }}
          </NButton>
        </NSpace>
      </template>
    </NModal>

    <NModal
      v-model:show="bindOpen"
      preset="card"
      :title="`绑定设备 · ${bindCard?.name || ''}`"
      style="width: 640px"
      :mask-closable="!binding"
    >
      <NText depth="3" class="tip">
        一张卡可以绑定多台设备；一台设备只能绑一张卡，已被别的卡占用的设备要先解绑。
      </NText>
      <NEmpty v-if="!devices.length" description="还没有设备，先去设备管理登记" />
      <div v-for="device in devices" :key="device.id" class="bind-row">
        <div>
          <strong>{{ device.name }}</strong>
          <div><code>{{ device.address }}</code></div>
          <div class="muted">
            当前能力卡：{{ device.cardName || (device.cardId ? `#${device.cardId}` : '未绑定') }}
          </div>
        </div>
        <NSpace align="center">
          <NTag
            v-if="boundState(device) === 'this'"
            size="small"
            type="success"
            :bordered="false"
          >
            已绑定本卡
          </NTag>
          <NTag v-else-if="boundState(device) === 'other'" size="small" type="warning" :bordered="false">
            已绑定其他卡
          </NTag>
          <NButton
            v-if="boundState(device) === 'this'"
            size="small"
            secondary
            :loading="binding"
            @click="unbind(device.id)"
          >
            <template #icon>
              <NIcon><UnlinkOutline /></NIcon>
            </template>
            解绑
          </NButton>
          <NButton
            v-else
            size="small"
            type="primary"
            secondary
            :loading="binding"
            @click="bind(device.id)"
          >
            <template #icon>
              <NIcon><LinkOutline /></NIcon>
            </template>
            绑定
          </NButton>
        </NSpace>
      </div>

      <template #footer>
        <NSpace justify="end">
          <NButton @click="bindOpen = false">关闭</NButton>
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

.bind-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  padding: 12px 0;
  border-bottom: 1px solid #e5e7eb;
}
</style>
