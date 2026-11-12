import { createRouter, createWebHistory } from 'vue-router'
import { isLoggedIn } from './api.js'
import LoginView from './views/LoginView.vue'
import DevicesView from './views/DevicesView.vue'
import ChatView from './views/ChatView.vue'
import HistoryView from './views/HistoryView.vue'
import SettingsView from './views/SettingsView.vue'
import OpsView from './views/OpsView.vue'

const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/', redirect: '/devices' },
    { path: '/login', name: 'login', component: LoginView, meta: { public: true } },
    { path: '/devices', name: 'devices', component: DevicesView },
    { path: '/chat', name: 'chat', component: ChatView },
    { path: '/history', name: 'history', component: HistoryView },
    { path: '/settings', name: 'settings', component: SettingsView },
    { path: '/ops', name: 'ops', component: OpsView },
    { path: '/:pathMatch(.*)*', redirect: '/devices' },
  ],
})

router.beforeEach((to) => {
  if (to.meta.public) {
    return true
  }
  return isLoggedIn() ? true : { name: 'login', query: { next: to.fullPath } }
})

export default router
