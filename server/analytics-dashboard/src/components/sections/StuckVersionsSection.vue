<template>
  <DashSection title="Devices on old versions" :loading="loading" :error="error">
    <DistributionBar
      v-if="data"
      :rows="rows"
      filter-key="version"
      title="Older release lines"
      :caption="`Distinct devices with a session, ${span}, on lines older than the newest two; click a line to expand it`"
    />
  </DashSection>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import DashSection from '@/components/DashSection.vue'
import DistributionBar from '@/components/DistributionBar.vue'
import { useDashData } from '@/composables/useDashData'
import { api } from '@/services/api'
import { groupVersions } from '@/utils/versions'

const { data, loading, error, span, filters } = useDashData(api.getAdoption)

const rows = computed(() => {
  const versions = data.value?.versions ?? []
  // Every raw version on the two newest lines, whatever the expansion state.
  const current = new Set(
    groupVersions(versions, new Set())
      .filter((g) => !g.key.startsWith('line:'))
      .flatMap((g) => g.members)
  )
  return groupVersions(versions, new Set(filters.expand)).filter(
    (g) => !g.members.every((m) => current.has(m))
  )
})
</script>
