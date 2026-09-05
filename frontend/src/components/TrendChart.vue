<script setup lang="ts">
import { computed } from 'vue'
import type { TrendPoint } from '@/types'

const props = defineProps<{ history: TrendPoint[] }>()

const PLOT_TOP = 12
const PLOT_BOTTOM = 88

function samplePoints(selector: (point: TrendPoint) => number, fixedMaximum?: number): Array<{ x: number; y: number }> {
  const points = props.history
  if (points.length < 2) return []
  const maximum = fixedMaximum ?? Math.max(1, ...points.map(selector))
  return points.map((point, index) => ({
    x: (index / (points.length - 1)) * 100,
    y: PLOT_BOTTOM - (selector(point) / maximum) * (PLOT_BOTTOM - PLOT_TOP),
  }))
}

// Catmull-Rom 转三次贝塞尔，让 1 秒采样点的折线变成顺滑曲线
function smoothPath(points: Array<{ x: number; y: number }>): string {
  if (points.length < 2) return ''
  let d = `M ${points[0].x} ${points[0].y}`
  for (let i = 0; i < points.length - 1; i += 1) {
    const previous = points[Math.max(0, i - 1)]
    const current = points[i]
    const next = points[i + 1]
    const after = points[Math.min(points.length - 1, i + 2)]
    const clampY = (y: number): number => Math.min(PLOT_BOTTOM + 6, Math.max(PLOT_TOP - 6, y))
    const control1X = current.x + (next.x - previous.x) / 6
    const control1Y = clampY(current.y + (next.y - previous.y) / 6)
    const control2X = next.x - (after.x - current.x) / 6
    const control2Y = clampY(next.y - (after.y - current.y) / 6)
    d += ` C ${control1X} ${control1Y}, ${control2X} ${control2Y}, ${next.x} ${next.y}`
  }
  return d
}

function areaPath(line: string, points: Array<{ x: number; y: number }>): string {
  if (!line || points.length === 0) return ''
  const last = points[points.length - 1]
  const first = points[0]
  return `${line} L ${last.x} ${PLOT_BOTTOM} L ${first.x} ${PLOT_BOTTOM} Z`
}

const queuePoints = computed(() => samplePoints((point) => point.queueLength))
// 成功率语义即百分比，固定 0-100 刻度，避免自适应缩放把低成功率曲线顶到图表面
const ratePoints = computed(() => samplePoints((point) => point.successRate, 100))
const queueLine = computed(() => smoothPath(queuePoints.value))
const rateLine = computed(() => smoothPath(ratePoints.value))
const queueArea = computed(() => areaPath(queueLine.value, queuePoints.value))
const rateArea = computed(() => areaPath(rateLine.value, ratePoints.value))

const lastQueue = computed(() => props.history.at(-1)?.queueLength ?? 0)
const lastRate = computed(() => (props.history.at(-1)?.successRate ?? 0).toFixed(1))
</script>

<template>
  <section class="panel trend-panel">
    <div class="panel-heading compact-heading">
      <div>
        <h2>实时运行趋势</h2>
        <p class="heading-description">最近 {{ Math.max(history.length, 1) }} 秒的队列与成功率走势</p>
      </div>
      <div class="trend-chips">
        <span class="data-chip chip-queue"><i></i>队列 <b>{{ lastQueue }}</b></span>
        <span class="data-chip chip-rate"><i></i>成功率 <b>{{ lastRate }}%</b></span>
      </div>
    </div>
    <div class="trend-plot">
      <svg viewBox="0 0 100 100" preserveAspectRatio="none" aria-label="队列长度与派单成功率趋势图">
        <defs>
          <linearGradient id="trend-queue-fill" x1="0" y1="0" x2="0" y2="1">
            <stop offset="0" stop-color="#f6c960" stop-opacity=".26" />
            <stop offset="1" stop-color="#f6c960" stop-opacity="0" />
          </linearGradient>
          <linearGradient id="trend-rate-fill" x1="0" y1="0" x2="0" y2="1">
            <stop offset="0" stop-color="#56e1ec" stop-opacity=".2" />
            <stop offset="1" stop-color="#56e1ec" stop-opacity="0" />
          </linearGradient>
        </defs>
        <path class="grid-line" d="M 0 24 H 100 M 0 44 H 100 M 0 64 H 100 M 0 88 H 100" />
        <path v-if="queueArea" class="queue-area" :d="queueArea" />
        <path v-if="rateArea" class="rate-area" :d="rateArea" />
        <path v-if="queueLine" class="queue-path" :d="queueLine" />
        <path v-if="rateLine" class="rate-path" :d="rateLine" />
      </svg>
    </div>
  </section>
</template>
