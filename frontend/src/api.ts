import type { DashboardSnapshot } from './types'

async function request<T>(path: string, options?: RequestInit): Promise<T> {
  const response = await fetch(path, { cache: 'no-store', ...options })
  if (!response.ok) {
    throw new Error(`请求失败：${response.status}`)
  }
  return response.json() as Promise<T>
}

export interface SnapshotStreamHandlers {
  onSnapshot: (data: DashboardSnapshot) => void
  onStatus: (connected: boolean) => void
}

export function openSnapshotStream(handlers: SnapshotStreamHandlers): EventSource {
  const source = new EventSource('/api/stream')
  source.onopen = () => handlers.onStatus(true)
  source.onmessage = (event) => {
    try {
      handlers.onSnapshot(JSON.parse(event.data as string) as DashboardSnapshot)
    } catch {
      // 忽略不完整的数据帧，等待下一帧
    }
  }
  source.onerror = () => handlers.onStatus(false)
  return source
}

export function controlSimulation(action: 'pause' | 'resume' | 'reset'): Promise<{ ok: boolean }> {
  return request<{ ok: boolean }>(`/api/control/${action}`, { method: 'POST' })
}
