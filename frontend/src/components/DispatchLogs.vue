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
  // watch 在 DOM 更新前触发，此刻测得的"是否在底部"才是用户更新前的状态
  const element = list.value
  const nearBottom = !element ||
    element.scrollHeight - element.scrollTop - element.clientHeight < 40
  await nextTick()
  if (element && nearBottom) {
    element.scrollTop = element.scrollHeight
  }
})
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
