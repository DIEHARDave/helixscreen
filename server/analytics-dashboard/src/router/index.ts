import { createRouter, createWebHistory, type RouteRecordRaw } from 'vue-router'
import { useAuthStore } from '@/stores/auth'
import { LEGACY_ROUTES, legacyLocation } from '@/utils/urlState'
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
      path: '/releases',
      name: 'releases',
      component: ReleasesView,
      meta: { requiresAuth: true }
    },
    {
      path: '/stability',
      name: 'stability',
      component: () => import('@/views/StabilityView.vue'),
      meta: { requiresAuth: true }
    },
    {
      path: '/performance',
      name: 'performance',
      component: () => import('@/views/PerformanceView.vue'),
      meta: { requiresAuth: true }
    },
    {
      path: '/fleet',
      name: 'fleet',
      component: () => import('@/views/FleetView.vue'),
      meta: { requiresAuth: true }
    },
    {
      path: '/usage',
      name: 'usage',
      component: () => import('@/views/UsageView.vue'),
      meta: { requiresAuth: true }
    },
    {
      path: '/prints',
      name: 'prints',
      component: PrintsView,
      meta: { requiresAuth: true }
    },
    // Old bookmarks land on the tab that holds their content, filters intact.
    ...LEGACY_ROUTES.map((path): RouteRecordRaw => ({
      path,
      redirect: (to) => legacyLocation(path, to.query)!
    }))
  ]
})

router.beforeEach((to) => {
  const auth = useAuthStore()
  if (to.meta.requiresAuth && !auth.isAuthenticated) {
    return { name: 'login' }
  }
})
