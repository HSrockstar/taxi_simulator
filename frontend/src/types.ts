export type DriverState = 'IDLE' | 'SERVING' | 'REBALANCING'

export interface DriverSnapshot {
  id: number
  x: number
  y: number
  rating: number
  state: DriverState
}

export interface MetricsSnapshot {
  queueLength: number
  generated: number
  matched: number
  cancelled: number
  completed: number
  successRate: number
  totalMatchMicros: number
  averageMatchMicros: number
}

export interface LogEntry {
  sequence: number
  message: string
}

export interface DashboardSnapshot {
  tick: number
  paused: boolean
  hotspotIndex: number
  pending: number[]
  idle: number[]
  drivers: DriverSnapshot[]
  metrics: MetricsSnapshot
  logs: LogEntry[]
}

export interface TrendPoint {
  tick: number
  queueLength: number
  successRate: number
}
