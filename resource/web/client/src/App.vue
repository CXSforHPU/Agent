<script setup>
import { onMounted, onUnmounted, computed, h, ref } from 'vue'
import { useRouter, useRoute } from 'vue-router'
import {
  NConfigProvider,
  NMessageProvider,
  NDialogProvider,
  NNotificationProvider,
  NLayout,
  NLayoutSider,
  NLayoutHeader,
  NLayoutContent,
  NMenu,
  NButton,
  NIcon,
  NText,
  zhCN,
  dateZhCN,
} from 'naive-ui'
import {
  GridOutline,
  HardwareChipOutline,
  IdCardOutline,
  ChatbubblesOutline,
  TimeOutline,
  SettingsOutline,
  ServerOutline,
  LogOutOutline,
  MenuOutline,
} from '@vicons/ionicons5'
import { useAuthStore } from './store.js'
import { themeOverrides } from './theme.js'

const auth = useAuthStore()
const router = useRouter()
const route = useRoute()

const narrowScreen = window.matchMedia('(max-width: 760px)')
const collapsed = ref(narrowScreen.matches)
function syncNavigation(event) {
  collapsed.value = event.matches
}
onMounted(() => narrowScreen.addEventListener('change', syncNavigation))
onUnmounted(() => narrowScreen.removeEventListener('change', syncNavigation))

const menus = [
  { label: '总览仪表盘', key: 'dashboard', icon: GridOutline },
  { label: '设备管理', key: 'devices', icon: HardwareChipOutline },
  { label: '能力卡', key: 'cards', icon: IdCardOutline },
  { label: '对话', key: 'chat', icon: ChatbubblesOutline },
  { label: '会话历史', key: 'history', icon: TimeOutline },
  { label: '设置', key: 'settings', icon: SettingsOutline },
  { label: '运维', key: 'ops', icon: ServerOutline },
]

const menuOptions = menus.map((m) => ({
  label: m.label,
  key: m.key,
  icon: () => h(NIcon, null, { default: () => h(m.icon) }),
}))

const activeKey = computed(() => route.name || 'devices')

const currentTitle = computed(() => {
  const hit = menus.find((m) => m.key === activeKey.value)
  return hit ? hit.label : 'Agent 管理控制台'
})

// 登录页不套后台外壳
const bare = computed(() => route.meta.public === true)

onMounted(() => {
  auth.restore()
})

function onMenuSelect(key) {
  if (key !== activeKey.value) {
    router.push({ name: key })
  }
}

function logout() {
  auth.logout()
  router.push({ name: 'login' })
}
</script>

<template>
  <NConfigProvider :theme-overrides="themeOverrides" :locale="zhCN" :date-locale="dateZhCN">
    <NMessageProvider>
      <NDialogProvider>
        <NNotificationProvider>
          <router-view v-if="bare" />

          <NLayout v-else has-sider position="absolute">
            <NLayoutSider
              bordered
              collapse-mode="width"
              :collapsed-width="64"
              :width="244"
              :collapsed="collapsed"
              :native-scrollbar="false"
            >
              <div class="brand">
                <span class="brand-logo"><NIcon :size="22"><HardwareChipOutline /></NIcon></span>
                <span v-show="!collapsed" class="brand-text">Agent Console</span>
              </div>
              <NMenu
                :options="menuOptions"
                :value="activeKey"
                :collapsed="collapsed"
                :collapsed-width="64"
                :collapsed-icon-size="20"
                :indent="18"
                @update:value="onMenuSelect"
              />
            </NLayoutSider>

            <NLayout>
              <NLayoutHeader bordered class="topbar">
                <NButton quaternary circle size="small" aria-label="切换导航" :aria-expanded="!collapsed" @click="collapsed = !collapsed">
                  <template #icon>
                    <NIcon :size="18"><MenuOutline /></NIcon>
                  </template>
                </NButton>
                <NText strong class="page-title">工作空间 / {{ currentTitle }}</NText>
                <div class="spacer"></div>
                <NText depth="3" class="who">{{ auth.username }}</NText>
                <NButton quaternary size="small" @click="logout">
                  <template #icon>
                    <NIcon :size="16"><LogOutOutline /></NIcon>
                  </template>
                  退出
                </NButton>
              </NLayoutHeader>

              <NLayoutContent class="content" :native-scrollbar="false">
                <div class="content-inner">
                  <router-view />
                </div>
              </NLayoutContent>
            </NLayout>
          </NLayout>
        </NNotificationProvider>
      </NDialogProvider>
    </NMessageProvider>
  </NConfigProvider>
</template>

<style scoped>
.brand {
  display: flex;
  align-items: center;
  gap: 12px;
  height: 84px;
  padding: 0 17px;
  white-space: nowrap;
  overflow: hidden;
}
.brand-logo {
  display: grid;
  place-items: center;
  width: 36px;
  height: 36px;
  flex: 0 0 36px;
  border-radius: 12px;
  color: #fff;
  background: #6366f1;
  box-shadow: 0 5px 12px #6366f12b;
}
.brand-text {
  color: #20253d;
  font-weight: 700;
  font-size: 18px;
  letter-spacing: -.5px;
}
.topbar {
  display: flex;
  align-items: center;
  gap: 16px;
  padding: 0 28px;
}
.page-title { font-size: 13px; color: #737b91; }
.spacer { flex: 1; }
.who { font-size: 13px; }
.content { background: #f5f6fa; }
.content-inner {
  max-width: 1600px;
  margin: 0 auto;
  padding: 32px;
}
@media (max-width: 760px) {
  .content-inner { padding: 16px 12px; }
  .topbar { padding: 0 12px; gap: 8px; }
  .who { display: none; }
  .page-title { font-size: 12px; }
}
</style>
