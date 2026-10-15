<template>
  <div class="layout">
    <aside class="sidebar">
      <div class="sidebar-header">
        <h1>HelixScreen Analytics</h1>
      </div>
      <nav class="sidebar-nav">
        <router-link :to="navTo('/')" class="nav-link" :class="{ active: route.path === '/' }">
          Overview
        </router-link>
        <router-link :to="navTo('/adoption')" class="nav-link" :class="{ active: route.path === '/adoption' }">
          Adoption
        </router-link>
        <router-link :to="navTo('/prints')" class="nav-link" :class="{ active: route.path === '/prints' }">
          Prints
        </router-link>
        <router-link :to="navTo('/stability')" class="nav-link" :class="{ active: route.path === '/stability' }">
          Stability
        </router-link>
        <router-link :to="navTo('/releases')" class="nav-link" :class="{ active: route.path === '/releases' }">
          Releases
        </router-link>
        <div class="nav-separator"></div>
        <router-link :to="navTo('/memory')" class="nav-link" :class="{ active: route.path === '/memory' }">
          Memory
        </router-link>
        <router-link :to="navTo('/hardware')" class="nav-link" :class="{ active: route.path === '/hardware' }">
          Hardware
        </router-link>
        <router-link :to="navTo('/engagement')" class="nav-link" :class="{ active: route.path === '/engagement' }">
          Engagement
        </router-link>
        <router-link :to="navTo('/performance')" class="nav-link" :class="{ active: route.path === '/performance' }">
          Performance
        </router-link>
        <router-link :to="navTo('/features')" class="nav-link" :class="{ active: route.path === '/features' }">
          Features
        </router-link>
        <router-link :to="navTo('/ux')" class="nav-link" :class="{ active: route.path === '/ux' }">
          UX Insights
        </router-link>
      </nav>
      <div class="sidebar-footer">
        <button class="logout-btn" @click="handleLogout">Logout</button>
      </div>
    </aside>
    <main class="content">
      <div class="content-header">
        <FilterBar />
      </div>
      <div class="content-body">
        <slot />
      </div>
    </main>
  </div>
</template>

<script setup lang="ts">
import { watch } from 'vue'
import { useRoute } from 'vue-router'
import { useAuthStore } from '@/stores/auth'
import { useFiltersStore } from '@/stores/filters'
import { router } from '@/router'
import FilterBar from '@/components/FilterBar.vue'

const route = useRoute()
const auth = useAuthStore()
const filters = useFiltersStore()

// The URL is the source of truth: route changes (links, back/forward) feed the
// store, and store edits are written back with replace.
// A deliberate nav click carries the active filters; history entries stay as recorded.
function navTo(path: string) {
  return { path, query: filters.queryParams }
}

watch(() => route.query, (q) => filters.syncFromRoute(q), { immediate: true })
watch(
  () => [filters.range, filters.platform, filters.version, filters.model, filters.board, filters.expand],
  () => filters.syncToRoute(router),
  { deep: true }
)

function handleLogout() {
  auth.logout()
  router.push('/login')
}
</script>

<style scoped>
.layout {
  display: flex;
  height: 100vh;
}

.sidebar {
  width: var(--sidebar-width);
  min-width: var(--sidebar-width);
  background: var(--bg-card);
  border-right: 1px solid var(--border);
  display: flex;
  flex-direction: column;
}

.sidebar-header {
  padding: 20px;
  border-bottom: 1px solid var(--border);
}

.sidebar-header h1 {
  font-size: 16px;
  font-weight: 600;
  color: var(--text-primary);
}

.sidebar-nav {
  flex: 1;
  padding: 12px 0;
  display: flex;
  flex-direction: column;
  gap: 2px;
}

.nav-link {
  display: block;
  padding: 10px 20px;
  color: var(--text-secondary);
  font-size: 14px;
  transition: color 0.15s, background 0.15s;
}

.nav-link:hover {
  color: var(--text-primary);
  background: rgba(59, 130, 246, 0.08);
}

.nav-link.active {
  color: var(--accent-blue);
  background: rgba(59, 130, 246, 0.12);
  font-weight: 500;
}

.nav-separator {
  height: 1px;
  background: var(--border);
  margin: 8px 16px;
}

.sidebar-footer {
  padding: 16px 20px;
  border-top: 1px solid var(--border);
}

.logout-btn {
  width: 100%;
  padding: 8px;
  background: transparent;
  border: 1px solid var(--border);
  border-radius: 6px;
  color: var(--text-secondary);
  font-size: 13px;
  transition: color 0.15s, border-color 0.15s;
}

.logout-btn:hover {
  color: var(--accent-red);
  border-color: var(--accent-red);
}

.content {
  flex: 1;
  overflow-y: auto;
  display: flex;
  flex-direction: column;
}

.content-header {
  padding: 16px 32px;
  border-bottom: 1px solid var(--border);
  flex-shrink: 0;
}

.content-body {
  flex: 1;
  overflow-y: auto;
  padding: 32px;
}
</style>
