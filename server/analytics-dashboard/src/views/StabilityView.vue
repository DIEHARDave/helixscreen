<template>
  <AppLayout>
    <div class="page">
      <div class="page-header">
        <h2>Stability</h2>
      </div>

      <CrashClustersSection />
      <CrashRateSection />
      <ErrorCodesSection />
      <MemoryWarningsSection />
      <ConnectionSection />
      <DisplayAnomaliesSection />
    </div>
  </AppLayout>
</template>

<script setup lang="ts">
import AppLayout from '@/components/AppLayout.vue'
import CrashClustersSection from '@/components/sections/CrashClustersSection.vue'
import CrashRateSection from '@/components/sections/CrashRateSection.vue'
import ErrorCodesSection from '@/components/sections/ErrorCodesSection.vue'
import MemoryWarningsSection from '@/components/sections/MemoryWarningsSection.vue'
import ConnectionSection from '@/components/sections/ConnectionSection.vue'
import DisplayAnomaliesSection from '@/components/sections/DisplayAnomaliesSection.vue'
</script>
