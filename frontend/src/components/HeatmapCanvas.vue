<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import type { DashboardSnapshot } from '@/types'

const props = defineProps<{ snapshot: DashboardSnapshot | null }>()

const canvas = ref<HTMLCanvasElement | null>(null)
const tooltip = ref<{ x: number; y: number; text: string } | null>(null)
const label = computed(() => tooltip.value?.text ?? '悬停查看网格供需情况')

function cellColor(pending: number, idle: number): string {
  const difference = pending - idle
  if (difference > 0) {
    const strength = Math.min(1, difference / 5)
    return `hsl(${Math.round(7 - strength * 7)} 80% ${Math.round(25 + strength * 29)}%)`
  }
  if (difference < 0) {
    const strength = Math.min(1, -difference / 3)
    return `hsl(${Math.round(150 + strength * 12)} 56% ${Math.round(18 + strength * 23)}%)`
  }
  return pending > 0 ? '#725337' : '#0e202a'
}

function draw(): void {
  const element = canvas.value
  const snapshot = props.snapshot
  if (!element || !snapshot) return

  const size = element.width
  const context = element.getContext('2d')
  if (!context) return
  const cellSize = size / 100
  context.clearRect(0, 0, size, size)

  for (let index = 0; index < 10000; index += 1) {
    const x = (index % 100) * cellSize
    const y = Math.floor(index / 100) * cellSize
    context.fillStyle = cellColor(snapshot.pending[index], snapshot.idle[index])
    context.fillRect(x, y, cellSize - 0.28, cellSize - 0.28)
  }

  for (const driver of snapshot.drivers) {
    if (driver.state === 'SERVING') continue
    context.beginPath()
    context.arc(driver.x / 1000 * size, driver.y / 1000 * size, driver.state === 'REBALANCING' ? 3.4 : 2, 0, Math.PI * 2)
    context.fillStyle = driver.state === 'REBALANCING' ? '#55d8ff' : '#ddfff2'
    context.fill()
  }
}

function handlePointer(event: PointerEvent): void {
  const element = canvas.value
  const snapshot = props.snapshot
  if (!element || !snapshot) return
  const bounds = element.getBoundingClientRect()
  const cellX = Math.min(99, Math.max(0, Math.floor((event.clientX - bounds.left) / bounds.width * 100)))
  const cellY = Math.min(99, Math.max(0, Math.floor((event.clientY - bounds.top) / bounds.height * 100)))
  const index = cellY * 100 + cellX
  tooltip.value = {
    x: event.clientX - bounds.left + 14,
    y: event.clientY - bounds.top + 14,
    text: `网格 (${cellX}, ${cellY}) · 等待订单 ${snapshot.pending[index]} · 空闲司机 ${snapshot.idle[index]}`,
  }
}

function clearTooltip(): void {
  tooltip.value = null
}

function resizeCanvas(): void {
  draw()
}

watch(() => props.snapshot, draw, { deep: false })
onMounted(() => {
  window.addEventListener('resize', resizeCanvas)
  draw()
})
onBeforeUnmount(() => window.removeEventListener('resize', resizeCanvas))
</script>

<template>
  <section class="panel map-panel">
    <div class="panel-heading">
      <div>
        <p class="section-kicker">SPATIAL SUPPLY & DEMAND</p>
        <h2>城市供需热力图</h2>
        <p class="heading-description">红色代表订单积压，绿色代表空闲运力，青色点位表示正在调度。</p>
      </div>
      <div class="legend" aria-label="热力图图例">
        <span><i class="legend-dot demand"></i>需求缺口</span>
        <span><i class="legend-dot supply"></i>运力富余</span>
      </div>
    </div>
    <div class="map-frame">
      <canvas ref="canvas" width="800" height="800" aria-label="城市供需热力图" @pointermove="handlePointer" @pointerleave="clearTooltip" />
      <div v-if="tooltip" class="map-tooltip" :style="{ left: `${tooltip.x}px`, top: `${tooltip.y}px` }">{{ tooltip.text }}</div>
      <p class="map-caption">{{ label }}</p>
    </div>
  </section>
</template>
