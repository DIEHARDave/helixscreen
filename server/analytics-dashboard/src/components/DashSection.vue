<template>
  <section class="dash-section">
    <h2 class="section-title">{{ title }}</h2>
    <div v-if="loading" class="loading">Loading...</div>
    <div v-else-if="error" class="error">{{ error }}</div>
    <slot v-else />
  </section>
</template>

<script setup lang="ts">
defineProps<{
  title: string
  loading?: boolean
  error?: string
}>()
</script>
