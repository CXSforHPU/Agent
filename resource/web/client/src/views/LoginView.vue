<script setup>
import { ref, computed } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { NCard, NForm, NFormItem, NInput, NButton, NIcon, NText, NAlert } from 'naive-ui'
import { PersonOutline, LockClosedOutline } from '@vicons/ionicons5'
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
const title = computed(() => (isRegister.value ? '注册账号' : '登录控制台'))

const rules = {
  username: [{ required: true, message: '请输入用户名', trigger: ['blur', 'input'] }],
  password: [
    { required: true, message: '请输入密码', trigger: ['blur', 'input'] },
    { min: 6, message: '密码至少 6 位', trigger: ['blur', 'input'] },
  ],
}

async function submit() {
  error.value = ''
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
    <div class="login-side">
      <div class="brand">
        <span class="brand-logo">A</span>
        <span class="brand-name">Agent 管理控制台</span>
      </div>
      <p class="slogan">管设备、跑对话、看日志，都在这一个后台里。</p>
      <ul class="points">
        <li>设备在线探活与配置下发</li>
        <li>与端侧 agent 直接对话</li>
        <li>文件服务器、MCP、TTS 组件按需启停</li>
      </ul>
    </div>

    <div class="login-main">
      <NCard class="login-card" :bordered="false">
        <h1>{{ title }}</h1>
        <NText depth="3" class="sub">
          {{ isRegister ? '创建一个账号来管理你的设备' : '使用你的账号登录' }}
        </NText>

        <NAlert v-if="error" type="error" class="alert" :show-icon="false">
          {{ error }}
        </NAlert>

        <NForm :rules="rules" @submit.prevent="submit">
          <NFormItem label="用户名" path="username">
            <NInput
              v-model:value="username"
              placeholder="32 字符以内"
              autocomplete="username"
              clearable
            >
              <template #prefix>
                <NIcon :size="16"><PersonOutline /></NIcon>
              </template>
            </NInput>
          </NFormItem>

          <NFormItem label="密码" path="password">
            <NInput
              v-model:value="password"
              type="password"
              show-password-on="click"
              placeholder="至少 6 位"
              :autocomplete="isRegister ? 'new-password' : 'current-password'"
            >
              <template #prefix>
                <NIcon :size="16"><LockClosedOutline /></NIcon>
              </template>
            </NInput>
          </NFormItem>

          <div class="actions">
            <NButton type="primary" attr-type="submit" block :loading="busy">
              {{ busy ? '提交中……' : title }}
            </NButton>
            <NButton quaternary block @click="switchMode">
              {{ isRegister ? '已有账号？去登录' : '还没有账号？去注册' }}
            </NButton>
          </div>
        </NForm>
      </NCard>
    </div>
  </div>
</template>

<style scoped>
.login-wrap {
  min-height: 100vh;
  display: grid;
  grid-template-columns: 1.1fr 1fr;
}

.login-side {
  background: linear-gradient(150deg, #1b2540 0%, #24304f 55%, #2f6bff 160%);
  color: #e8ecf7;
  padding: 56px 60px;
  display: flex;
  flex-direction: column;
  justify-content: center;
}

.brand {
  display: flex;
  align-items: center;
  gap: 12px;
}

.brand-logo {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 38px;
  height: 38px;
  border-radius: 11px;
  background: rgba(255, 255, 255, 0.16);
  font-weight: 700;
  font-size: 19px;
}

.brand-name {
  font-size: 18px;
  font-weight: 600;
}

.slogan {
  margin: 28px 0 20px;
  font-size: 26px;
  line-height: 1.5;
  font-weight: 600;
  max-width: 420px;
}

.points {
  margin: 0;
  padding-left: 20px;
  color: #b9c3da;
  line-height: 2;
  font-size: 14px;
}

.login-main {
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 40px 24px;
  background: #f2f4f8;
}

.login-card {
  width: 380px;
  box-shadow: 0 12px 32px rgba(24, 33, 56, 0.08);
}

h1 {
  margin: 0 0 4px;
  font-size: 22px;
}

.sub {
  display: block;
  font-size: 13px;
  margin-bottom: 18px;
}

.login-card :deep(.n-form-item) {
  margin-bottom: 18px;
}

.alert {
  margin-bottom: 16px;
}

.actions {
  display: flex;
  flex-direction: column;
  gap: 6px;
  margin-top: 6px;
}

@media (max-width: 900px) {
  .login-wrap {
    grid-template-columns: 1fr;
  }

  .login-side {
    padding: 32px 28px;
  }

  .slogan {
    font-size: 20px;
  }
}
</style>
