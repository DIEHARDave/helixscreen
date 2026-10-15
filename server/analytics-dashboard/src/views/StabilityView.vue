<template>
  <AppLayout>
    <div class="page">
      <div class="page-header">
        <h2>Stability</h2>
      </div>

      <div v-if="loading" class="loading">Loading...</div>
      <div v-else-if="error" class="error">{{ error }}</div>
      <template v-else-if="data">
        <div class="metrics-row">
          <MetricCard
            title="Crash Rate"
            :value="formatRate(overallCrashRate)"
            subtitle="of sessions"
            color="var(--accent-red)"
          />
          <MetricCard
            title="Avg Uptime Before Crash"
            :value="formatDuration(data.avg_uptime_sec)"
            subtitle="before crash"
            color="var(--accent-yellow)"
          />
          <MetricCard
            title="Memory Warnings"
            :value="totalMemoryWarnings.toLocaleString()"
            subtitle="in period"
            color="#f59e0b"
          />
          <MetricCard
            title="Klippy Errors"
            :value="data.delta_era_devices > 0 ? totalKlippyErrors.toLocaleString() : '\u2014'"
            :subtitle="data.delta_era_devices > 0 ? 'in period' : waitingForNewClient(data.delta_era_devices)"
            color="#f97316"
          />
        </div>

        <div class="chart-section">
          <h3>Crash Rate Over Time</h3>
          <LineChart :data="crashRateChartData" :options="crashRateOpts" />
        </div>

        <div class="grid-2col">
          <div class="chart-section">
            <h3>Crash Rate by Version</h3>
            <BarChart :data="versionChartData" :options="barPercentOpts" />
          </div>
          <div class="chart-section">
            <h3>Crashes by Signal</h3>
            <PieChart :data="signalChartData" />
          </div>
        </div>

        <div class="grid-2col">
          <div class="chart-section">
            <h3>Klippy Errors &amp; Shutdowns</h3>
            <DeltaEraWaiting v-if="data.delta_era_devices === 0" :devices="data.delta_era_devices" />
            <LineChart v-else :data="klippyChartData" />
          </div>
          <div class="chart-section">
            <h3>Memory Warnings Over Time</h3>
            <LineChart :data="memoryWarningsChartData" />
          </div>
        </div>

        <div class="chart-section">
          <h3>Error Hotspots</h3>
          <BarChart :data="errorCategoryChartData" :options="horizontalBarOpts" />
        </div>

        <div class="chart-section">
          <h3>Error Codes</h3>
          <div v-if="data.error_codes.length === 0" class="empty-state">No error codes found in this period.</div>
          <div v-else class="table-wrapper">
            <table class="data-table">
              <thead>
                <tr>
                  <th>Category</th>
                  <th>Code</th>
                  <th>Count</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="(row, i) in data.error_codes" :key="i">
                  <td><span class="badge category">{{ row.category }}</span></td>
                  <td class="mono">{{ row.code }}</td>
                  <td class="mono">{{ row.count.toLocaleString() }}</td>
                </tr>
              </tbody>
            </table>
          </div>
        </div>

        <!-- LVGL Display Anomalies -->
        <template v-if="data.display_anomalies">
          <div class="section-divider">
            <h2>LVGL Display Anomalies</h2>
            <span class="section-subtitle">Double-schedule, use-after-free near-miss, event chain corruption</span>
          </div>

          <div class="metrics-row">
            <MetricCard
              title="Display Anomalies"
              :value="totalDisplayAnomalies.toLocaleString()"
              subtitle="in period"
              color="#a855f7"
            />
            <MetricCard
              title="Affected Devices"
              :value="(data.display_anomalies.affected_devices ?? 0).toLocaleString()"
              subtitle="with anomalies"
              color="#8b5cf6"
            />
            <MetricCard
              title="Anomaly Codes"
              :value="(data.display_anomalies.by_code?.length ?? 0).toString()"
              subtitle="distinct types"
              color="#7c3aed"
            />
            <MetricCard
              title="Affected Versions"
              :value="(data.display_anomalies.by_version?.length ?? 0).toString()"
              subtitle="with anomalies"
              color="#6d28d9"
            />
          </div>

          <div class="chart-section" v-if="displayTrendChartData.labels.length > 0">
            <h3>Display Anomalies Over Time</h3>
            <LineChart :data="displayTrendChartData" />
          </div>

          <div class="grid-2col" v-if="displayCodeChartData.labels.length > 0 || displayVersionChartData.labels.length > 0">
            <div class="chart-section" v-if="displayCodeChartData.labels.length > 0">
              <h3>By Anomaly Code</h3>
              <BarChart :data="displayCodeChartData" :options="horizontalBarOpts" />
            </div>
            <div class="chart-section" v-if="displayVersionChartData.labels.length > 0">
              <h3>By Version</h3>
              <BarChart :data="displayVersionChartData" :options="horizontalBarOpts" />
            </div>
          </div>

          <div class="chart-section">
            <h3>Recent Display Anomalies</h3>
            <div v-if="!data.display_anomalies.recent?.length" class="empty-state">No display anomalies found in this period.</div>
            <div v-else class="table-wrapper">
              <table class="data-table">
                <thead>
                  <tr>
                    <th>Time</th>
                    <th>Version</th>
                    <th>Platform</th>
                    <th>Code</th>
                    <th>Uptime</th>
                    <th>Device</th>
                    <th>Context</th>
                  </tr>
                </thead>
                <tbody>
                  <tr v-for="(row, i) in data.display_anomalies.recent" :key="i">
                    <td class="mono">{{ formatTimestamp(row.timestamp) }}</td>
                    <td><span class="badge version">{{ row.version || '—' }}</span></td>
                    <td>{{ row.platform || '—' }}</td>
                    <td><span class="badge anomaly-code">{{ row.code }}</span></td>
                    <td class="mono">{{ formatDuration(row.uptime_sec) }}</td>
                    <td class="mono device-id">{{ shortDeviceId(row.device_id) }}</td>
                    <td class="mono context-cell" :title="row.context">{{ truncateContext(row.context) }}</td>
                  </tr>
                </tbody>
              </table>
            </div>
          </div>
        </template>

        <div class="chart-section">
          <h3>Recent Crashes</h3>
          <div v-if="data.recent_crashes.length === 0" class="empty-state">No crash events found in this period.</div>
          <div v-else class="table-wrapper">
            <table class="crash-table">
              <thead>
                <tr>
                  <th>Time</th>
                  <th>Version</th>
                  <th>Signal</th>
                  <th>Platform</th>
                  <th>Uptime</th>
                  <th>Device</th>
                  <th>Count</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="(crash, i) in data.recent_crashes" :key="i">
                  <td class="mono">{{ formatTimestamp(crash.timestamp) }}</td>
                  <td><span class="badge version">{{ crash.version || '\u2014' }}</span></td>
                  <td><span class="badge" :class="signalClass(crash.signal)">{{ crash.signal || '\u2014' }}</span></td>
                  <td>{{ crash.platform || '\u2014' }}</td>
                  <td class="mono">{{ formatDuration(crash.uptime_sec) }}</td>
                  <td class="mono device-id">{{ shortDeviceId(crash.device_id) }}</td>
                  <td class="mono">
                    <span v-if="crash.occurrences > 1" class="badge occurrences">{{ crash.occurrences }}×</span>
                    <span v-else>1</span>
                  </td>
                </tr>
              </tbody>
            </table>
          </div>
        </div>
      </template>
    </div>
  </AppLayout>
</template>

<script setup lang="ts">
import { ref, watch, computed } from 'vue'
import AppLayout from '@/components/AppLayout.vue'
import MetricCard from '@/components/MetricCard.vue'
import DeltaEraWaiting from '@/components/DeltaEraWaiting.vue'
import LineChart from '@/components/LineChart.vue'
import BarChart from '@/components/BarChart.vue'
import PieChart from '@/components/PieChart.vue'
import { useFiltersStore } from '@/stores/filters'
import { api } from '@/services/api'
import type { StabilityData } from '@/services/api'
import type { ChartOptions } from 'chart.js'
import { horizontalBarOpts, compareVersions } from '@/utils/chart'
import { formatDuration, formatRate, formatTimestamp, shortDeviceId, waitingForNewClient } from '@/utils/format'

const COLORS = ['#3b82f6', '#10b981', '#f59e0b', '#ef4444', '#8b5cf6', '#ec4899', '#06b6d4', '#84cc16']

const filters = useFiltersStore()
const data = ref<StabilityData | null>(null)
const loading = ref(true)
const error = ref('')

function signalClass(signal: string): string {
  if (signal === 'SIGSEGV') return 'signal-segv'
  if (signal === 'SIGABRT') return 'signal-abrt'
  return 'signal-other'
}

// 0-1, or null when the filters make the crash rate inapplicable.
const overallCrashRate = computed((): number | null => {
  const trend = data.value?.crash_rate_trend ?? []
  if (trend.some(d => d.rate === null)) return null
  const crashes = trend.reduce((sum, d) => sum + d.crashes, 0)
  const sessions = trend.reduce((sum, d) => sum + d.sessions, 0)
  return sessions > 0 ? crashes / sessions : 0
})

const totalMemoryWarnings = computed(() =>
  (data.value?.memory_warnings_trend ?? []).reduce((sum, d) => sum + d.count, 0)
)

const totalKlippyErrors = computed(() =>
  (data.value?.klippy_trend ?? []).reduce((sum, d) => sum + d.errors, 0)
)

const crashRateOpts: ChartOptions<'line'> = {
  scales: {
    y: {
      ticks: {
        callback: (value) => `${value}%`,
        color: '#94a3b8',
      },
      grid: { color: 'rgba(45, 51, 72, 0.5)' },
    },
    x: {
      ticks: { color: '#94a3b8' },
      grid: { color: 'rgba(45, 51, 72, 0.5)' },
    },
  },
  plugins: {
    tooltip: {
      callbacks: {
        label: (ctx) => {
          const idx = ctx.dataIndex
          const trend = data.value?.crash_rate_trend ?? []
          const point = trend[idx]
          if (point) {
            return `Rate: ${formatRate(point.rate, 2)} (${point.crashes} crashes / ${point.sessions} sessions)`
          }
          return `${ctx.formattedValue}%`
        }
      }
    }
  }
}

const barPercentOpts: ChartOptions<'bar'> = {
  scales: {
    y: {
      ticks: {
        callback: (value) => `${value}%`,
        color: '#94a3b8',
      },
      grid: { color: 'rgba(45, 51, 72, 0.5)' },
    },
    x: {
      ticks: { color: '#94a3b8' },
      grid: { color: 'rgba(45, 51, 72, 0.5)' },
    },
  },
}

const crashRateChartData = computed(() => ({
  labels: data.value?.crash_rate_trend.map(d => d.date) ?? [],
  datasets: [{
    label: 'Crash Rate %',
    data: data.value?.crash_rate_trend.map(d => (d.rate === null ? null : d.rate * 100)) ?? [],
    borderColor: '#ef4444',
    backgroundColor: 'rgba(239, 68, 68, 0.1)',
    fill: true,
    tension: 0.3
  }]
}))

const versionChartData = computed(() => {
  const sorted = [...(data.value?.by_version ?? [])].sort((a, b) => compareVersions(a.version, b.version))
  return {
    labels: sorted.map(v => v.version),
    datasets: [{
      label: 'Crash Rate %',
      data: sorted.map(v => (v.rate === null ? null : v.rate * 100)),
      backgroundColor: '#ef4444'
    }]
  }
})

const signalChartData = computed(() => ({
  labels: data.value?.by_signal.map(s => s.signal) ?? [],
  datasets: [{
    data: data.value?.by_signal.map(s => s.count) ?? [],
    backgroundColor: COLORS
  }]
}))

const klippyChartData = computed(() => ({
  labels: data.value?.klippy_trend.map(d => d.date) ?? [],
  datasets: [
    {
      label: 'Errors',
      data: data.value?.klippy_trend.map(d => d.errors) ?? [],
      borderColor: '#f97316',
      backgroundColor: 'rgba(249, 115, 22, 0.1)',
      fill: false,
      tension: 0.3
    },
    {
      label: 'Shutdowns',
      data: data.value?.klippy_trend.map(d => d.shutdowns) ?? [],
      borderColor: '#ef4444',
      backgroundColor: 'rgba(239, 68, 68, 0.1)',
      fill: false,
      tension: 0.3
    }
  ]
}))

const memoryWarningsChartData = computed(() => ({
  labels: data.value?.memory_warnings_trend.map(d => d.date) ?? [],
  datasets: [{
    label: 'Warnings',
    data: data.value?.memory_warnings_trend.map(d => d.count) ?? [],
    borderColor: '#f59e0b',
    backgroundColor: 'rgba(245, 158, 11, 0.1)',
    fill: true,
    tension: 0.3
  }]
}))

const errorCategoryChartData = computed(() => ({
  labels: data.value?.error_categories.map(c => c.category) ?? [],
  datasets: [{
    label: 'Count',
    data: data.value?.error_categories.map(c => c.count) ?? [],
    backgroundColor: '#ef4444'
  }]
}))

const totalDisplayAnomalies = computed(() =>
  (data.value?.display_anomalies?.trend ?? []).reduce((sum, d) => sum + d.count, 0)
)

const displayTrendChartData = computed(() => ({
  labels: data.value?.display_anomalies?.trend.map(d => d.date) ?? [],
  datasets: [{
    label: 'Anomalies',
    data: data.value?.display_anomalies?.trend.map(d => d.count) ?? [],
    borderColor: '#a855f7',
    backgroundColor: 'rgba(168, 85, 247, 0.1)',
    fill: true,
    tension: 0.3
  }]
}))

const displayCodeChartData = computed(() => ({
  labels: data.value?.display_anomalies?.by_code.map(c => c.code) ?? [],
  datasets: [{
    label: 'Count',
    data: data.value?.display_anomalies?.by_code.map(c => c.count) ?? [],
    backgroundColor: '#a855f7'
  }]
}))

const displayVersionChartData = computed(() => {
  const sorted = [...(data.value?.display_anomalies?.by_version ?? [])]
    .sort((a, b) => compareVersions(a.version, b.version))
  return {
    labels: sorted.map(v => v.version),
    datasets: [{
      label: 'Count',
      data: sorted.map(v => v.count),
      backgroundColor: '#8b5cf6'
    }]
  }
})

function truncateContext(ctx: string): string {
  if (!ctx) return '—'
  // Show the descriptive part before the backtrace
  const btIdx = ctx.indexOf(' | bt=')
  const display = btIdx >= 0 ? ctx.slice(0, btIdx) : ctx
  return display.length > 80 ? display.slice(0, 77) + '...' : display
}

async function fetchData() {
  loading.value = true
  error.value = ''
  try {
    data.value = await api.getStability(filters.queryString)
  } catch (e) {
    error.value = e instanceof Error ? e.message : 'Failed to load data'
  } finally {
    loading.value = false
  }
}

watch(() => filters.queryString, fetchData, { immediate: true })
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

.metrics-row {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  gap: 16px;
  margin-bottom: 24px;
}

.grid-2col {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 16px;
  margin-bottom: 24px;
  align-items: start;
}

.chart-section {
  margin-bottom: 24px;
}

.chart-section h3 {
  font-size: 14px;
  font-weight: 500;
  color: var(--text-secondary);
  margin-bottom: 12px;
}

.loading, .error, .empty-state {
  padding: 40px;
  text-align: center;
  color: var(--text-secondary);
}

.error {
  color: var(--accent-red);
}

.empty-state {
  background: var(--bg-card);
  border: 1px solid var(--border);
  border-radius: 8px;
}

.table-wrapper {
  background: var(--bg-card);
  border: 1px solid var(--border);
  border-radius: 8px;
  overflow-x: auto;
}

.data-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 13px;
}

.data-table th {
  text-align: left;
  padding: 10px 14px;
  font-weight: 500;
  color: var(--text-secondary);
  border-bottom: 1px solid var(--border);
  white-space: nowrap;
}

.data-table td {
  padding: 8px 14px;
  border-bottom: 1px solid var(--border);
  color: var(--text-primary);
  white-space: nowrap;
}

.data-table tbody tr:last-child td {
  border-bottom: none;
}

.data-table tbody tr:hover {
  background: rgba(255, 255, 255, 0.03);
}

.crash-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 13px;
}

.crash-table th {
  text-align: left;
  padding: 10px 14px;
  font-weight: 500;
  color: var(--text-secondary);
  border-bottom: 1px solid var(--border);
  white-space: nowrap;
}

.crash-table td {
  padding: 8px 14px;
  border-bottom: 1px solid var(--border);
  color: var(--text-primary);
  white-space: nowrap;
}

.crash-table tbody tr:last-child td {
  border-bottom: none;
}

.crash-table tbody tr:hover {
  background: rgba(255, 255, 255, 0.03);
}

.mono {
  font-family: 'SF Mono', 'Fira Code', monospace;
  font-size: 12px;
}

.device-id {
  color: var(--text-secondary);
}

.badge {
  display: inline-block;
  padding: 2px 8px;
  border-radius: 4px;
  font-size: 12px;
  font-weight: 500;
}

.badge.version {
  background: rgba(59, 130, 246, 0.15);
  color: #60a5fa;
}

.badge.category {
  background: rgba(139, 92, 246, 0.15);
  color: #a78bfa;
}

.badge.signal-segv {
  background: rgba(239, 68, 68, 0.15);
  color: #f87171;
}

.badge.signal-abrt {
  background: rgba(245, 158, 11, 0.15);
  color: #fbbf24;
}

.badge.signal-other {
  background: rgba(139, 92, 246, 0.15);
  color: #a78bfa;
}

.badge.occurrences {
  background: rgba(245, 158, 11, 0.15);
  color: #fbbf24;
}

.badge.anomaly-code {
  background: rgba(168, 85, 247, 0.15);
  color: #c084fc;
}

.section-divider {
  margin-top: 40px;
  margin-bottom: 24px;
  padding-top: 24px;
  border-top: 1px solid var(--border);
}

.section-divider h2 {
  font-size: 20px;
  font-weight: 600;
  margin-bottom: 4px;
}

.section-subtitle {
  font-size: 13px;
  color: var(--text-secondary);
}

.context-cell {
  max-width: 300px;
  overflow: hidden;
  text-overflow: ellipsis;
  font-size: 11px;
  color: var(--text-secondary);
}
</style>
