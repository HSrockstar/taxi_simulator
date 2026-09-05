import type { DashboardSnapshot } from './types'

async function request<T>(path: string, options?: RequestInit): Promise<T> {
  const response = await fetch(path, { cache: 'no-store', ...options })
  if (!response.ok) {
    throw new Error(`请求失败：${response.status}`)
  }
  return response.json() as Promise<T>
}

export function getSnapshot(signal?: AbortSignal): Promise<DashboardSnapshot> {
  return request<DashboardSnapshot>('/api/snapshot', { signal })
}

export function controlSimulation(action: 'pause' | 'resume' | 'reset'): Promise<{ ok: boolean }> {
  return request<{ ok: boolean }>(`/api/control/${action}`, { method: 'POST' })
}
