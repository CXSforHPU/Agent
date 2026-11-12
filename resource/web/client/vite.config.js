import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// 开发时 5173 由 setup 脚本拉起，后端 8080 的地址在 .env 里改
export default defineConfig({
  plugins: [vue()],
  server: {
    port: 5173,
    host: '127.0.0.1',
    proxy: {
      // 走同源代理，前端不用自己处理跨域，JWT 也不用到处传域
      '/api': {
        target: 'http://127.0.0.1:8080',
        changeOrigin: true,
      },
    },
  },
  preview: {
    port: 5173,
    host: '127.0.0.1',
  },
})
