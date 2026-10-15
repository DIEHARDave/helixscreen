<template>
  <AppLayout>
    <div class="page">
      <div class="page-header">
        <h2>Devices</h2>
      </div>

      <DashSection title="Devices" :loading="loading" :error="error">
        <template v-if="data">
          <ChartCaption :text="caption" />
          <NoData v-if="data.devices.length === 0" />
          <div v-else class="table-wrapper">
            <table class="data-table">
              <thead>
                <tr>
                  <th>Device</th>
                  <th>Last seen</th>
                  <th>Version</th>
                  <th>Platform</th>
                  <th>Board</th>
                  <th>Model</th>
                  <th>Sessions</th>
                  <th>Crashes</th>
                  <th>Prints</th>
                </tr>
              </thead>
              <tbody>
                <tr
                  v-for="d in data.devices"
                  :key="d.device_id"
                  class="clickable-row"
                  @click="open(d.device_id)"
                >
                  <td class="mono">{{ shortDeviceId(d.device_id) }}</td>
                  <td>{{ formatTimestamp(d.last_seen) }}</td>
                  <td><span class="badge version">{{ d.version || '-' }}</span></td>
                  <td>{{ d.platform || '-' }}</td>
                  <td>{{ d.board || '-' }}</td>
                  <td>{{ d.model || '-' }}</td>
                  <td class="mono">{{ d.sessions.toLocaleString() }}</td>
                  <td class="mono" :title="countTitle('crash_count')">{{ count(d.crashes, 'crash_count') }}</td>
                  <td class="mono" :title="countTitle('print_count')">{{ count(d.prints, 'print_count') }}</td>
                </tr>
              </tbody>
            </table>
          </div>
        </template>
      </DashSection>
    </div>
  </AppLayout>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useRouter } from 'vue-router'
import { api } from '@/services/api'
import { useDashData } from '@/composables/useDashData'
import { formatTimestamp, shortDeviceId } from '@/utils/format'
import AppLayout from '@/components/AppLayout.vue'
import ChartCaption from '@/components/ChartCaption.vue'
import DashSection from '@/components/DashSection.vue'
import NoData from '@/components/NoData.vue'

const LIST_LIMIT = 200

// Filter keys that the crash and print rows do not carry; a count under one of
// them is not a zero, it is unknown.
const UNREPORTED_FILTERS = {
  crash_count: ['model'],
  print_count: ['model']
} as const
type CountColumn = keyof typeof UNREPORTED_FILTERS

const router = useRouter()
const { data, loading, error, span, filters } = useDashData(api.getDevices)

const caption = computed(() => {
  const total = data.value?.total ?? 0
  const head = total > LIST_LIMIT
    ? `Showing ${LIST_LIMIT} of ${total.toLocaleString()} devices`
    : `${total.toLocaleString()} devices`
  return `${head}, active in the ${span.value}; click a row for its timeline`
})

function blockingFilter(column: CountColumn): string | null {
  return UNREPORTED_FILTERS[column].find((key) => filters[key].length > 0) ?? null
}

function count(value: number, column: CountColumn): string {
  return blockingFilter(column) ? 'n/a' : value.toLocaleString()
}

function countTitle(column: CountColumn): string | undefined {
  const key = blockingFilter(column)
  return key ? `not reported per ${key}` : undefined
}

function open(id: string) {
  router.push({ path: `/device/${id}`, query: filters.queryParams })
}
</script>
