import { createRouter, createWebHistory } from 'vue-router'
import { isLoggedIn } from './api.js'
const LoginView = () => import('./views/LoginView.vue')
const DevicesView = () => import('./views/DevicesView.vue')
const CardsView = () => import('./views/CardsView.vue')
const ChatView = () => import('./views/ChatView.vue')
const HistoryView = () => import('./views/HistoryView.vue')
const SettingsView = () => import('./views/SettingsView.vue')
const OpsView = () => import('./views/OpsView.vue')

const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/', redirect: '/dashboard' },
    { path: '/dashboard', name: 'dashboard', component: () => import('./views/DashboardView.vue') },
    { path: '/login', name: 'login', component: LoginView, meta: { public: true } },
    { path: '/devices', name: 'devices', component: DevicesView },
    { path: '/cards', name: 'cards', component: CardsView },
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
