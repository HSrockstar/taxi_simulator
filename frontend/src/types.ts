export type DriverState = 'IDLE' | 'EN_ROUTE' | 'ON_TRIP' | 'REBALANCING'

export interface DriverSnapshot {
  id: number
  x: number
  y: number
  targetX: number
  targetY: number
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

export interface ParamsSnapshot {
  driverCount: number
  orderRateMin: number
  orderRateMax: number
  matchRadius: number
  rebalanceRadius: number
  imbalanceThreshold: number
  orderTimeout: number
}

export interface DashboardSnapshot {
  tick: number
  paused: boolean
  hotspotIndex: number
  pending: number[]
  idle: number[]
  drivers: DriverSnapshot[]
  metrics: MetricsSnapshot
  params: ParamsSnapshot
  logs: LogEntry[]
}

export interface TrendPoint {
  tick: number
  queueLength: number
  successRate: number
}
