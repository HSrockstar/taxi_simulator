<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { controlSimulation, getSnapshot } from './api'
import DispatchLogs from './components/DispatchLogs.vue'
import HeatmapCanvas from './components/HeatmapCanvas.vue'
import MetricCard from './components/MetricCard.vue'
import TrendChart from './components/TrendChart.vue'
import type { DashboardSnapshot, TrendPoint } from './types'

const snapshot = ref<DashboardSnapshot | null>(null)
const history = ref<TrendPoint[]>([])
const loading = ref(true)
const errorMessage = ref('')
const controlling = ref(false)
let refreshTimer: number | undefined
let requestController: AbortController | undefined

const metrics = computed(() => snapshot.value?.metrics)
const statusText = computed(() => {
  if (errorMessage.value) return '连接已断开'
  if (snapshot.value?.paused) return '模拟已暂停'
  return '引擎运行中'
})

function addHistory(data: DashboardSnapshot): void {
  const lastPoint = history.value.at(-1)
  if (lastPoint?.tick === data.tick) return
  history.value.push({
    tick: data.tick,
    queueLength: data.metrics.queueLength,
    successRate: data.metrics.successRate,
  })
  if (history.value.length > 36) history.value.shift()
}

async function refresh(): Promise<void> {
  if (requestController) return
  requestController = new AbortController()
  try {
    const data = await getSnapshot(requestController.signal)
    snapshot.value = data
    addHistory(data)
    errorMessage.value = ''
  } catch (error) {
    if (!(error instanceof DOMException && error.name === 'AbortError')) {
      errorMessage.value = error instanceof Error ? error.message : '无法连接模拟器'
    }
  } finally {
    requestController = undefined
    loading.value = false
  }
}

async function control(action: 'pause' | 'resume' | 'reset'): Promise<void> {
  if (controlling.value) return
  controlling.value = true
  try {
    await controlSimulation(action)
    if (action === 'reset') history.value = []
    window.setTimeout(() => void refresh(), action === 'reset' ? 500 : 100)
  } catch (error) {
    errorMessage.value = error instanceof Error ? error.message : '控制请求失败'
  } finally {
    controlling.value = false
  }
}

onMounted(() => {
  void refresh()
  refreshTimer = window.setInterval(() => void refresh(), 500)
})

onBeforeUnmount(() => {
  if (refreshTimer) window.clearInterval(refreshTimer)
  requestController?.abort()
})
</script>

<template>
  <main class="dashboard-shell">
    <header class="topbar">
      <div>
        <p class="eyebrow">TAXI · DISPATCH CONTROL CENTER</p>
        <h1>智能网约车实时调度中心</h1>
        <p class="topbar-subtitle">100 × 100 时空网格 · C++ 手工数据结构引擎 · 实时供需平衡</p>
      </div>
      <div class="engine-status" :class="{ paused: snapshot?.paused, offline: Boolean(errorMessage) }">
        <i></i>{{ statusText }}
      </div>
    </header>

    <div v-if="errorMessage" class="connection-warning">
      <strong>暂时无法获取引擎数据。</strong>{{ errorMessage }}。请确认 C++ 模拟器已在 8080 端口启动。
    </div>

    <section class="dashboard-grid" :class="{ 'is-loading': loading }">
      <HeatmapCanvas :snapshot="snapshot" />

      <aside class="right-column">
        <section class="panel metrics-panel">
          <div class="panel-heading compact-heading">
            <div>
              <p class="section-kicker">SYSTEM OVERVIEW</p>
              <h2>全局指标</h2>
              <p class="heading-description">模拟时间 {{ snapshot?.tick ?? 0 }} 秒 · 热点区域 {{ (snapshot?.hotspotIndex ?? 0) + 1 }}</p>
            </div>
          </div>
          <div class="metric-grid">
            <MetricCard label="等待队列" :value="metrics?.queueLength ?? 0" suffix="个订单" tone="amber" />
            <MetricCard label="派单成功率" :value="`${(metrics?.successRate ?? 0).toFixed(1)}%`" suffix="已结束订单" tone="green" />
            <MetricCard label="成功派单" :value="metrics?.matched ?? 0" suffix="累计订单" tone="cyan" />
            <MetricCard label="超时取消" :value="metrics?.cancelled ?? 0" suffix="累计订单" tone="red" />
            <MetricCard label="平均撮合" :value="(metrics?.averageMatchMicros ?? 0).toFixed(1)" suffix="微秒" tone="cyan" />
            <MetricCard label="完成行程" :value="metrics?.completed ?? 0" suffix="司机已回归" tone="green" />
          </div>
          <div class="controls">
            <button :disabled="controlling || !snapshot" class="primary-button" @click="control(snapshot?.paused ? 'resume' : 'pause')">
              {{ snapshot?.paused ? '继续模拟' : '暂停模拟' }}
            </button>
            <button :disabled="controlling || !snapshot" class="secondary-button" @click="control('reset')">重置数据</button>
          </div>
        </section>

        <TrendChart :history="history" />
        <DispatchLogs :logs="snapshot?.logs ?? []" />
      </aside>
    </section>
  </main>
</template>
