<template>
  <AppLayout>
    <div class="page">
      <div class="page-header">
        <h2>UX Insights</h2>
      </div>

      <div v-if="loading" class="loading">Loading...</div>
      <div v-else-if="error" class="error">{{ error }}</div>
      <template v-else-if="data">
        <div class="metrics-row">
          <MetricCard
            title="Avg Session Duration"
            :value="data.avg_session_sec === null ? '\u2014' : formatDuration(data.avg_session_sec)"
            :subtitle="data.avg_session_sec === null ? deltaEraPlaceholder(data) : undefined"
            color="var(--accent-blue)"
          />
          <MetricCard
            title="Most Visited Panel"
            :value="data.most_visited_panel || 'N/A'"
            color="var(--accent-green)"
          />
          <MetricCard
            title="Least Visited Panel"
            :value="data.least_visited_panel || 'N/A'"
            color="var(--accent-red)"
          />
          <MetricCard
            title="Settings Changes/Device/Week"
            :value="data.settings_change_rate_per_device_per_week.toFixed(1)"
            color="var(--accent-yellow)"
          />
        </div>

        <div class="grid-2col">
          <div class="chart-section">
            <h3>Time per Panel</h3>
            <DeltaEraWaiting v-if="data.delta_era_devices === 0" :era="data" />
            <PieChart v-else :data="panelTimePieData" />
          </div>
          <div class="chart-section">
            <h3>Visits per Panel (per device)</h3>
            <DeltaEraWaiting v-if="data.delta_era_devices === 0" :era="data" />
            <BarChart v-else :data="panelVisitsBarData" :options="horizontalBarOpts" />
          </div>
        </div>

        <div class="chart-section">
          <h3>Most Changed Settings</h3>
          <BarChart :data="settingsChangesData" :options="horizontalBarOpts" />
        </div>

        <div class="chart-section">
          <h3>Settings Changed from Default (% of fleet)</h3>
          <table class="data-table">
            <thead>
              <tr><th>Setting</th><th>% Changed</th><th>Devices</th></tr>
            </thead>
            <tbody>
              <tr v-for="s in data.settings_defaults" :key="s.setting">
                <td>{{ titleCase(s.setting) }}</td>
                <td>{{ (s.pct_changed * 100).toFixed(1) }}%</td>
                <td>{{ s.devices_changed }}</td>
              </tr>
            </tbody>
          </table>
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
import BarChart from '@/components/BarChart.vue'
import PieChart from '@/components/PieChart.vue'
import { useFiltersStore } from '@/stores/filters'
import { api } from '@/services/api'
import type { UxInsightsData } from '@/services/api'
import { horizontalBarOpts } from '@/utils/chart'
import { formatDuration, titleCase, deltaEraPlaceholder } from '@/utils/format'

const COLORS = ['#3b82f6', '#10b981', '#f59e0b', '#ef4444', '#8b5cf6', '#ec4899', '#06b6d4', '#84cc16']

const filters = useFiltersStore()
const data = ref<UxInsightsData | null>(null)
const loading = ref(true)
const error = ref('')

const panelTimePieData = computed(() => ({
  labels: data.value?.panel_time.map(p => p.panel) ?? [],
  datasets: [{
    data: data.value?.panel_time.map(p => p.total_time_sec) ?? [],
    backgroundColor: COLORS,
  }],
}))

const panelVisitsBarData = computed(() => {
  const sorted = [...(data.value?.panel_visits ?? [])].sort((a, b) => b.visits_per_device - a.visits_per_device)
  return {
    labels: sorted.map(p => p.panel),
    datasets: [{
      label: 'Visits/Device',
      data: sorted.map(p => +p.visits_per_device.toFixed(1)),
      backgroundColor: '#10b981',
    }],
  }
})

const settingsChangesData = computed(() => {
  const sorted = [...(data.value?.settings_changes ?? [])].sort((a, b) => b.change_count - a.change_count)
  return {
    labels: sorted.map(s => titleCase(s.setting)),
    datasets: [{
      label: 'Changes',
      data: sorted.map(s => s.change_count),
      backgroundColor: '#8b5cf6',
    }],
  }
})

async function fetchData() {
  loading.value = true
  error.value = ''
  try {
    data.value = await api.getUxInsights(filters.queryString)
  } catch (e) {
    error.value = e instanceof Error ? e.message : 'Failed to load data'
  } finally {
    loading.value = false
  }
}

watch(() => filters.queryString, fetchData, { immediate: true })
</script>

<style scoped>
.page-header { display: flex; align-items: center; justify-content: space-between; margin-bottom: 24px; }
.page-header h2 { font-size: 20px; font-weight: 600; }
.metrics-row { display: grid; grid-template-columns: repeat(4, 1fr); gap: 16px; margin-bottom: 24px; }
.grid-2col { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; margin-bottom: 24px; }
.chart-section { margin-bottom: 24px; }
.chart-section h3 { font-size: 14px; font-weight: 500; color: var(--text-secondary); margin-bottom: 12px; }
.data-table { width: 100%; border-collapse: collapse; }
.data-table th, .data-table td { padding: 8px 12px; text-align: left; border-bottom: 1px solid var(--border); }
.data-table th { font-size: 12px; color: var(--text-secondary); font-weight: 500; }
.loading, .error { padding: 40px; text-align: center; color: var(--text-secondary); }
.error { color: var(--accent-red); }
</style>
