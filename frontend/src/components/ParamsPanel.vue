<script setup lang="ts">
import { computed, reactive, ref, watch } from 'vue'
import { updateParams } from '@/api'
import type { ParamsSnapshot } from '@/types'

const props = defineProps<{ params: ParamsSnapshot | null }>()

const DEFAULTS: ParamsSnapshot = {
  driverCount: 100,
  orderRateMin: 5,
  orderRateMax: 10,
  matchRadius: 3,
  rebalanceRadius: 10,
  imbalanceThreshold: 2,
  orderTimeout: 10,
}

const draft = reactive<ParamsSnapshot>({ ...DEFAULTS })
const serverParams = ref<ParamsSnapshot | null>(null)
const applying = ref(false)
const applyState = ref<'idle' | 'ok' | 'fail'>('idle')
const errorMessage = ref('')

function sameParams(left: ParamsSnapshot, right: ParamsSnapshot): boolean {
  return left.driverCount === right.driverCount &&
    left.orderRateMin === right.orderRateMin &&
    left.orderRateMax === right.orderRateMax &&
    left.matchRadius === right.matchRadius &&
    left.rebalanceRadius === right.rebalanceRadius &&
    left.imbalanceThreshold === right.imbalanceThreshold &&
    left.orderTimeout === right.orderTimeout
}

const dirty = computed(() => serverParams.value !== null && !sameParams(draft, serverParams.value))

watch(() => props.params, (value) => {
  if (!value || applying.value) return
  // 用户正在编辑时不用服务端推送覆盖草稿
  const editing = dirty.value
  serverParams.value = value
  if (!editing) Object.assign(draft, value)
}, { immediate: true })

function coerce(value: number | string): number {
  const parsed = typeof value === 'number' ? value : Number(value)
  return Number.isFinite(parsed) ? Math.round(parsed) : NaN
}

async function apply(): Promise<void> {
  if (applying.value) return
  applying.value = true
  errorMessage.value = ''
  try {
    const saved = await updateParams({
      driverCount: coerce(draft.driverCount),
      orderRateMin: coerce(draft.orderRateMin),
      orderRateMax: coerce(draft.orderRateMax),
      matchRadius: coerce(draft.matchRadius),
      rebalanceRadius: coerce(draft.rebalanceRadius),
      imbalanceThreshold: coerce(draft.imbalanceThreshold),
      orderTimeout: coerce(draft.orderTimeout),
    })
    Object.assign(draft, saved)
    serverParams.value = saved
    applyState.value = 'ok'
  } catch {
    applyState.value = 'fail'
    errorMessage.value = '参数未保存，请检查各字段的合法范围。'
  } finally {
    applying.value = false
    window.setTimeout(() => { applyState.value = 'idle' }, 2000)
  }
}

function resetDefaults(): void {
  Object.assign(draft, DEFAULTS)
  errorMessage.value = ''
}
</script>

<template>
  <section class="panel params-panel">
    <div class="panel-heading compact-heading">
      <div>
        <p class="section-kicker">ENGINE TUNING</p>
        <h2>引擎参数调节</h2>
        <p class="heading-description">保存后立即生效，作用于下一个模拟秒。</p>
      </div>
      <span v-if="dirty" class="dirty-badge">未保存</span>
    </div>
    <div class="params-grid">
      <label class="param-field"><span>司机数 10-500</span><input v-model.number="draft.driverCount" type="number" min="10" max="500" step="10" /></label>
      <label class="param-field"><span>订单率下限/秒</span><input v-model.number="draft.orderRateMin" type="number" min="0" max="50" /></label>
      <label class="param-field"><span>订单率上限/秒</span><input v-model.number="draft.orderRateMax" type="number" min="0" max="50" /></label>
      <label class="param-field"><span>撮合半径 (格)</span><input v-model.number="draft.matchRadius" type="number" min="1" max="20" /></label>
      <label class="param-field"><span>调度半径 (格)</span><input v-model.number="draft.rebalanceRadius" type="number" min="1" max="30" /></label>
      <label class="param-field"><span>失衡阈值</span><input v-model.number="draft.imbalanceThreshold" type="number" min="1" max="20" /></label>
      <label class="param-field"><span>超时时间 (秒)</span><input v-model.number="draft.orderTimeout" type="number" min="1" max="60" /></label>
    </div>
    <p v-if="errorMessage" class="param-error">{{ errorMessage }}</p>
    <div class="param-actions">
      <button class="primary-button" :disabled="applying" @click="apply">
        {{ applyState === 'ok' ? '已应用 ✓' : applyState === 'fail' ? '参数无效' : applying ? '保存中…' : '应用参数' }}
      </button>
      <button class="secondary-button" :disabled="applying" @click="resetDefaults">恢复默认</button>
    </div>
  </section>
</template>
