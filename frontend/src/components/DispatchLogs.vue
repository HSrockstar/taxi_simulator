<script setup lang="ts">
import { computed, nextTick, ref, watch } from 'vue'
import type { LogEntry } from '@/types'

const props = defineProps<{ logs: LogEntry[] }>()
const list = ref<HTMLElement | null>(null)
const visibleLogs = computed(() => props.logs.slice(-50))

function classFor(message: string): string {
  if (message.includes('派单成功')) return 'log-success'
  if (message.includes('订单取消')) return 'log-cancel'
  if (message.includes('运力调度')) return 'log-dispatch'
  return 'log-system'
}

watch(visibleLogs, async () => {
  await nextTick()
  if (list.value) list.value.scrollTop = list.value.scrollHeight
}, { deep: true })
</script>

<template>
  <section class="panel logs-panel">
    <div class="panel-heading compact-heading">
      <div>
        <p class="section-kicker">ENGINE EVENTS</p>
        <h2>实时派单日志</h2>
      </div>
      <span class="log-count">{{ visibleLogs.length }} 条</span>
    </div>
    <div ref="list" class="log-list">
      <p v-if="visibleLogs.length === 0" class="log-empty">等待引擎事件……</p>
      <p v-for="entry in visibleLogs" :key="entry.sequence" :class="classFor(entry.message)">
        <span>#{{ entry.sequence }}</span>{{ entry.message }}
      </p>
    </div>
  </section>
</template>
