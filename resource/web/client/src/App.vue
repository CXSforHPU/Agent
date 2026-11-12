<script setup>
import { onMounted, computed, h, ref } from 'vue'
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
  HardwareChipOutline,
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

const collapsed = ref(false)

const menus = [
  { label: '设备管理', key: 'devices', icon: HardwareChipOutline },
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
              :width="216"
              :collapsed="collapsed"
              :native-scrollbar="false"
            >
              <div class="brand">
                <span class="brand-logo">A</span>
                <span v-show="!collapsed" class="brand-text">Agent 控制台</span>
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
                <NButton quaternary circle size="small" @click="collapsed = !collapsed">
                  <template #icon>
                    <NIcon :size="18"><MenuOutline /></NIcon>
                  </template>
                </NButton>
                <NText strong class="page-title">{{ currentTitle }}</NText>
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
  gap: 10px;
  height: 58px;
  padding: 0 16px;
  overflow: hidden;
  white-space: nowrap;
}

.brand-logo {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 30px;
  height: 30px;
  flex: 0 0 30px;
  border-radius: 9px;
  background: linear-gradient(135deg, #2f6bff, #6d4bff);
  color: #fff;
  font-weight: 700;
  font-size: 16px;
}

.brand-text {
  color: #e8ecf7;
  font-weight: 600;
  font-size: 15px;
}

.topbar {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 0 20px;
}

.page-title {
  font-size: 15px;
}

.spacer {
  flex: 1;
}

.who {
  font-size: 13px;
}

.content {
  background: #f2f4f8;
}

.content-inner {
  max-width: 1280px;
  margin: 0 auto;
  padding: 20px;
}
</style>
