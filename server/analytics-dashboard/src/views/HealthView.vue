<template>
  <AppLayout>
    <div class="page">
      <div class="page-header">
        <h2>Health</h2>
      </div>

      <DashSection title="Fleet" :loading="loading" :error="error">
        <template v-if="data">
          <div class="metrics-row">
            <MetricCard
              title="Opt-in actives"
              :value="data.fleet.actives_cur.toLocaleString()"
              :subtitle="activesDelta"
              color="#3b82f6"
            />
            <MetricCard
              title="CDN sources"
              :value="data.fleet.cdn_sources === null ? 'n/a' : data.fleet.cdn_sources.toLocaleString()"
              :subtitle="data.fleet.cdn_date ?? ''"
              color="#10b981"
            />
            <MetricCard
              title="New error codes"
              :value="newErrors.length"
              subtitle="not seen in the previous weeks"
              :color="newErrors.length > 0 ? '#ef4444' : '#10b981'"
            />
            <MetricCard
              title="Low-data kinds"
              :value="lowKinds.length"
              subtitle="under half their usual volume"
              :color="lowKinds.length > 0 ? '#f59e0b' : '#10b981'"
            />
          </div>
          <ChartCaption :text="WINDOW" />
        </template>
      </DashSection>

      <DashSection title="Releases" :loading="loading" :error="error">
        <template v-if="data">
          <ChartCaption :text="`${WINDOW}; arrows show the change against the earlier week, click a version for its release page`" />
          <NoData v-if="data.releases.length === 0" />
          <div v-else class="table-wrapper">
            <table class="data-table">
              <thead>
                <tr>
                  <th>Version</th>
                  <th>Devices</th>
                  <th>Crash-free</th>
                  <th>Crash rate</th>
                  <th>Update failure</th>
                  <th>Print success</th>
                </tr>
              </thead>
              <tbody>
                <tr
                  v-for="r in data.releases"
                  :key="r.version"
                  class="clickable"
                  title="click to open this release"
                  @click="filters.go(router, '/releases', [['version', single(r.version)]])"
                >
                  <td><span class="badge version">{{ r.version }}</span></td>
                  <td class="mono">{{ r.cur.devices.toLocaleString() }}</td>
                  <td v-for="m in METRICS" :key="m.key" class="mono">
                    {{ formatRate(r.cur[m.key]) }}
                    <span v-if="change(r.cur[m.key], r.prev[m.key])" class="delta" :class="changeClass(r.cur[m.key], r.prev[m.key], m.upIsGood)">
                      {{ change(r.cur[m.key], r.prev[m.key]) }}
                    </span>
                  </td>
                </tr>
              </tbody>
            </table>
          </div>
        </template>
      </DashSection>

      <DashSection title="Errors" :loading="loading" :error="error">
        <template v-if="data">
          <ChartCaption :text="`${WINDOW}; click a row to find the code on the Stability tab`" />
          <NoData v-if="data.errors.length === 0" />
          <div v-else class="table-wrapper event-log">
            <table class="data-table">
              <thead>
                <tr>
                  <th>Category</th>
                  <th>Code</th>
                  <th>This week</th>
                  <th>Before</th>
                  <th>Devices</th>
                  <th>Status</th>
                </tr>
              </thead>
              <tbody>
                <tr
                  v-for="e in data.errors"
                  :key="`${e.category}|${e.code}`"
                  class="clickable"
                  title="click to see this code on the Stability tab"
                  @click="filters.go(router, '/stability', [], `#${errorAnchor(e.code)}`)"
                >
                  <td><span class="badge category">{{ e.category }}</span></td>
                  <td class="mono">{{ e.code }}</td>
                  <td class="mono">{{ e.cur.toLocaleString() }}</td>
                  <td class="mono">{{ e.prev.toLocaleString() }}</td>
                  <td class="mono">{{ e.devices.toLocaleString() }}</td>
                  <td><span v-if="e.status !== 'steady'" class="badge" :class="`status-${e.status}`">{{ e.status }}</span></td>
                </tr>
              </tbody>
            </table>
          </div>
        </template>
      </DashSection>

      <DashSection title="Crash clusters" :loading="loading" :error="error">
        <template v-if="data">
          <ChartCaption :text="`${WINDOW}; click a row to filter on its version and board and open Stability`" />
          <NoData v-if="data.crash_clusters.length === 0" />
          <div v-else class="table-wrapper event-log">
            <table class="data-table">
              <thead>
                <tr>
                  <th>Signal</th>
                  <th>Fault code</th>
                  <th>Version</th>
                  <th>Board</th>
                  <th>This week</th>
                  <th>Before</th>
                  <th>Devices</th>
                </tr>
              </thead>
              <tbody>
                <tr
                  v-for="(c, i) in data.crash_clusters"
                  :key="i"
                  class="clickable"
                  :title="activationHint(clusterEntries(c))"
                  @click="filters.go(router, '/stability', clusterEntries(c))"
                >
                  <td>{{ c.signal || '-' }}</td>
                  <td class="mono">{{ c.fault_code || '-' }}</td>
                  <td><span class="badge version">{{ c.version || '-' }}</span></td>
                  <td>{{ c.board || '-' }}</td>
                  <td class="mono">{{ c.cur.toLocaleString() }}</td>
                  <td class="mono">{{ c.prev.toLocaleString() }}</td>
                  <td class="mono">{{ c.devices.toLocaleString() }}</td>
                </tr>
              </tbody>
            </table>
          </div>
        </template>
      </DashSection>

      <DashSection title="Data health" :loading="loading" :error="error">
        <template v-if="data">
          <ChartCaption :text="`Latest complete day against the median of the days before it`" />
          <p v-if="lowKinds.length === 0" class="health-line">All kinds within normal volume</p>
          <ul v-else class="health-line low">
            <li v-for="k in lowKinds" :key="k.kind">
              <span class="mono">{{ k.kind }}</span>: {{ k.latest.toLocaleString() }} latest vs {{ k.median.toLocaleString() }} median
            </li>
          </ul>
        </template>
      </DashSection>
    </div>
  </AppLayout>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useRouter } from 'vue-router'
import AppLayout from '@/components/AppLayout.vue'
import DashSection from '@/components/DashSection.vue'
import ChartCaption from '@/components/ChartCaption.vue'
import MetricCard from '@/components/MetricCard.vue'
import NoData from '@/components/NoData.vue'
import { useDashData } from '@/composables/useDashData'
import { api } from '@/services/api'
import type { HealthData } from '@/services/api'
import { errorAnchor } from '@/utils/anchors'
import { activationHint, single } from '@/utils/drill'
import { formatRate } from '@/utils/format'
import type { DashFilterKey } from '@/utils/urlState'
import type { Grouped } from '@/utils/versions'

const WINDOW = 'Last 7 days vs the 7 before'

type RateKey = 'crash_free' | 'crash_rate' | 'update_failure_rate' | 'print_success'
const METRICS: { key: RateKey; upIsGood: boolean }[] = [
  { key: 'crash_free', upIsGood: true },
  { key: 'crash_rate', upIsGood: false },
  { key: 'update_failure_rate', upIsGood: false },
  { key: 'print_success', upIsGood: true }
]

const { data, loading, error, filters } = useDashData(api.getHealth)
const router = useRouter()

const newErrors = computed(() => (data.value?.errors ?? []).filter((e) => e.status === 'new'))
const lowKinds = computed(() => (data.value?.data_health ?? []).filter((k) => k.low))

const activesDelta = computed(() => {
  const f = data.value?.fleet
  if (!f) return ''
  const diff = f.actives_cur - f.actives_prev
  const sign = diff > 0 ? '+' : ''
  const pct = f.actives_prev > 0 ? ` (${sign}${((diff / f.actives_prev) * 100).toFixed(1)}%)` : ''
  return `${sign}${diff.toLocaleString()}${pct} vs last week`
})

/** The signed change in percentage points with an arrow; empty when either window has no rate. */
function change(cur: number | null, prev: number | null): string {
  if (cur === null || prev === null) return ''
  const pp = (cur - prev) * 100
  if (Math.abs(pp) < 0.05) return '= 0.0pp'
  return `${pp > 0 ? '↑ +' : '↓ '}${pp.toFixed(1)}pp`
}

function changeClass(cur: number | null, prev: number | null, upIsGood: boolean): string {
  if (cur === null || prev === null || Math.abs(cur - prev) * 100 < 0.05) return ''
  return cur > prev === upIsGood ? 'delta-good' : 'delta-bad'
}

function clusterEntries(c: HealthData['crash_clusters'][number]): [DashFilterKey, Grouped][] {
  return [
    ['version', single(c.version)],
    ['board', single(c.board)]
  ]
}
</script>

<style scoped>
.clickable {
  cursor: pointer;
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

.badge.status-new {
  background: rgba(239, 68, 68, 0.15);
  color: #f87171;
}

.badge.status-rising {
  background: rgba(245, 158, 11, 0.15);
  color: #fbbf24;
}

.health-line {
  margin: 0;
  color: var(--text-secondary);
}

.health-line.low {
  padding-left: 18px;
}
</style>
