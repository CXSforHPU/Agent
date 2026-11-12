<script setup>
import { ref, computed } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { useAuthStore } from '../store.js'

const auth = useAuthStore()
const route = useRoute()
const router = useRouter()

const mode = ref('login')
const username = ref('')
const password = ref('')
const error = ref('')
const busy = ref(false)

const isRegister = computed(() => mode.value === 'register')
const title = computed(() => (isRegister.value ? '注册账号' : '登录'))

async function submit() {
  error.value = ''
  if (!username.value.trim()) {
    error.value = '请输入用户名'
    return
  }
  if (password.value.length < 6) {
    error.value = '密码至少 6 位'
    return
  }
  busy.value = true
  try {
    if (isRegister.value) {
      await auth.register(username.value.trim(), password.value)
    } else {
      await auth.login(username.value.trim(), password.value)
    }
    const next = route.query.next
    router.push(typeof next === 'string' && next ? next : '/devices')
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}

function switchMode() {
  mode.value = isRegister.value ? 'login' : 'register'
  error.value = ''
}
</script>

<template>
  <div class="login-wrap">
    <form class="panel login-card" @submit.prevent="submit">
      <h3>{{ title }}</h3>

      <div class="field">
        <label for="u">用户名</label>
        <input id="u" v-model="username" class="input" autocomplete="username" placeholder="32 字符以内" />
      </div>

      <div class="field">
        <label for="p">密码</label>
        <input id="p" v-model="password" class="input" type="password"
               :autocomplete="isRegister ? 'new-password' : 'current-password'"
               placeholder="至少 6 位" />
      </div>

      <p v-if="error" class="error">{{ error }}</p>

      <div class="actions">
        <button class="btn" type="submit" :disabled="busy">
          {{ busy ? '提交中……' : title }}
        </button>
        <button class="btn ghost" type="button" @click="switchMode">
          {{ isRegister ? '去登录' : '去注册' }}
        </button>
      </div>
    </form>
  </div>
</template>

<style scoped>
.login-wrap {
  min-height: 100vh;
  display: flex;
  align-items: center;
  justify-content: center;
}

.login-card {
  width: 340px;
  margin-bottom: 0;
}

.actions {
  display: flex;
  gap: 8px;
}
</style>
