<script setup>
import { onMounted, computed } from 'vue'
import { useRouter, useRoute } from 'vue-router'
import { useAuthStore } from './store.js'

const auth = useAuthStore()
const router = useRouter()
const route = useRoute()
const routeName = computed(() => route.name)

const title = computed(() => {
  if (routeName.value === 'devices') return '设备管理'
  if (routeName.value === 'chat') return '对话'
  if (routeName.value === 'history') return '会话历史'
  if (routeName.value === 'settings') return '设置'
  if (routeName.value === 'ops') return '运维'
  return 'Agent 管理控制台'
})

onMounted(() => {
  auth.restore()
})

function logout() {
  auth.logout()
  router.push({ name: 'login' })
}
</script>

<template>
  <div class="layout">
    <header v-if="auth.loggedIn" class="topbar">
      <span class="brand">Agent 管理控制台</span>
      <span class="title">{{ title }}</span>
      <span class="spacer"></span>
      <span class="who">{{ auth.username }}</span>
      <button class="btn ghost" @click="logout">退出</button>
    </header>

    <nav v-if="auth.loggedIn" class="nav">
      <router-link to="/devices" class="nav-item">设备</router-link>
      <router-link to="/chat" class="nav-item">对话</router-link>
      <router-link to="/history" class="nav-item">会话历史</router-link>
      <router-link to="/settings" class="nav-item">设置</router-link>
      <router-link to="/ops" class="nav-item">运维</router-link>
    </nav>

    <main class="main">
      <router-view />
    </main>
  </div>
</template>
