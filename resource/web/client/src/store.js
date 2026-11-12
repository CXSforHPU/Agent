import { defineStore } from 'pinia'
import { api, setToken, getToken } from './api.js'

export const useAuthStore = defineStore('auth', {
  state: () => ({
    username: '',
    userId: 0,
    // 启动时先从 localStorage 恢复，再用 /me 校验一次
    ready: false,
  }),
  getters: {
    loggedIn: (state) => Boolean(state.username),
  },
  actions: {
    async restore() {
      if (!getToken()) {
        this.ready = true
        return
      }
      try {
        const me = await api.me()
        this.username = me.username
        this.userId = me.userId
      } catch {
        setToken('')
      } finally {
        this.ready = true
      }
    },
    async login(username, password) {
      const data = await api.login(username, password)
      setToken(data.token)
      this.username = data.username
      this.userId = data.userId
      this.ready = true
    },
    async register(username, password) {
      const data = await api.register(username, password)
      setToken(data.token)
      this.username = data.username
      this.userId = data.userId
      this.ready = true
    },
    logout() {
      setToken('')
      this.username = ''
      this.userId = 0
    },
  },
})
