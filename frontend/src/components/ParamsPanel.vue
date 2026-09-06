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

interface FieldConfig {
  key: keyof ParamsSnapshot
  label: string
  hint: string
  min: number
  max: number
  step: number
}

const FIELDS: FieldConfig[] = [
  { key: 'driverCount', label: '司机数量', hint: '10 – 500', min: 10, max: 500, step: 10 },
  { key: 'orderRateMin', label: '订单率下限', hint: '单/秒', min: 0, max: 50, step: 1 },
  { key: 'orderRateMax', label: '订单率上限', hint: '单/秒', min: 0, max: 50, step: 1 },
  { key: 'matchRadius', label: '撮合半径', hint: '格', min: 1, max: 20, step: 1 },
  { key: 'rebalanceRadius', label: '调度半径', hint: '格', min: 1, max: 30, step: 1 },
  { key: 'imbalanceThreshold', label: '失衡阈值', hint: '单差', min: 1, max: 20, step: 1 },
  { key: 'orderTimeout', label: '订单超时', hint: '秒', min: 1, max: 60, step: 1 },
]

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

function bump(field: FieldConfig, direction: 1 | -1): void {
  const current = Number(draft[field.key])
  const base = Number.isFinite(current) ? current : DEFAULTS[field.key]
  draft[field.key] = Math.min(field.max, Math.max(field.min, base + direction * field.step))
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
        <h2>引擎参数调节</h2>
        <p class="heading-description">保存后立即生效，作用于下一个模拟节拍（100ms）</p>
      </div>
      <span v-if="dirty" class="dirty-badge">未保存</span>
    </div>
    <div class="params-grid">
      <label v-for="field in FIELDS" :key="field.key" class="param-field">
        <span class="param-label"><span>{{ field.label }}</span><small>{{ field.hint }}</small></span>
        <span class="stepper">
          <input v-model.number="draft[field.key]" type="number" :min="field.min" :max="field.max" :step="field.step" />
          <button type="button" class="step-btn" :aria-label="`减小${field.label}`" @click.prevent="bump(field, -1)">
            <svg width="10" height="10" viewBox="0 0 10 10" aria-hidden="true"><path d="M1.5 5h7" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" /></svg>
          </button>
          <button type="button" class="step-btn" :aria-label="`增大${field.label}`" @click.prevent="bump(field, 1)">
            <svg width="10" height="10" viewBox="0 0 10 10" aria-hidden="true"><path d="M5 1.5v7M1.5 5h7" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" /></svg>
          </button>
        </span>
      </label>
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
