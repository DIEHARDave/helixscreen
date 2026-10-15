<template>
  <div class="chart-section">
    <h3>{{ title }}</h3>
    <ChartCaption :text="caption" />
    <NoData v-if="rows.length === 0" :era="era" />
    <BarChart
      v-else
      :data="chartData"
      :options="options"
      :height="barChartHeight(rows.length)"
      @select="onSelect"
    />
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useRouter } from 'vue-router'
import type { ChartOptions } from 'chart.js'
import BarChart from '@/components/BarChart.vue'
import ChartCaption from '@/components/ChartCaption.vue'
import NoData from '@/components/NoData.vue'
import type { DeltaEra } from '@/services/api'
import { useFiltersStore } from '@/stores/filters'
import { horizontalBarOpts } from '@/utils/chart'
import { isFilterable } from '@/utils/models'
import { barChartHeight, seriesColor } from '@/utils/series'
import type { DashFilterKey } from '@/utils/urlState'
import type { Grouped } from '@/utils/versions'

const props = withDefaults(
  defineProps<{
    rows: Grouped[]
    filterKey: DashFilterKey
    caption: string
    title: string
    /** Dataset label shown in the tooltip. */
    valueLabel?: string
    era?: DeltaEra | null
  }>(),
  { valueLabel: 'Devices', era: null }
)

const filters = useFiltersStore()
const router = useRouter()

const filterable = (row: Grouped) => row.members.every(isFilterable)

const chartData = computed(() => ({
  labels: props.rows.map((r) => (r.expandable ? `${r.label} ▸` : r.label)),
  datasets: [{
    label: props.valueLabel,
    data: props.rows.map((r) => r.count),
    backgroundColor: props.rows.map((_, i) => seriesColor(i))
  }]
}))

const options = computed((): ChartOptions<'bar'> => ({
  ...horizontalBarOpts,
  plugins: {
    legend: { display: false },
    tooltip: {
      callbacks: {
        afterLabel: (ctx) => {
          const row = props.rows[ctx.dataIndex]
          if (!row) return ''
          if (row.expandable) return 'click to expand'
          return filterable(row) ? 'click to filter' : 'not filterable'
        }
      }
    }
  },
  onHover: (evt, elements) => {
    const target = evt.native?.target
    if (target instanceof HTMLElement) target.style.cursor = elements.length ? 'pointer' : 'default'
  }
}))

function onSelect(index: number) {
  const row = props.rows[index]
  if (!row) return
  if (row.expandable) filters.toggleExpand(row.key)
  else if (filterable(row)) filters.drill(router, props.filterKey, row.members)
}
</script>
