// SPDX-License-Identifier: GPL-3.0-or-later
import { computed, ref, watch, type Ref } from 'vue'
import { useFiltersStore } from '@/stores/filters'
import { rangeLabel } from '@/utils/format'

type Loader<T> = (queryString: string) => Promise<T>

// Requests still in flight, per loader and query string. Sections on one tab
// that read the same endpoint mount together and share one request.
const inflight = new WeakMap<Loader<unknown>, Map<string, Promise<unknown>>>()

function shared<T>(load: Loader<T>, queryString: string): Promise<T> {
  let byQuery = inflight.get(load as Loader<unknown>)
  if (!byQuery) {
    byQuery = new Map()
    inflight.set(load as Loader<unknown>, byQuery)
  }
  const pending = byQuery.get(queryString)
  if (pending) return pending as Promise<T>
  const request = load(queryString)
  byQuery.set(queryString, request)
  const forget = () => byQuery!.delete(queryString)
  request.then(forget, forget)
  return request
}

/**
 * Loads one dashboard endpoint for the active filters and reloads it when
 * they change. A response that arrives after a newer filter change is
 * dropped. `span` is the filter window as a caption phrase.
 */
export function useDashData<T>(load: Loader<T>) {
  const filters = useFiltersStore()
  const data = ref(null) as Ref<T | null>
  const loading = ref(true)
  const error = ref('')
  let latest = 0

  watch(
    () => filters.queryString,
    async (queryString) => {
      const ticket = ++latest
      loading.value = true
      error.value = ''
      try {
        const result = await shared(load, queryString)
        if (ticket === latest) data.value = result
      } catch (e) {
        if (ticket === latest) error.value = e instanceof Error ? e.message : 'Failed to load data'
      } finally {
        if (ticket === latest) loading.value = false
      }
    },
    { immediate: true }
  )

  const span = computed(() => rangeLabel(filters.range))
  return { data, loading, error, span, filters }
}
