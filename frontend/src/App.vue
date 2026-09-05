<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { controlSimulation, openSnapshotStream } from './api'
import DispatchLogs from './components/DispatchLogs.vue'
import HeatmapCanvas from './components/HeatmapCanvas.vue'
import MetricCard from './components/MetricCard.vue'
import ParamsPanel from './components/ParamsPanel.vue'
import TrendChart from './components/TrendChart.vue'
import type { DashboardSnapshot, TrendPoint } from './types'

const snapshot = ref<DashboardSnapshot | null>(null)
const history = ref<TrendPoint[]>([])
const loading = ref(true)
const errorMessage = ref('')
const controlling = ref(false)
let eventSource: EventSource | undefined

const metrics = computed(() => snapshot.value?.metrics)
const statusText = computed(() => {
  if (errorMessage.value) return '连接已断开'
  if (snapshot.value?.paused) return '模拟已暂停'
  return '引擎运行中'
})
const tickText = computed(() => {
  const tick = snapshot.value?.tick ?? 0
  return `T+${String(tick).padStart(3, '0')}s`
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

function acceptSnapshot(data: DashboardSnapshot): void {
  snapshot.value = data
  addHistory(data)
  loading.value = false
}

onMounted(() => {
  eventSource = openSnapshotStream({
    onSnapshot: (data) => {
      errorMessage.value = ''
      acceptSnapshot(data)
    },
    onStatus: (connected) => {
      if (connected) {
        errorMessage.value = ''
      } else if (!errorMessage.value) {
        errorMessage.value = '实时连接已断开，正在自动重连……'
      }
    },
  })
})

onBeforeUnmount(() => eventSource?.close())

async function control(action: 'pause' | 'resume' | 'reset'): Promise<void> {
  if (controlling.value) return
  controlling.value = true
  try {
    await controlSimulation(action)
    if (action === 'reset') history.value = []
  } catch (error) {
    errorMessage.value = error instanceof Error ? error.message : '控制请求失败'
  } finally {
    controlling.value = false
  }
}
</script>

<template>
  <main class="dashboard-shell">
    <header class="topbar">
      <div class="brand">
        <span class="brand-mark" aria-hidden="true">
          <svg width="46" height="46" viewBox="0 0 40 40" fill="none">
            <defs>
              <linearGradient id="brand-route" x1="9" y1="28" x2="31" y2="9" gradientUnits="userSpaceOnUse">
                <stop stop-color="#2fb3c9" />
                <stop offset="1" stop-color="#7ff0f8" />
              </linearGradient>
            </defs>
            <rect x="2" y="2" width="36" height="36" rx="10" fill="rgba(86,225,236,.08)" stroke="rgba(86,225,236,.45)" stroke-width="1.5" />
            <path d="M14 4v32M26 4v32M4 14h32M4 26h32" stroke="rgba(86,225,236,.14)" stroke-width="1" />
            <path d="M9 28 L18 18 L23 22 L31 9" stroke="url(#brand-route)" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" />
            <circle cx="9" cy="28" r="2" fill="rgba(86,225,236,.55)" />
            <circle cx="31" cy="9" r="3" fill="#5ce5ed" />
          </svg>
        </span>
        <div>
          <h1>智能网约车实时调度中心</h1>
          <p class="topbar-subtitle">100 × 100 时空网格 · C++ 手工数据结构引擎 · 实时供需平衡</p>
        </div>
      </div>
      <div class="engine-status" :class="{ paused: snapshot?.paused, offline: Boolean(errorMessage) }">
        <span class="led" aria-hidden="true"></span>{{ statusText }}
        <span v-if="snapshot" class="engine-tick">{{ tickText }}</span>
      </div>
    </header>

    <div v-if="errorMessage" class="connection-warning">
      <svg width="16" height="16" viewBox="0 0 16 16" fill="none" aria-hidden="true">
        <path d="M8 2 14.7 13.6H1.3L8 2Z" stroke="#ff8b96" stroke-width="1.4" stroke-linejoin="round" />
        <path d="M8 6.6v3.2" stroke="#ff8b96" stroke-width="1.4" stroke-linecap="round" />
        <path d="M8 11.9h.01" stroke="#ff8b96" stroke-width="1.6" stroke-linecap="round" />
      </svg>
      <p><strong>暂时无法获取引擎数据。</strong>{{ errorMessage }}。请确认 C++ 模拟器已在 8080 端口启动。</p>
    </div>

    <section class="dashboard-grid" :class="{ 'is-loading': loading }">
      <div class="left-column">
        <HeatmapCanvas :snapshot="snapshot" />
        <TrendChart :history="history" />
      </div>

      <aside class="right-column">
        <section class="panel metrics-panel">
          <div class="panel-heading compact-heading">
            <div>
              <h2>全局指标</h2>
              <p class="heading-description">热点区域 #{{ (snapshot?.hotspotIndex ?? 0) + 1 }} · 指标每秒随引擎刷新</p>
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
              <svg v-if="snapshot?.paused" width="13" height="13" viewBox="0 0 14 14" aria-hidden="true">
                <path d="M4.4 2.8v8.4L11.4 7 4.4 2.8Z" fill="currentColor" />
              </svg>
              <svg v-else width="13" height="13" viewBox="0 0 14 14" aria-hidden="true">
                <path d="M4.6 3v8M9.4 3v8" stroke="currentColor" stroke-width="2" stroke-linecap="round" />
              </svg>
              {{ snapshot?.paused ? '继续模拟' : '暂停模拟' }}
            </button>
            <button :disabled="controlling || !snapshot" class="secondary-button" @click="control('reset')">
              <svg width="13" height="13" viewBox="0 0 14 14" fill="none" aria-hidden="true">
                <path d="M12.2 7A5.2 5.2 0 1 1 9.8 2.6" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" />
                <path d="M12.4 1.6v3.2H9.2" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" />
              </svg>
              重置数据
            </button>
          </div>
        </section>

        <ParamsPanel :params="snapshot?.params ?? null" />
        <DispatchLogs :logs="snapshot?.logs ?? []" />
      </aside>
    </section>
  </main>
</template>
