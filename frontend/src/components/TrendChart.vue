<script setup lang="ts">
import { computed } from 'vue'
import type { TrendPoint } from '@/types'

const props = defineProps<{ history: TrendPoint[] }>()

const queuePath = computed(() => {
  if (props.history.length < 2) return ''
  const maximum = Math.max(1, ...props.history.map((point) => point.queueLength))
  return props.history.map((point, index) => {
    const x = index / (props.history.length - 1) * 100
    const y = 90 - point.queueLength / maximum * 78
    return `${index === 0 ? 'M' : 'L'} ${x} ${y}`
  }).join(' ')
})

const successPath = computed(() => {
  if (props.history.length < 2) return ''
  return props.history.map((point, index) => {
    const x = index / (props.history.length - 1) * 100
    const y = 90 - point.successRate / 100 * 78
    return `${index === 0 ? 'M' : 'L'} ${x} ${y}`
  }).join(' ')
})
</script>

<template>
  <section class="panel trend-panel">
    <div class="panel-heading compact-heading">
      <div>
        <p class="section-kicker">LIVE TREND</p>
        <h2>实时运行趋势</h2>
      </div>
      <div class="mini-legend"><span class="queue-line"></span>队列 <span class="rate-line"></span>成功率</div>
    </div>
    <svg viewBox="0 0 100 100" preserveAspectRatio="none" aria-label="队列长度与派单成功率趋势图">
      <path class="grid-line" d="M 0 12 H 100 M 0 38 H 100 M 0 64 H 100 M 0 90 H 100" />
      <path v-if="queuePath" class="queue-path" :d="queuePath" />
      <path v-if="successPath" class="rate-path" :d="successPath" />
    </svg>
  </section>
</template>
