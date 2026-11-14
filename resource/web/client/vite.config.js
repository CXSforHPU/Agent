import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// 开发时 5173 由 setup 脚本拉起，后端地址默认 8080；
// 端到端测试可用 AGENT_API_TARGET 指到独立端口，避免撞上正在运行的实例。
export default defineConfig({
  plugins: [vue()],
  server: {
    port: Number(process.env.AGENT_CLIENT_PORT || 5173),
    host: '127.0.0.1',
    strictPort: false,
    proxy: {
      // 走同源代理，前端不用自己处理跨域，JWT 也不用到处传域
      '/api': {
        target: process.env.AGENT_API_TARGET || 'http://127.0.0.1:8080',
        changeOrigin: true,
      },
    },
  },
  preview: {
    port: 5173,
    host: '127.0.0.1',
  },
})
