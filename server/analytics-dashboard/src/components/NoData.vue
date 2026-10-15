<template>
  <div class="empty-state">{{ text }}</div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import type { DeltaEra } from '@/services/api'
import { useFiltersStore } from '@/stores/filters'
import { DASH_FILTER_KEYS } from '@/utils/urlState'
import { emptyPlaceholder } from '@/utils/format'

const props = withDefaults(defineProps<{ era?: DeltaEra | null }>(), { era: null })

const filters = useFiltersStore()
const text = computed(() =>
  emptyPlaceholder(props.era, DASH_FILTER_KEYS.filter((key) => filters[key].length > 0))
)
</script>
