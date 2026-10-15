<template>
  <AppLayout>
    <div class="page">
      <div class="page-header">
        <h2>Releases</h2>
      </div>

      <div class="version-selector">
        <h3>Select up to 5 versions to compare</h3>
        <div class="version-pills">
          <button
            v-for="v in availableVersions"
            :key="v"
            class="version-pill"
            :class="{ active: selectedVersions.includes(v), disabled: !selectedVersions.includes(v) && selectedVersions.length >= 5 }"
            :disabled="!selectedVersions.includes(v) && selectedVersions.length >= 5"
            @click="toggleVersion(v)"
          >
            {{ v }}
          </button>
        </div>
      </div>

      <div v-if="loading" class="loading">Loading...</div>
      <div v-else-if="error" class="error">{{ error }}</div>
      <div v-else-if="sortedVersions.length > 0" class="comparison-wrapper">
        <table class="comparison-table">
          <thead>
            <tr>
              <th class="metric-col"></th>
              <th v-for="ver in sortedVersions" :key="ver.version" class="version-col">
                <span class="version-header">{{ ver.version }}</span>
              </th>
            </tr>
          </thead>
          <tbody>
            <tr>
              <td class="metric-label">Active Devices</td>
              <td v-for="ver in sortedVersions" :key="ver.version" class="metric-value">
                {{ ver.active_devices.toLocaleString() }}
              </td>
            </tr>
            <tr>
              <td class="metric-label">Sessions</td>
              <td v-for="ver in sortedVersions" :key="ver.version" class="metric-value">
                {{ ver.total_sessions.toLocaleString() }}
              </td>
            </tr>
            <tr>
              <td class="metric-label">Crash Rate</td>
              <td
                v-for="ver in sortedVersions"
                :key="ver.version"
                class="metric-value"
                :class="crashRateClass(ver.crash_rate)"
              >
                {{ formatRate(ver.crash_rate) }}
                <span v-if="crashDelta(ver) !== null" class="delta" :class="crashDelta(ver)! > 0 ? 'delta-bad' : 'delta-good'">
                  {{ crashDelta(ver)! > 0 ? '+' : '' }}{{ crashDelta(ver)!.toFixed(1) }}pp
                </span>
              </td>
            </tr>
            <tr>
              <td class="metric-label">Print Success</td>
              <td
                v-for="ver in sortedVersions"
                :key="ver.version"
                class="metric-value"
                :class="successRateClass(ver.print_success_rate)"
              >
                {{ formatRate(ver.print_success_rate) }}
                <span v-if="successDelta(ver) !== null" class="delta" :class="successDelta(ver)! > 0 ? 'delta-good' : 'delta-bad'">
                  {{ successDelta(ver)! > 0 ? '+' : '' }}{{ successDelta(ver)!.toFixed(1) }}pp
                </span>
              </td>
            </tr>
            <tr>
              <td class="metric-label">Total Crashes</td>
              <td v-for="ver in sortedVersions" :key="ver.version" class="metric-value crashes">
                {{ ver.total_crashes.toLocaleString() }}
              </td>
            </tr>
          </tbody>
        </table>
      </div>
      <div v-else class="empty">Select versions above to compare</div>
    </div>
  </AppLayout>
</template>

<script setup lang="ts">
import { ref, watch, computed } from 'vue'
import AppLayout from '@/components/AppLayout.vue'
import { useFiltersStore } from '@/stores/filters'
import { api } from '@/services/api'
import type { ReleasesData } from '@/services/api'
import { compareVersions } from '@/utils/chart'
import { formatRate } from '@/utils/format'

const MAX_SELECTIONS = 5

const filters = useFiltersStore()
const availableVersions = ref<string[]>([])
const selectedVersions = ref<string[]>([])
const data = ref<ReleasesData | null>(null)
const loading = ref(false)
const error = ref('')

function toggleVersion(v: string) {
  const idx = selectedVersions.value.indexOf(v)
  if (idx >= 0) {
    selectedVersions.value.splice(idx, 1)
  } else if (selectedVersions.value.length < MAX_SELECTIONS) {
    selectedVersions.value.push(v)
  }
}

async function fetchVersionList() {
  try {
    const adoption = await api.getAdoption(filters.queryString)
    availableVersions.value = adoption.versions
      .map(v => v.name)
      .sort(compareVersions)
    selectedVersions.value = selectedVersions.value.filter(v => availableVersions.value.includes(v))
  } catch {
    // Silently fail
  }
}

const sortedVersions = computed(() =>
  [...(data.value?.versions ?? [])].sort((a, b) => compareVersions(a.version, b.version))
)

// Delta helpers: compare each version to its predecessor in the sorted list
function crashDelta(ver: { version: string; crash_rate: number | null }): number | null {
  const idx = sortedVersions.value.findIndex(v => v.version === ver.version)
  if (idx <= 0) return null
  const prev = sortedVersions.value[idx - 1]
  if (ver.crash_rate === null || prev.crash_rate === null) return null
  return (ver.crash_rate - prev.crash_rate) * 100
}

function successDelta(ver: { version: string; print_success_rate: number | null }): number | null {
  const idx = sortedVersions.value.findIndex(v => v.version === ver.version)
  if (idx <= 0) return null
  const prev = sortedVersions.value[idx - 1]
  if (ver.print_success_rate === null || prev.print_success_rate === null) return null
  return (ver.print_success_rate - prev.print_success_rate) * 100
}

function crashRateClass(rate: number | null): string {
  if (rate === null) return ''
  const pct = rate * 100
  if (pct > 10) return 'rate-bad'
  if (pct > 5) return 'rate-warn'
  return 'rate-good'
}

function successRateClass(rate: number | null): string {
  if (rate === null) return ''
  const pct = rate * 100
  if (pct >= 80) return 'rate-good'
  if (pct >= 60) return 'rate-warn'
  return 'rate-bad'
}

async function fetchReleases() {
  if (selectedVersions.value.length === 0) {
    data.value = null
    return
  }
  loading.value = true
  error.value = ''
  try {
    data.value = await api.getReleases(filters.queryString, selectedVersions.value)
  } catch (e) {
    error.value = e instanceof Error ? e.message : 'Failed to load data'
  } finally {
    loading.value = false
  }
}

watch(selectedVersions, fetchReleases, { deep: true })
watch(() => filters.queryString, fetchVersionList, { immediate: true })
</script>

<style scoped>
.page-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 24px;
}

.page-header h2 {
  font-size: 20px;
  font-weight: 600;
}

.version-selector {
  margin-bottom: 24px;
}

.version-selector h3 {
  font-size: 14px;
  font-weight: 500;
  color: var(--text-secondary);
  margin-bottom: 12px;
}

.version-pills {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}

.version-pill {
  padding: 6px 14px;
  background: var(--bg-card);
  border: 1px solid var(--border);
  border-radius: 20px;
  color: var(--text-secondary);
  font-size: 13px;
  cursor: pointer;
  transition: all 0.15s;
}

.version-pill:hover:not(.disabled) {
  border-color: var(--accent-blue);
  color: var(--text-primary);
}

.version-pill.active {
  background: rgba(59, 130, 246, 0.15);
  border-color: var(--accent-blue);
  color: var(--accent-blue);
}

.version-pill.disabled {
  opacity: 0.35;
  cursor: not-allowed;
}

.comparison-wrapper {
  background: var(--bg-card);
  border: 1px solid var(--border);
  border-radius: 8px;
  overflow-x: auto;
}

.comparison-table {
  width: 100%;
  border-collapse: collapse;
}

.comparison-table th,
.comparison-table td {
  padding: 14px 20px;
  text-align: center;
  border-bottom: 1px solid var(--border);
}

.comparison-table tbody tr:last-child td {
  border-bottom: none;
}

.comparison-table tbody tr:hover {
  background: rgba(255, 255, 255, 0.02);
}

.metric-col {
  width: 160px;
  min-width: 160px;
}

.version-col {
  min-width: 140px;
}

.version-header {
  font-size: 16px;
  font-weight: 700;
  color: var(--accent-blue);
}

.metric-label {
  text-align: left;
  font-size: 13px;
  font-weight: 500;
  color: var(--text-secondary);
  white-space: nowrap;
}

.metric-value {
  font-size: 15px;
  font-weight: 600;
  color: var(--text-primary);
  font-family: 'SF Mono', 'Fira Code', monospace;
  position: relative;
}

.metric-value.crashes {
  color: var(--accent-red);
}

.metric-value.rate-good {
  color: var(--accent-green);
}

.metric-value.rate-warn {
  color: var(--accent-yellow);
}

.metric-value.rate-bad {
  color: var(--accent-red);
}

.delta {
  display: block;
  font-size: 11px;
  font-weight: 500;
  margin-top: 2px;
}

.delta-good {
  color: var(--accent-green);
}

.delta-bad {
  color: var(--accent-red);
}

.loading, .error, .empty {
  padding: 40px;
  text-align: center;
  color: var(--text-secondary);
}

.error {
  color: var(--accent-red);
}
</style>
