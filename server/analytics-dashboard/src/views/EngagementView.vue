<template>
  <AppLayout>
    <div class="page">
      <div class="page-header">
        <h2>Engagement</h2>
      </div>

      <div v-if="loading" class="loading">Loading...</div>
      <div v-else-if="error" class="error">{{ error }}</div>
      <template v-else-if="data">
        <div class="metrics-row">
          <MetricCard
            title="Median Brightness"
            :value="`${Math.round(data.brightness.p50)}%`"
            color="var(--accent-yellow)"
          />
          <MetricCard
            title="Avg Session"
            :value="data.avg_session_sec === null ? '\u2014' : formatDuration(data.avg_session_sec)"
            :subtitle="data.avg_session_sec === null ? waitingForNewClient(data.delta_era_devices) : 'per boot'"
            color="var(--accent-blue)"
          />
        </div>

        <div class="chart-section">
          <h3>Time per Panel</h3>
          <DeltaEraWaiting v-if="data.delta_era_devices === 0" :devices="data.delta_era_devices" />
          <BarChart v-else :data="panelTimeChartData" :options="horizontalBarOpts" />
        </div>

        <div class="chart-section">
          <h3>Visit Count per Panel</h3>
          <DeltaEraWaiting v-if="data.delta_era_devices === 0" :devices="data.delta_era_devices" />
          <BarChart v-else :data="panelVisitsChartData" :options="horizontalBarOpts" />
        </div>

        <div class="chart-section" v-if="data.delta_era_devices === 0 || overlayVisitsChartData.labels.length">
          <h3>Visit Count per Overlay</h3>
          <DeltaEraWaiting v-if="data.delta_era_devices === 0" :devices="data.delta_era_devices" />
          <BarChart v-else :data="overlayVisitsChartData" :options="horizontalBarOpts" />
        </div>

        <div class="chart-section">
          <h3>Avg Session Duration Over Time</h3>
          <DeltaEraWaiting v-if="data.delta_era_devices === 0" :devices="data.delta_era_devices" />
          <LineChart v-else :data="sessionDurationChartData" />
        </div>

        <div class="grid-2col">
          <div class="chart-section">
            <h3>Theme Distribution</h3>
            <BarChart :data="themeChartData" :options="horizontalBarOpts" />
          </div>
          <div class="chart-section">
            <h3>Dark vs Light Mode</h3>
            <PieChart :data="darkLightChartData" />
          </div>
        </div>

        <div class="chart-section">
          <h3>Widget Adoption (devices with widget enabled)</h3>
          <BarChart :data="widgetPlacementChartData" :options="verticalWidgetBarOpts" />
        </div>

        <div class="chart-section">
          <h3>Widget Interactions</h3>
          <DeltaEraWaiting v-if="data.delta_era_devices === 0" :devices="data.delta_era_devices" />
          <BarChart v-else :data="widgetInteractionChartData" :options="verticalWidgetBarOpts" />
        </div>

        <div class="grid-2col">
          <div class="chart-section">
            <h3>Locale Breakdown</h3>
            <PieChart :data="localeChartData" />
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
import type { EngagementData } from '@/services/api'
import type { ChartOptions } from 'chart.js'
import { horizontalBarOpts } from '@/utils/chart'
import { formatDuration, waitingForNewClient } from '@/utils/format'

const COLORS = ['#3b82f6', '#10b981', '#f59e0b', '#ef4444', '#8b5cf6', '#ec4899', '#06b6d4', '#84cc16']

const filters = useFiltersStore()
const data = ref<EngagementData | null>(null)
const loading = ref(true)
const error = ref('')

const panelTimeChartData = computed(() => {
  const sorted = [...(data.value?.panel_time ?? [])].sort((a, b) => b.total_time_sec - a.total_time_sec)
  return {
    labels: sorted.map(p => `${p.panel} (${formatDuration(p.total_time_sec)})`),
    datasets: [{
      label: 'Seconds',
      data: sorted.map(p => p.total_time_sec),
      backgroundColor: '#3b82f6'
    }]
  }
})

const panelVisitsChartData = computed(() => ({
  labels: data.value?.panel_visits.map(p => p.panel) ?? [],
  datasets: [{
    label: 'Visits',
    data: data.value?.panel_visits.map(p => p.total_visits) ?? [],
    backgroundColor: '#10b981'
  }]
}))

const overlayVisitsChartData = computed(() => ({
  labels: data.value?.overlay_visits?.map(o => o.overlay) ?? [],
  datasets: [{
    label: 'Visits',
    data: data.value?.overlay_visits?.map(o => o.total_visits) ?? [],
    backgroundColor: '#f59e0b'
  }]
}))

const sessionDurationChartData = computed(() => ({
  labels: data.value?.session_duration_trend.map(d => d.date) ?? [],
  datasets: [{
    label: 'Avg Duration (s)',
    data: data.value?.session_duration_trend.map(d => d.avg_session_sec) ?? [],
    borderColor: '#8b5cf6',
    backgroundColor: 'rgba(139, 92, 246, 0.1)',
    fill: true,
    tension: 0.3
  }]
}))

const themeChartData = computed(() => {
  const filtered = (data.value?.themes ?? []).filter(t => t.name !== 'dark' && t.name !== 'light')
  return {
    labels: filtered.map(t => t.name),
    datasets: [{
      label: 'Users',
      data: filtered.map(t => t.count),
      backgroundColor: '#8b5cf6'
    }]
  }
})

const verticalWidgetBarOpts: ChartOptions<'bar'> = {
  scales: { x: { ticks: { autoSkip: false, maxRotation: 45, minRotation: 45 } } }
}

function formatWidgetName(id: string): string {
  return id.split('_').map(w => w.charAt(0).toUpperCase() + w.slice(1)).join(' ')
}

const widgetPlacementChartData = computed(() => ({
  labels: data.value?.widget_placement?.map(w => formatWidgetName(w.widget)) ?? [],
  datasets: [{
    label: 'Devices',
    data: data.value?.widget_placement?.map(w => w.devices) ?? [],
    backgroundColor: '#10b981'
  }]
}))

const widgetInteractionChartData = computed(() => ({
  labels: data.value?.widget_interactions?.map(w => formatWidgetName(w.widget)) ?? [],
  datasets: [{
    label: 'Interactions',
    data: data.value?.widget_interactions?.map(w => w.interactions) ?? [],
    backgroundColor: '#8b5cf6'
  }]
}))

const DARK_LIGHT_COLORS: Record<string, string> = { dark: '#1e293b', light: '#f8fafc' }

const darkLightChartData = computed(() => ({
  labels: data.value?.dark_vs_light.map(d => d.name.charAt(0).toUpperCase() + d.name.slice(1)) ?? [],
  datasets: [{
    data: data.value?.dark_vs_light.map(d => d.count) ?? [],
    backgroundColor: data.value?.dark_vs_light.map(d => DARK_LIGHT_COLORS[d.name] ?? '#64748b') ?? [],
    borderColor: '#334155',
    borderWidth: 1
  }]
}))

const localeChartData = computed(() => ({
  labels: data.value?.locales.map(l => l.name) ?? [],
  datasets: [{
    data: data.value?.locales.map(l => l.count) ?? [],
    backgroundColor: COLORS
  }]
}))

async function fetchData() {
  loading.value = true
  error.value = ''
  try {
    data.value = await api.getEngagement(filters.queryString)
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

.loading, .error {
  padding: 40px;
  text-align: center;
  color: var(--text-secondary);
}

.error {
  color: var(--accent-red);
}
</style>
