<template>
  <div class="delta-era-waiting">{{ waitingForNewClient(devices) }}</div>
</template>

<script setup lang="ts">
import { waitingForNewClient } from '@/utils/format'

defineProps<{
  devices: number
}>()
</script>

<style scoped>
.delta-era-waiting {
  padding: 24px;
  text-align: center;
  color: var(--text-secondary);
  border: 1px dashed var(--border);
  border-radius: 8px;
}
</style>
