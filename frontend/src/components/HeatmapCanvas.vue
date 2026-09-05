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
const baseLayer = document.createElement('canvas')
let gridImage: ImageData | null = null

interface HotCell {
  x: number
  y: number
  strength: number
  phase: number
}

type SpriteKey = 'demand' | 'supply' | 'rebalancing'
const sprites = new Map<SpriteKey, HTMLCanvasElement>()
const spritePalettes: Record<SpriteKey, [string, string]> = {
  demand: ['rgba(255, 122, 128, .95)', 'rgba(255, 70, 80, .32)'],
  supply: ['rgba(120, 255, 190, .9)', 'rgba(60, 220, 150, .26)'],
  rebalancing: ['rgba(140, 240, 255, .95)', 'rgba(80, 210, 240, .32)'],
}

let hotCells: HotCell[] = []
let layerSize = 0
let hoverCell: { x: number; y: number } | null = null
let rafId = 0

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
  return pending > 0 ? [114, 83, 55] : [12, 30, 40]
}

function spriteFor(key: SpriteKey): HTMLCanvasElement {
  const cached = sprites.get(key)
  if (cached) return cached
  const sprite = document.createElement('canvas')
  sprite.width = 64
  sprite.height = 64
  const context = sprite.getContext('2d')
  if (!context) return sprite
  const [inner, middle] = spritePalettes[key]
  const gradient = context.createRadialGradient(32, 32, 0, 32, 32, 32)
  gradient.addColorStop(0, inner)
  gradient.addColorStop(0.32, middle)
  gradient.addColorStop(1, 'rgba(0, 0, 0, 0)')
  context.fillStyle = gradient
  context.fillRect(0, 0, 64, 64)
  sprites.set(key, sprite)
  return sprite
}

function drawRuler(context: CanvasRenderingContext2D, cell: number): void {
  context.font = `${Math.min(13, Math.max(9, cell * 1.3))}px "JetBrains Mono", Consolas, monospace`
  context.textBaseline = 'top'
  context.lineWidth = 1
  for (let tick = 0; tick < 100; tick += 5) {
    const major = tick % 25 === 0
    const position = tick * cell
    const length = major ? cell * 0.75 : cell * 0.4
    context.strokeStyle = major ? 'rgba(150, 215, 228, .42)' : 'rgba(150, 215, 228, .18)'
    context.beginPath()
    context.moveTo(position, 0)
    context.lineTo(position, length)
    context.moveTo(0, position)
    context.lineTo(length, position)
    context.stroke()
    if (major && cell >= 7) {
      context.fillStyle = 'rgba(150, 215, 228, .55)'
      // 刻度 0 与左上角装饰括号重叠，仅标注左边缘
      if (tick > 0) context.fillText(String(tick), position + 3, cell * 0.9)
      if (tick > 0) context.fillText(String(tick), cell * 0.9, position + 3)
    }
  }
}

function buildBase(snapshot: DashboardSnapshot, size: number): void {
  if (!offscreenContext) return
  baseLayer.width = size
  baseLayer.height = size
  const context = baseLayer.getContext('2d')
  if (!context) return

  if (!gridImage) gridImage = offscreenContext.createImageData(100, 100)
  const data = gridImage.data
  hotCells = []
  for (let index = 0; index < 10000; index += 1) {
    const pending = snapshot.pending[index]
    const idle = snapshot.idle[index]
    const [red, green, blue] = cellRgb(pending, idle)
    const offset = index * 4
    data[offset] = red
    data[offset + 1] = green
    data[offset + 2] = blue
    data[offset + 3] = 255
    const difference = pending - idle
    if (difference > 0) {
      hotCells.push({
        x: index % 100,
        y: Math.floor(index / 100),
        strength: Math.min(1, difference / 5),
        phase: (index % 17) * 0.7,
      })
    }
  }
  offscreenContext.putImageData(gridImage, 0, 0)

  const cell = size / 100
  context.imageSmoothingEnabled = false
  context.fillStyle = '#081720'
  context.fillRect(0, 0, size, size)
  context.drawImage(offscreen, 0, 0, size, size)

  context.strokeStyle = 'rgba(3, 12, 18, .88)'
  context.lineWidth = Math.max(0.5, cell * 0.04)
  context.beginPath()
  for (let line = 0; line <= 100; line += 1) {
    const position = line * cell
    context.moveTo(position, 0)
    context.lineTo(position, size)
    context.moveTo(0, position)
    context.lineTo(size, position)
  }
  context.stroke()

  drawRuler(context, cell)
  layerSize = size
}

function composite(now: number): void {
  const element = canvas.value
  const snapshot = props.snapshot
  if (element && snapshot && layerSize) {
    const context = element.getContext('2d')
    if (context) {
      context.drawImage(baseLayer, 0, 0)

      const cell = layerSize / 100
      const seconds = now / 1000

      // 热点区呼吸光晕：叠加模式让红色积压像灯一样明灭
      context.globalCompositeOperation = 'lighter'
      for (const hot of hotCells) {
        if (hot.strength < 0.08) continue
        const breath = 0.72 + 0.28 * Math.sin(seconds * 2 + hot.phase)
        const glowRadius = cell * (1.5 + hot.strength * 2.6)
        context.globalAlpha = (0.1 + 0.34 * hot.strength) * breath
        const sprite = spriteFor('demand')
        context.drawImage(sprite, hot.x * cell + cell / 2 - glowRadius, hot.y * cell + cell / 2 - glowRadius, glowRadius * 2, glowRadius * 2)
      }
      context.globalAlpha = 1
      context.globalCompositeOperation = 'source-over'

      for (const driver of snapshot.drivers) {
        if (driver.state === 'SERVING') continue
        const rebalancing = driver.state === 'REBALANCING'
        const px = (driver.x / 1000) * layerSize
        const py = (driver.y / 1000) * layerSize
        const glowRadius = cell * (rebalancing ? 1.5 : 1)
        context.globalAlpha = rebalancing ? 0.85 : 0.6
        context.drawImage(spriteFor(rebalancing ? 'rebalancing' : 'supply'), px - glowRadius, py - glowRadius, glowRadius * 2, glowRadius * 2)
        context.globalAlpha = 1
        context.beginPath()
        context.arc(px, py, cell * (rebalancing ? 0.3 : 0.22), 0, Math.PI * 2)
        context.fillStyle = rebalancing ? '#c9f4ff' : '#eafff6'
        context.fill()
      }

      if (hoverCell) {
        const cx = hoverCell.x * cell
        const cy = hoverCell.y * cell
        context.fillStyle = 'rgba(120, 230, 244, .08)'
        context.fillRect(cx, cy, cell, cell)
        const outline = Math.max(1, cell * 0.12)
        context.strokeStyle = 'rgba(140, 240, 250, .9)'
        context.lineWidth = outline
        context.strokeRect(cx + outline / 2, cy + outline / 2, cell - outline, cell - outline)
        context.strokeStyle = 'rgba(120, 230, 244, .22)'
        context.lineWidth = 1
        context.beginPath()
        context.moveTo(cx + cell / 2, 0)
        context.lineTo(cx + cell / 2, layerSize)
        context.moveTo(0, cy + cell / 2)
        context.lineTo(layerSize, cy + cell / 2)
        context.stroke()
      }
    }
  }
  rafId = requestAnimationFrame(composite)
}

function syncSize(): number {
  const element = canvas.value
  if (!element) return 0
  const dpr = window.devicePixelRatio || 1
  const target = Math.max(200, Math.min(2048, Math.round(element.clientWidth * dpr)))
  if (element.width !== target) {
    element.width = target
    element.height = target
  }
  return element.width
}

function rebuild(): void {
  const snapshot = props.snapshot
  if (!snapshot) return
  const size = syncSize()
  if (size) buildBase(snapshot, size)
}

function handlePointer(event: PointerEvent): void {
  const element = canvas.value
  const snapshot = props.snapshot
  if (!element || !snapshot) return
  const bounds = element.getBoundingClientRect()
  const cellX = Math.min(99, Math.max(0, Math.floor(((event.clientX - bounds.left) / bounds.width) * 100)))
  const cellY = Math.min(99, Math.max(0, Math.floor(((event.clientY - bounds.top) / bounds.height) * 100)))
  hoverCell = { x: cellX, y: cellY }
  const index = cellY * 100 + cellX
  tooltip.value = {
    x: Math.min(event.clientX - bounds.left + 14, bounds.width - 216),
    y: Math.min(event.clientY - bounds.top + 14, bounds.height - 44),
    text: `网格 (${cellX}, ${cellY}) · 等待订单 ${snapshot.pending[index]} · 空闲司机 ${snapshot.idle[index]}`,
  }
}

function clearTooltip(): void {
  tooltip.value = null
  hoverCell = null
}

watch(() => props.snapshot, rebuild)
onMounted(() => {
  window.addEventListener('resize', rebuild)
  rebuild()
  rafId = requestAnimationFrame(composite)
})
onBeforeUnmount(() => {
  window.removeEventListener('resize', rebuild)
  cancelAnimationFrame(rafId)
})
</script>

<template>
  <section class="panel map-panel">
    <div class="panel-heading">
      <div>
        <h2>城市供需热力图</h2>
        <p class="heading-description">红色区域存在订单积压，绿色代表运力富余，青色点位为调度中车辆。</p>
      </div>
      <div class="map-legend" aria-label="热力图图例">
        <span class="data-chip chip-demand"><i></i>需求缺口</span>
        <span class="data-chip chip-supply"><i></i>运力富余</span>
      </div>
    </div>
    <div class="map-frame">
      <canvas ref="canvas" width="800" height="800" aria-label="城市供需热力图" @pointermove="handlePointer" @pointerleave="clearTooltip" />
      <div v-if="tooltip" class="map-tooltip" :style="{ left: `${tooltip.x}px`, top: `${tooltip.y}px` }">{{ tooltip.text }}</div>
      <p class="map-caption">{{ label }}</p>
    </div>
  </section>
</template>
