<template>
  <nav class="breadcrumb" aria-label="Current view">
    <span class="crumb crumb-tab">{{ trail }}</span>
    <template v-for="key in DASH_FILTER_KEYS" :key="key">
      <button
        v-for="value in filtersStore[key]"
        :key="`${key}:${value}`"
        class="chip"
        :title="`Remove ${key} filter`"
        @click="filtersStore.remove(key, value)"
      >
        {{ key }} = {{ value }} <span class="chip-x" aria-hidden="true">&times;</span>
      </button>
    </template>
  </nav>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useRoute } from 'vue-router'
import { useFiltersStore } from '@/stores/filters'
import { DASH_FILTER_KEYS } from '@/utils/urlState'

const TAB_LABELS: Record<string, string> = {
  overview: 'Health',
  releases: 'Releases',
  stability: 'Stability',
  performance: 'Performance',
  fleet: 'Fleet',
  usage: 'Usage',
  prints: 'Prints',
  devices: 'Devices',
  device: 'Devices'
}

const route = useRoute()
const filtersStore = useFiltersStore()

const trail = computed(() => {
  const name = typeof route.name === 'string' ? route.name : ''
  const label = TAB_LABELS[name] ?? route.path
  if (name === 'device') {
    const id = String(route.params.id ?? '')
    return `${label} › ${id.slice(0, 8)}…`
  }
  return label
})
</script>

<style scoped>
.breadcrumb {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: 6px;
  font-size: 13px;
}

.crumb-tab {
  color: var(--text-primary);
  font-weight: 500;
  margin-right: 4px;
}

.chip {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  padding: 2px 8px;
  background: transparent;
  border: 1px solid var(--border);
  border-radius: 10px;
  color: var(--text-secondary);
  font-size: 12px;
  cursor: pointer;
}

.chip:hover {
  color: var(--accent-red);
  border-color: var(--accent-red);
}
</style>
