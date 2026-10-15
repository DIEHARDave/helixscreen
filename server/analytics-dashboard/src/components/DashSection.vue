<template>
  <section class="dash-section">
    <h2 class="section-title">{{ title }}</h2>
    <div v-if="loading" class="loading">Loading...</div>
    <div v-else-if="error || renderError" class="error">{{ error || renderError }}</div>
    <RenderBoundary v-else><slot /></RenderBoundary>
  </section>
</template>

<script setup lang="ts">
import { onErrorCaptured, ref, watch } from 'vue'
import RenderBoundary from './RenderBoundary.vue'

const props = defineProps<{
  title: string
  loading?: boolean
  error?: string
}>()

// A section whose content throws while rendering shows the message in place of
// its content; the rest of the page keeps rendering. A new load clears it.
const renderError = ref('')
watch(
  () => props.loading,
  (loading) => {
    if (loading) renderError.value = ''
  }
)
onErrorCaptured((err) => {
  renderError.value = `Failed to render: ${err instanceof Error ? err.message : String(err)}`
  return false
})
</script>
