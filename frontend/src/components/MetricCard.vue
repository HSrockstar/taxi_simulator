<script setup lang="ts">
defineProps<{
  label: string
  value: string | number
  suffix?: string
  tone?: 'cyan' | 'green' | 'amber' | 'red'
}>()
</script>

<template>
  <article class="metric-card" :class="`tone-${tone ?? 'cyan'}`">
    <span class="metric-label">{{ label }}</span>
    <strong class="metric-value">{{ value }}</strong>
    <small v-if="suffix" class="metric-suffix">{{ suffix }}</small>
  </article>
</template>
