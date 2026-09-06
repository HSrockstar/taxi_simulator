<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import type { DashboardSnapshot } from '@/types'

const props = defineProps<{ snapshot: DashboardSnapshot | null }>()

const canvas = ref<HTMLCanvasElement | null>(null)
const tooltip = ref<{ x: number; y: number; text: string } | null>(null)
const label = computed(() => tooltip.value?.text ?? '悬停查看网格供需情况')

// 与 styles.css 的三列锁定布局断点保持一致：宽屏下画布由可用高度和列宽共同决定
const wideLayout = window.matchMedia('(min-width: 1280px) and (min-height: 700px)')
let resizeObserver: ResizeObserver | null = null

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

type SpriteKey = 'demand' | 'supply' | 'rebalancing' | 'enroute' | 'ontrip'
const sprites = new Map<SpriteKey, HTMLCanvasElement>()
const spritePalettes: Record<SpriteKey, [string, string]> = {
  demand: ['rgba(255, 122, 128, .95)', 'rgba(255, 70, 80, .32)'],
  supply: ['rgba(120, 255, 190, .9)', 'rgba(60, 220, 150, .26)'],
  rebalancing: ['rgba(140, 240, 255, .95)', 'rgba(80, 210, 240, .32)'],
  enroute: ['rgba(255, 186, 100, .95)', 'rgba(255, 150, 60, .30)'],
  ontrip: ['rgba(196, 164, 255, .95)', 'rgba(150, 110, 245, .30)'],
}

// 前往接客 / 行程中两类在途司机的渲染配色
const tripStyles: Record<'EN_ROUTE' | 'ON_TRIP', { line: string; dot: string; sprite: SpriteKey }> = {
  EN_ROUTE: { line: 'rgba(255, 186, 100, .5)', dot: '#ffe3b8', sprite: 'enroute' },
  ON_TRIP: { line: 'rgba(196, 164, 255, .5)', dot: '#e2d6ff', sprite: 'ontrip' },
}

let hotCells: HotCell[] = []
let alertCells: HotCell[] = []
let layerSize = 0
let hoverCell: { x: number; y: number } | null = null
let rafId = 0

interface Flow {
  fromX: number
  fromY: number
  toX: number
  toY: number
  startedAt: number
}

// 后端调度行程固定为 2 个 tick（约 2 秒），流向动画与其同步
const kFlowDurationMs = 2000
const flows = new Map<number, Flow>()

function syncFlows(snapshot: DashboardSnapshot): void {
  const now = performance.now()
  const active = new Set<number>()
  for (const driver of snapshot.drivers) {
    if (driver.state !== 'REBALANCING') continue
    active.add(driver.id)
    if (!flows.has(driver.id)) {
      flows.set(driver.id, {
        fromX: driver.x,
        fromY: driver.y,
        toX: driver.targetX,
        toY: driver.targetY,
        startedAt: now,
      })
    }
  }
  for (const id of [...flows.keys()]) {
    if (!active.has(id)) flows.delete(id)
  }
}

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
  context.font = `${Math.min(18, Math.max(10, cell * 1.35))}px "JetBrains Mono", Consolas, monospace`
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
  const threshold = snapshot.params.imbalanceThreshold
  hotCells = []
  alertCells = []
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
      const cellInfo = {
        x: index % 100,
        y: Math.floor(index / 100),
        strength: Math.min(1, difference / 5),
        phase: (index % 17) * 0.7,
      }
      hotCells.push(cellInfo)
      // 与调度引擎同一判据：差值达到失衡阈值的格子标记为红色警报热点区
      if (difference >= threshold) alertCells.push(cellInfo)
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

      // 红色警报热点区：差值达到失衡阈值的格子同步呼吸描框，随参数面板实时增减
      const alertPulse = 0.55 + 0.4 * Math.sin(seconds * 2.4)
      context.strokeStyle = `rgba(255, 84, 98, ${alertPulse.toFixed(3)})`
      context.lineWidth = Math.max(1.2, cell * 0.14)
      const inset = Math.max(0.6, cell * 0.1)
      for (const alert of alertCells) {
        context.strokeRect(alert.x * cell + inset, alert.y * cell + inset, cell - inset * 2, cell - inset * 2)
      }

      // 调度流向：起点到热点格的渐隐路径，司机点沿线移动；
      // 司机点在这里画，下方静态循环跳过 REBALANCING 避免重复
      const flowNow = performance.now()
      for (const [, flow] of flows) {
        const raw = (flowNow - flow.startedAt) / kFlowDurationMs
        if (raw >= 1) continue
        const alpha = Math.sin(raw * Math.PI)
        const eased = raw < 0.5 ? 2 * raw * raw : 1 - Math.pow(-2 * raw + 2, 2) / 2
        const fx = (flow.fromX / 1000) * layerSize
        const fy = (flow.fromY / 1000) * layerSize
        const tx = (flow.toX / 1000) * layerSize
        const ty = (flow.toY / 1000) * layerSize

        context.strokeStyle = `rgba(125, 226, 250, ${(0.5 * alpha).toFixed(3)})`
        context.lineWidth = Math.max(1, cell * 0.12)
        context.beginPath()
        context.moveTo(fx, fy)
        context.lineTo(tx, ty)
        context.stroke()

        context.strokeStyle = `rgba(125, 226, 250, ${(0.7 * alpha).toFixed(3)})`
        context.lineWidth = Math.max(1, cell * 0.1)
        context.beginPath()
        context.arc(tx, ty, cell * 0.55, 0, Math.PI * 2)
        context.stroke()

        const mx = fx + (tx - fx) * eased
        const my = fy + (ty - fy) * eased
        const glowRadius = cell * 1.5
        context.globalAlpha = 0.85 * alpha
        context.drawImage(spriteFor('rebalancing'), mx - glowRadius, my - glowRadius, glowRadius * 2, glowRadius * 2)
        context.globalAlpha = 1
        context.beginPath()
        context.arc(mx, my, cell * 0.3, 0, Math.PI * 2)
        context.fillStyle = '#c9f4ff'
        context.fill()
      }

      // 前往接客 / 行程中：虚线标出上车点或目的地，司机点随引擎每秒推进
      for (const driver of snapshot.drivers) {
        if (driver.state !== 'EN_ROUTE' && driver.state !== 'ON_TRIP') continue
        const style = tripStyles[driver.state]
        const px = (driver.x / 1000) * layerSize
        const py = (driver.y / 1000) * layerSize
        const tx = (driver.targetX / 1000) * layerSize
        const ty = (driver.targetY / 1000) * layerSize
        context.setLineDash([cell * 0.7, cell * 0.55])
        context.strokeStyle = style.line
        context.lineWidth = Math.max(1, cell * 0.1)
        context.beginPath()
        context.moveTo(px, py)
        context.lineTo(tx, ty)
        context.stroke()
        context.setLineDash([])
        const glowRadius = cell * 1.4
        context.globalAlpha = 0.85
        context.drawImage(spriteFor(style.sprite), px - glowRadius, py - glowRadius, glowRadius * 2, glowRadius * 2)
        context.globalAlpha = 1
        context.beginPath()
        context.arc(px, py, cell * 0.26, 0, Math.PI * 2)
        context.fillStyle = style.dot
        context.fill()
      }

      for (const driver of snapshot.drivers) {
        if (driver.state !== 'IDLE') continue
        const px = (driver.x / 1000) * layerSize
        const py = (driver.y / 1000) * layerSize
        const glowRadius = cell
        context.globalAlpha = 0.6
        context.drawImage(spriteFor('supply'), px - glowRadius, py - glowRadius, glowRadius * 2, glowRadius * 2)
        context.globalAlpha = 1
        context.beginPath()
        context.arc(px, py, cell * 0.22, 0, Math.PI * 2)
        context.fillStyle = '#eafff6'
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
  const stage = element?.parentElement?.parentElement ?? null
  if (!element || !stage) return 0
  const dpr = window.devicePixelRatio || 1
  let cssSize: number
  if (wideLayout.matches) {
    // 一屏锁定布局：正方形画布取「列宽与剩余高度」的较小值，居中放置
    cssSize = Math.floor(Math.min(stage.clientWidth, stage.clientHeight))
    element.style.width = `${cssSize}px`
    element.style.height = `${cssSize}px`
  } else {
    // 经典两列布局：画布宽度跟随列宽，高度按正方形延展
    element.style.width = ''
    element.style.height = ''
    cssSize = element.clientWidth
  }
  if (cssSize < 50) return 0
  const target = Math.max(200, Math.min(2048, Math.round(cssSize * dpr)))
  if (element.width !== target) {
    element.width = target
    element.height = target
  }
  return element.width
}

function rebuild(): void {
  const size = syncSize()
  const snapshot = props.snapshot
  if (!snapshot) return
  syncFlows(snapshot)
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
  const stage = canvas.value?.parentElement?.parentElement ?? null
  if (stage) {
    resizeObserver = new ResizeObserver(() => rebuild())
    resizeObserver.observe(stage)
  }
  rebuild()
  rafId = requestAnimationFrame(composite)
})
onBeforeUnmount(() => {
  resizeObserver?.disconnect()
  resizeObserver = null
  cancelAnimationFrame(rafId)
})
</script>

<template>
  <section class="panel map-panel">
    <div class="panel-heading">
      <div>
        <h2>城市供需热力图</h2>
        <p class="heading-description">红色代表订单积压，绿色代表运力富余，红框为达到失衡阈值的警报热点区，青色路径为调度流向，橙色虚线为前往接客，紫色虚线为行程中。</p>
      </div>
      <div class="map-legend" aria-label="热力图图例">
        <span class="data-chip chip-demand"><i></i>需求缺口</span>
        <span class="data-chip chip-supply"><i></i>运力富余</span>
        <span class="data-chip chip-alert"><i></i>警报热点 ≥ <b>{{ snapshot?.params.imbalanceThreshold ?? 2 }}</b></span>
        <span class="data-chip chip-enroute"><i></i>前往接客</span>
        <span class="data-chip chip-ontrip"><i></i>行程中</span>
      </div>
    </div>
    <div class="map-stage">
      <div class="map-frame">
        <canvas ref="canvas" width="800" height="800" aria-label="城市供需热力图" @pointermove="handlePointer" @pointerleave="clearTooltip" />
        <div v-if="tooltip" class="map-tooltip" :style="{ left: `${tooltip.x}px`, top: `${tooltip.y}px` }">{{ tooltip.text }}</div>
        <p class="map-caption">{{ label }}</p>
      </div>
    </div>
  </section>
</template>
