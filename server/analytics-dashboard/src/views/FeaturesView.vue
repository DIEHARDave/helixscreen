<template>
  <AppLayout>
    <div class="page">
      <div class="page-header">
        <h2>Feature Adoption</h2>
      </div>

      <div v-if="loading" class="loading">Loading...</div>
      <div v-else-if="error" class="error">{{ error }}</div>
      <DeltaEraWaiting v-else-if="data && data.delta_era_devices === 0" :devices="data.delta_era_devices" />
      <template v-else-if="data">
        <div class="metrics-row">
          <MetricCard
            title="Devices on New Client"
            :value="data.delta_era_devices.toLocaleString()"
            subtitle="the adoption denominator"
            color="var(--accent-yellow)"
          />
          <MetricCard
            title="Features Used"
            :value="String(data.features.length)"
            color="var(--accent-blue)"
          />
          <MetricCard
            title="Most Used"
            :value="mostUsed"
            color="var(--accent-green)"
          />
          <MetricCard
            title="Least Used"
            :value="leastUsed"
            color="var(--accent-red)"
          />
        </div>

        <div class="chart-section">
          <h3>Overlay and Widget Adoption (% of devices that used each at least once)</h3>
          <BarChart :data="adoptionChartData" :options="horizontalBarOpts" />
        </div>

        <div class="chart-section">
          <h3>Least Adopted</h3>
          <table class="data-table">
            <thead>
              <tr><th>Feature</th><th>Adoption %</th><th>Devices Using</th></tr>
            </thead>
            <tbody>
              <tr v-for="f in leastAdopted" :key="f.id">
                <td>{{ titleCase(f.id) }}</td>
                <td>{{ (f.adoption_rate * 100).toFixed(1) }}%</td>
                <td>{{ f.devices }}</td>
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
import { useFiltersStore } from '@/stores/filters'
import { api } from '@/services/api'
import type { FeaturesData } from '@/services/api'
import { horizontalBarOpts } from '@/utils/chart'
import { titleCase } from '@/utils/format'

const filters = useFiltersStore()
const data = ref<FeaturesData | null>(null)
const loading = ref(true)
const error = ref('')

const mostUsed = computed(() => {
  const f = data.value?.features[0]
  return f ? `${titleCase(f.id)} (${(f.adoption_rate * 100).toFixed(0)}%)` : 'N/A'
})

const leastUsed = computed(() => {
  const features = data.value?.features ?? []
  const f = features[features.length - 1]
  return f ? `${titleCase(f.id)} (${(f.adoption_rate * 100).toFixed(0)}%)` : 'N/A'
})

const adoptionChartData = computed(() => {
  const sorted = [...(data.value?.features ?? [])].sort((a, b) => b.adoption_rate - a.adoption_rate)
  return {
    labels: sorted.map(f => titleCase(f.id)),
    datasets: [{
      label: 'Adoption %',
      data: sorted.map(f => +(f.adoption_rate * 100).toFixed(1)),
      backgroundColor: '#3b82f6',
    }],
  }
})

// Only ids some device used are reported, so this is the tail of what was used.
const leastAdopted = computed(() =>
  [...(data.value?.features ?? [])]
    .sort((a, b) => a.adoption_rate - b.adoption_rate)
    .slice(0, 10)
)

async function fetchData() {
  loading.value = true
  error.value = ''
  try {
    data.value = await api.getFeatures(filters.queryString)
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
.chart-section { margin-bottom: 24px; }
.chart-section h3 { font-size: 14px; font-weight: 500; color: var(--text-secondary); margin-bottom: 12px; }
.data-table { width: 100%; border-collapse: collapse; }
.data-table th, .data-table td { padding: 8px 12px; text-align: left; border-bottom: 1px solid var(--border); }
.data-table th { font-size: 12px; color: var(--text-secondary); font-weight: 500; }
.loading, .error { padding: 40px; text-align: center; color: var(--text-secondary); }
.error { color: var(--accent-red); }
</style>
