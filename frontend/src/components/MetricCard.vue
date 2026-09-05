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
    <span>{{ label }}</span>
    <strong>{{ value }}</strong>
    <small v-if="suffix">{{ suffix }}</small>
  </article>
</template>
