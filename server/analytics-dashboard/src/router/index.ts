import { createRouter, createWebHistory } from 'vue-router'
import { useAuthStore } from '@/stores/auth'
import { legacyRedirect } from '@/utils/urlState'
import LoginView from '@/views/LoginView.vue'
import OverviewView from '@/views/OverviewView.vue'
import PrintsView from '@/views/PrintsView.vue'
import ReleasesView from '@/views/ReleasesView.vue'

export const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/login', name: 'login', component: LoginView },
    {
      path: '/',
      name: 'overview',
      component: OverviewView,
      meta: { requiresAuth: true }
    },
    {
      path: '/fleet',
      name: 'fleet',
      component: () => import('@/views/FleetView.vue'),
      meta: { requiresAuth: true }
    },
    { path: '/adoption', redirect: (to) => ({ path: legacyRedirect(to.path)!, query: to.query }) },
    { path: '/hardware', redirect: (to) => ({ path: legacyRedirect(to.path)!, query: to.query }) },
    {
      path: '/prints',
      name: 'prints',
      component: PrintsView,
      meta: { requiresAuth: true }
    },
    {
      path: '/stability',
      name: 'stability',
      component: () => import('@/views/StabilityView.vue'),
      meta: { requiresAuth: true }
    },
    { path: '/crashes', redirect: (to) => ({ path: legacyRedirect(to.path)!, query: to.query }) },
    {
      path: '/releases',
      name: 'releases',
      component: ReleasesView,
      meta: { requiresAuth: true }
    },
    { path: '/memory', redirect: (to) => ({ path: legacyRedirect(to.path)!, query: to.query }) },
    {
      path: '/usage',
      name: 'usage',
      component: () => import('@/views/UsageView.vue'),
      meta: { requiresAuth: true }
    },
    { path: '/engagement', redirect: (to) => ({ path: legacyRedirect(to.path)!, query: to.query }) },
    { path: '/reliability', redirect: (to) => ({ path: legacyRedirect(to.path)!, query: to.query }) },
    {
      path: '/performance',
      name: 'performance',
      component: () => import('@/views/PerformanceView.vue'),
      meta: { requiresAuth: true }
    },
    { path: '/features', redirect: (to) => ({ path: legacyRedirect(to.path)!, query: to.query }) },
    { path: '/ux', redirect: (to) => ({ path: legacyRedirect(to.path)!, query: to.query }) },
  ]
})

router.beforeEach((to) => {
  const auth = useAuthStore()
  if (to.meta.requiresAuth && !auth.isAuthenticated) {
    return { name: 'login' }
  }
})
