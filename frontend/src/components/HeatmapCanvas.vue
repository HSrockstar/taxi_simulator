<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import type { DashboardSnapshot } from '@/types'

const props = defineProps<{ snapshot: DashboardSnapshot | null }>()

const canvas = ref<HTMLCanvasElement | null>(null)
const tooltip = ref<{ x: number; y: number; text: string } | null>(null)
const label = computed(() => tooltip.value?.text ?? '悬停查看网格供需情况')

const offscreen = document.createElement('canvas')
offscreen.width = 100
offscreen.height = 100
const offscreenContext = offscreen.getContext('2d')
let gridImage: ImageData | null = null

function hslToRgb(hue: number, saturation: number, lightness: number): [number, number, number] {
  const s = saturation / 100
  const l = lightness / 100
  const channel = (n: number): number => {
    const k = (n + hue / 30) % 12
    const a = s * Math.min(l, 1 - l)
    return l - a * Math.max(-1, Math.min(k - 3, Math.min(9 - k, 1)))
  }
  return [Math.round(channel(0) * 255), Math.round(channel(8) * 255), Math.round(channel(4) * 255)]
}

function cellRgb(pending: number, idle: number): [number, number, number] {
  const difference = pending - idle
  if (difference > 0) {
    const strength = Math.min(1, difference / 5)
    return hslToRgb(7 - strength * 7, 80, 25 + strength * 29)
  }
  if (difference < 0) {
    const strength = Math.min(1, -difference / 3)
    return hslToRgb(150 + strength * 12, 56, 18 + strength * 23)
  }
  return pending > 0 ? [114, 83, 55] : [14, 32, 42]
}

function draw(): void {
  const element = canvas.value
  const snapshot = props.snapshot
  if (!element || !snapshot || !offscreenContext) return

  const dpr = window.devicePixelRatio || 1
  const target = Math.max(200, Math.min(2048, Math.round(element.clientWidth * dpr)))
  if (element.width !== target) {
    element.width = target
    element.height = target
  }
  const size = element.width
  const context = element.getContext('2d')
  if (!context) return

  if (!gridImage) gridImage = offscreenContext.createImageData(100, 100)
  const data = gridImage.data
  for (let index = 0; index < 10000; index += 1) {
    const [red, green, blue] = cellRgb(snapshot.pending[index], snapshot.idle[index])
    const offset = index * 4
    data[offset] = red
    data[offset + 1] = green
    data[offset + 2] = blue
    data[offset + 3] = 255
  }
  offscreenContext.putImageData(gridImage, 0, 0)

  const cellSize = size / 100
  context.imageSmoothingEnabled = false
  context.fillStyle = '#091820'
  context.fillRect(0, 0, size, size)
  context.drawImage(offscreen, 0, 0, size, size)

  context.strokeStyle = '#091820'
  context.lineWidth = Math.max(0.4, cellSize * 0.035)
  context.beginPath()
  for (let line = 0; line <= 100; line += 1) {
    const position = line * cellSize
    context.moveTo(position, 0)
    context.lineTo(position, size)
    context.moveTo(0, position)
    context.lineTo(size, position)
  }
  context.stroke()

  for (const driver of snapshot.drivers) {
    if (driver.state === 'SERVING') continue
    context.beginPath()
    const radius = cellSize * (driver.state === 'REBALANCING' ? 0.42 : 0.25)
    context.arc(driver.x / 1000 * size, driver.y / 1000 * size, radius, 0, Math.PI * 2)
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
