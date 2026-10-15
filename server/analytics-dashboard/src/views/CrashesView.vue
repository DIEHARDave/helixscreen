<template>
  <AppLayout>
    <div class="page">
      <div class="page-header">
        <h2>Crashes</h2>
      </div>

      <div v-if="loading" class="loading">Loading...</div>
      <div v-else-if="error" class="error">{{ error }}</div>
      <template v-else-if="data">
        <!-- 1.0 READINESS HEADLINE: trailing 14-day session crash rate vs 1.5% threshold -->
        <div v-if="data.trailing_14d" class="gauge-section">
          <div class="gauge-card" :class="gaugeStatus">
            <div class="gauge-label">Trailing 14-day session crash rate</div>
            <div class="gauge-value">{{ formatRate(data.trailing_14d.rate, 2) }}</div>
            <div class="gauge-sub">
              {{ data.trailing_14d.crash_count.toLocaleString() }} crashes
              /
              {{ data.trailing_14d.session_count.toLocaleString() }} sessions
              <span class="gauge-threshold">
                · target: &lt; 1.50% for 1.0
              </span>
            </div>
          </div>
        </div>

        <!-- Per-platform breakdown: core platforms vs small-N platforms -->
        <div v-if="corePlatforms.length" class="chart-section">
          <h3>Core platforms (≥ 100 sessions)</h3>
          <div class="platform-grid">
            <div
              v-for="p in corePlatforms"
              :key="p.platform"
              class="platform-card"
              :class="platformRateClass(p.rate)"
            >
              <div class="platform-name">{{ p.platform }}</div>
              <div class="platform-rate">{{ formatRate(p.rate, 2) }}</div>
              <div class="platform-sub">
                {{ p.crash_count }} crashes / {{ p.session_count.toLocaleString() }} sessions
                <br>
                {{ p.crashing_devices }} of {{ p.session_devices }} devices crashed
              </div>
            </div>
          </div>
        </div>
        <div v-if="limitedPlatforms.length" class="chart-section">
          <h3>
            Limited-data platforms (&lt; 100 sessions)
            <span class="hint">— small N; rates may be noisy</span>
          </h3>
          <div class="platform-grid">
            <div
              v-for="p in limitedPlatforms"
              :key="p.platform"
              class="platform-card limited"
            >
              <div class="platform-name">{{ p.platform }}</div>
              <div class="platform-rate">{{ formatRate(p.rate) }}</div>
              <div class="platform-sub">
                {{ p.crash_count }} crashes / {{ p.session_count }} sessions
                <br>
                {{ p.crashing_devices }} of {{ p.session_devices }} devices crashed
              </div>
            </div>
          </div>
        </div>

        <div class="chart-section">
          <h3>Crash Rate by Version</h3>
          <BarChart :data="versionChartData" :options="barOptions" />
        </div>

        <div class="grid-2col">
          <div class="chart-section">
            <h3>Crashes by Signal</h3>
            <PieChart :data="signalChartData" />
          </div>
          <MetricCard
            title="Average Uptime"
            :value="formatDuration(data.avg_uptime_sec)"
            subtitle="before crash"
            color="var(--accent-yellow)"
          />
        </div>

        <div class="chart-section">
          <h3>Recent Crashes</h3>
          <div v-if="crashListLoading" class="loading">Loading crash list...</div>
          <div v-else-if="crashList.length === 0" class="empty-state">No crash events found in this period.</div>
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
                <tr v-for="(crash, i) in crashList" :key="i">
                  <td class="mono">{{ formatTimestamp(crash.timestamp) }}</td>
                  <td><span class="badge version">{{ crash.version || '—' }}</span></td>
                  <td><span class="badge" :class="signalClass(crash.signal)">{{ crash.signal || '—' }}</span></td>
                  <td>{{ crash.platform || '—' }}</td>
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
import BarChart from '@/components/BarChart.vue'
import PieChart from '@/components/PieChart.vue'
import MetricCard from '@/components/MetricCard.vue'
import { useFiltersStore } from '@/stores/filters'
import { api } from '@/services/api'
import type { CrashesData, CrashListData } from '@/services/api'
import type { ChartOptions } from 'chart.js'
import { formatDuration, formatRate, formatTimestamp, shortDeviceId } from '@/utils/format'

const COLORS = ['#3b82f6', '#10b981', '#f59e0b', '#ef4444', '#8b5cf6', '#ec4899', '#06b6d4', '#84cc16']

const filters = useFiltersStore()
const data = ref<CrashesData | null>(null)
const crashList = ref<CrashListData['crashes']>([])
const loading = ref(true)
const crashListLoading = ref(true)
const error = ref('')

function signalClass(signal: string): string {
  if (signal === 'SIGSEGV') return 'signal-segv'
  if (signal === 'SIGABRT') return 'signal-abrt'
  return 'signal-other'
}

const barOptions: ChartOptions<'bar'> = {
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

const versionChartData = computed(() => ({
  labels: data.value?.by_version.map(v => v.version) ?? [],
  datasets: [{
    label: 'Crash Rate %',
    data: data.value?.by_version.map(v => (v.rate === null ? null : v.rate * 100)) ?? [],
    backgroundColor: '#ef4444'
  }]
}))

const signalChartData = computed(() => ({
  labels: data.value?.by_signal.map(s => s.signal) ?? [],
  datasets: [{
    data: data.value?.by_signal.map(s => s.count) ?? [],
    backgroundColor: COLORS
  }]
}))

// 1.0-readiness gauge threshold — see stability sprint plan 2026-04-22.
const GAUGE_GREEN = 0.015 // < 1.5% sessions → green
const GAUGE_YELLOW = 0.03 // 1.5%–3% sessions → yellow, above → red

const gaugeStatus = computed(() => {
  const r = data.value?.trailing_14d?.rate ?? null
  if (r === null) return ''
  if (r < GAUGE_GREEN) return 'gauge-green'
  if (r < GAUGE_YELLOW) return 'gauge-yellow'
  return 'gauge-red'
})

function platformRateClass(rate: number | null): string {
  if (rate === null) return ''
  if (rate < GAUGE_GREEN) return 'rate-green'
  if (rate < GAUGE_YELLOW) return 'rate-yellow'
  return 'rate-red'
}

// Split platforms by sample size so small-N data (CC1, Snapmaker, AD5M early on)
// doesn't visually dominate the core fleet's rate.
const SMALL_N_THRESHOLD = 100
const corePlatforms = computed(() =>
  (data.value?.by_platform ?? []).filter(p => p.session_count >= SMALL_N_THRESHOLD),
)
const limitedPlatforms = computed(() =>
  (data.value?.by_platform ?? []).filter(p => p.session_count < SMALL_N_THRESHOLD),
)

async function fetchData() {
  loading.value = true
  crashListLoading.value = true
  error.value = ''
  try {
    const [crashesData, listData] = await Promise.all([
      api.getCrashes(filters.queryString),
      api.getCrashList(filters.queryString),
    ])
    data.value = crashesData
    crashList.value = listData.crashes
  } catch (e) {
    error.value = e instanceof Error ? e.message : 'Failed to load data'
  } finally {
    loading.value = false
    crashListLoading.value = false
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

/* 1.0-readiness headline gauge */
.gauge-section {
  margin-bottom: 24px;
}

.gauge-card {
  padding: 20px 24px;
  border: 1px solid var(--border);
  border-left-width: 6px;
  border-radius: 8px;
  background: var(--bg-card);
}

.gauge-card.gauge-green {
  border-left-color: #10b981;
}

.gauge-card.gauge-yellow {
  border-left-color: #f59e0b;
}

.gauge-card.gauge-red {
  border-left-color: #ef4444;
}

.gauge-label {
  font-size: 13px;
  color: var(--text-secondary);
  font-weight: 500;
}

.gauge-value {
  font-size: 42px;
  font-weight: 700;
  margin: 4px 0 8px;
  font-family: 'SF Mono', 'Fira Code', monospace;
}

.gauge-card.gauge-green .gauge-value {
  color: #10b981;
}

.gauge-card.gauge-yellow .gauge-value {
  color: #f59e0b;
}

.gauge-card.gauge-red .gauge-value {
  color: #ef4444;
}

.gauge-sub {
  font-size: 12px;
  color: var(--text-secondary);
}

.gauge-threshold {
  margin-left: 6px;
  font-style: italic;
}

/* Per-platform breakdown */
.platform-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(200px, 1fr));
  gap: 12px;
}

.platform-card {
  padding: 14px 16px;
  background: var(--bg-card);
  border: 1px solid var(--border);
  border-left-width: 4px;
  border-radius: 6px;
}

.platform-card.rate-green { border-left-color: #10b981; }
.platform-card.rate-yellow { border-left-color: #f59e0b; }
.platform-card.rate-red { border-left-color: #ef4444; }

.platform-card.limited {
  border-left-color: #64748b;
  opacity: 0.85;
}

.platform-name {
  font-size: 13px;
  font-weight: 600;
  color: var(--text-primary);
  text-transform: uppercase;
  letter-spacing: 0.04em;
}

.platform-rate {
  font-size: 24px;
  font-weight: 700;
  margin: 2px 0 6px;
  font-family: 'SF Mono', 'Fira Code', monospace;
}

.platform-card.rate-green .platform-rate { color: #10b981; }
.platform-card.rate-yellow .platform-rate { color: #f59e0b; }
.platform-card.rate-red .platform-rate { color: #ef4444; }
.platform-card.limited .platform-rate { color: var(--text-secondary); }

.platform-sub {
  font-size: 11px;
  color: var(--text-secondary);
  line-height: 1.4;
}

.chart-section h3 .hint {
  font-weight: 400;
  font-size: 12px;
  color: var(--text-secondary);
  margin-left: 8px;
}
</style>
