<template>
  <DashSection title="Success by material set" :loading="loading" :error="error">
    <template v-if="data">
      <div class="metrics-row">
        <MetricCard
          title="Multi-material Share"
          :value="formatRate(multiShare)"
          :subtitle="`prints with two or more materials, ${span}`"
          color="var(--accent-purple)"
        />
      </div>

      <div class="chart-section">
        <h3>Success by Material Set</h3>
        <ChartCaption :text="`Sample-weighted print outcomes per set of distinct materials loaded, ${span}`" />
        <NoData v-if="rows.length === 0" />
        <BarChart
          v-else
          :data="chartData"
          :options="successBarOpts(rows)"
          :height="barChartHeight(rows.length)"
        />
      </div>
    </template>
  </DashSection>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import DashSection from '@/components/DashSection.vue'
import BarChart from '@/components/BarChart.vue'
import ChartCaption from '@/components/ChartCaption.vue'
import MetricCard from '@/components/MetricCard.vue'
import NoData from '@/components/NoData.vue'
import { useDashData } from '@/composables/useDashData'
import { api } from '@/services/api'
import { successBarOpts, type SuccessRow } from '@/utils/chart'
import { formatRate } from '@/utils/format'
import { groupMaterialSets } from '@/utils/materials'
import { barChartHeight, seriesColor } from '@/utils/series'

const { data, loading, error, span } = useDashData(api.getPrints)

const sets = computed(() => groupMaterialSets(data.value?.by_filament ?? []))

const rows = computed((): SuccessRow[] =>
  sets.value.map((s) => ({ label: s.label, members: s.members, total: s.count, rate: s.success_rate }))
)

const multiShare = computed((): number | null => {
  const total = sets.value.reduce((sum, s) => sum + s.count, 0)
  if (total === 0) return null
  return sets.value.filter((s) => s.multi).reduce((sum, s) => sum + s.count, 0) / total
})

const chartData = computed(() => ({
  labels: rows.value.map((r) => `${r.label} (${r.total.toLocaleString()})`),
  datasets: [{
    label: 'Success rate',
    data: rows.value.map((r) => r.rate),
    backgroundColor: rows.value.map((_, i) => seriesColor(i))
  }]
}))
</script>
