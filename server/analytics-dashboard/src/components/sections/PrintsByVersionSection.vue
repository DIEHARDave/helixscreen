<template>
  <DashSection title="Success by version" :loading="loading" :error="error">
    <template v-if="data">
      <ChartCaption :text="`Sample-weighted print outcomes per HelixScreen version, ${span}; click a bar to filter`" />
      <NoData v-if="rows.length === 0" />
      <BarChart
        v-else
        :data="chartData"
        :options="successBarOpts(rows)"
        :height="barChartHeight(rows.length)"
        @select="(i) => drillRow(rows[i])"
      />
    </template>
  </DashSection>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useRouter } from 'vue-router'
import DashSection from '@/components/DashSection.vue'
import BarChart from '@/components/BarChart.vue'
import ChartCaption from '@/components/ChartCaption.vue'
import NoData from '@/components/NoData.vue'
import { useDashData } from '@/composables/useDashData'
import { api } from '@/services/api'
import { compareVersions, successBarOpts, type SuccessRow } from '@/utils/chart'
import { isFilterable } from '@/utils/models'
import { barChartHeight } from '@/utils/series'

const { data, loading, error, span, filters } = useDashData(api.getPrints)
const router = useRouter()

const rows = computed((): SuccessRow[] =>
  [...(data.value?.by_version ?? [])]
    .sort((a, b) => compareVersions(b.version, a.version))
    .map((v) => ({ label: v.version, members: [v.version], total: v.total, rate: v.rate }))
)

const chartData = computed(() => ({
  labels: rows.value.map((r) => `${r.label} (${r.total.toLocaleString()})`),
  datasets: [{ label: 'Success rate', data: rows.value.map((r) => r.rate), backgroundColor: '#3b82f6' }]
}))

function drillRow(row: SuccessRow | undefined) {
  if (row && row.members.every(isFilterable)) filters.drill(router, 'version', row.members)
}
</script>
