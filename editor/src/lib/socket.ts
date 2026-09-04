/*
 * The device WebSocket, contract v1 (DESIGN.md §4.2).
 *
 * The upgrade carries no credential — the token would otherwise sit in the
 * browser's history and in every access log between here and the panel — so
 * the first text frame authenticates and nothing else is accepted before
 * `auth_ok`.
 */

import { API_BASE } from './api'

export type ConnectionState =
  | 'connecting'
  | 'online'
  /** The panel is not answering. §9.4's outage, seen from the other end. */
  | 'offline'
  /** The panel answered and refused the internal session credential. */
  | 'unauthorized'

export interface StatusFrame {
  providers: Record<string, string>
  wifi: number
  heap_free: number
  lvgl_heap_free: number
  lvgl_frag_pct: number
}

export interface LogFrame {
  level: string
  msg: string
}

export interface ReloadedFrame {
  schema: number
  tiles: number
}

export interface SocketHandlers {
  onState?: (state: ConnectionState) => void
  onStatus?: (status: StatusFrame) => void
  onLog?: (log: LogFrame) => void
  onReloaded?: (reloaded: ReloadedFrame) => void
}

/*
 * §9.4's reconnect ladder, in the browser as well as in the firmware, because
 * the two are recovering from the same outage and a page that gives up while
 * the panel is still retrying reports an outage that has ended.
 */
const BACKOFF_MS = [1000, 2000, 4000, 8000, 15000, 30000]

/*
 * §4.2 returns the panel to normal mode after 60 s without a client ping, and
 * the ping is what tells it a live editor still owns edit mode. Three of them
 * inside that window: one lost frame must not end a drag session.
 */
const PING_INTERVAL_MS = 20000

/*
 * The device closes an unauthenticated socket five seconds after the upgrade.
 * A connection that has neither authenticated nor been closed by then is one
 * the network is holding open somewhere in between, so this end stops waiting
 * at the same deadline rather than sitting in `connecting` forever.
 */
const AUTH_TIMEOUT_MS = 6000

export class DeviceSocket {
  private readonly url: string
  private readonly token: string
  private readonly handlers: SocketHandlers

  private socket: WebSocket | null = null
  private state: ConnectionState = 'connecting'
  private attempt = 0
  private pingTimer: number | null = null
  private authTimer: number | null = null
  private retryTimer: number | null = null
  private stopped = false
  /** The mode this client wants, re-sent after every reconnect. */
  private desiredMode: 'normal' | 'edit' = 'normal'

  constructor(origin: string, token: string, handlers: SocketHandlers) {
    const base = new URL(`${origin}${API_BASE}/ws`, window.location.href)
    base.protocol = base.protocol === 'https:' ? 'wss:' : 'ws:'
    this.url = base.toString()
    this.token = token
    this.handlers = handlers
  }

  start(): void {
    this.stopped = false
    this.open()
  }

  stop(): void {
    this.stopped = true
    this.clearTimers()
    if (this.retryTimer !== null) {
      window.clearTimeout(this.retryTimer)
      this.retryTimer = null
    }
    if (this.socket !== null) {
      const socket = this.socket
      this.socket = null
      socket.onclose = null
      socket.close()
    }
  }

  /**
   * Ask the panel to enter or leave edit mode (§6.5).
   *
   * Remembered rather than fired and forgotten: a reconnect has to restore the
   * mode, since the panel returned to normal the moment the pings stopped.
   */
  setMode(mode: 'normal' | 'edit'): void {
    this.desiredMode = mode
    this.send({ type: 'mode', mode })
  }

  private open(): void {
    this.report('connecting')

    let socket: WebSocket
    try {
      socket = new WebSocket(this.url)
    } catch {
      this.retry()
      return
    }
    this.socket = socket

    socket.onopen = () => {
      /* §4.2: within five seconds of the upgrade, and before anything else. */
      socket.send(JSON.stringify({ type: 'auth', token: this.token }))
      this.authTimer = window.setTimeout(() => {
        if (this.state !== 'online') {
          socket.close()
        }
      }, AUTH_TIMEOUT_MS)
    }

    socket.onmessage = (event) => {
      this.receive(event.data)
    }

    socket.onclose = () => {
      if (this.socket === socket) {
        this.socket = null
      }
      this.clearTimers()
      if (this.state !== 'unauthorized') {
        this.retry()
      }
    }

    /* `onerror` is followed by `onclose` in every browser that implements the
     * standard, so the reconnect lives there and this only avoids the console
     * treating a LAN outage as an unhandled event. */
    socket.onerror = () => {}
  }

  private receive(data: unknown): void {
    if (typeof data !== 'string') {
      return
    }

    let frame: { type?: unknown }
    try {
      frame = JSON.parse(data) as { type?: unknown }
    } catch {
      return
    }
    if (typeof frame.type !== 'string') {
      return
    }

    switch (frame.type) {
      case 'auth_ok':
        this.attempt = 0
        this.report('online')
        this.startPings()
        if (this.desiredMode !== 'normal') {
          this.send({ type: 'mode', mode: this.desiredMode })
        }
        break
      case 'auth_invalid':
        /* The panel is there and the token is not the one it holds. Retrying
         * would be a slow loop against a state only a person can change. */
        this.report('unauthorized')
        this.stop()
        break
      case 'status':
        this.handlers.onStatus?.(frame as unknown as StatusFrame)
        break
      case 'log':
        this.handlers.onLog?.(frame as unknown as LogFrame)
        break
      case 'reloaded':
        this.handlers.onReloaded?.(frame as unknown as ReloadedFrame)
        break
      default:
        /* §3.1's forward compatibility, applied to the event channel: a frame
         * this build does not know is a newer firmware, not an error. */
        break
    }
  }

  private send(payload: Record<string, unknown>): void {
    if (this.socket !== null && this.socket.readyState === WebSocket.OPEN) {
      this.socket.send(JSON.stringify(payload))
    }
  }

  private startPings(): void {
    this.stopPings()
    this.pingTimer = window.setInterval(() => {
      this.send({ type: 'ping' })
    }, PING_INTERVAL_MS)
  }

  private stopPings(): void {
    if (this.pingTimer !== null) {
      window.clearInterval(this.pingTimer)
      this.pingTimer = null
    }
  }

  private clearTimers(): void {
    this.stopPings()
    if (this.authTimer !== null) {
      window.clearTimeout(this.authTimer)
      this.authTimer = null
    }
  }

  private retry(): void {
    if (this.stopped) {
      return
    }
    this.report('offline')

    const delay = BACKOFF_MS[Math.min(this.attempt, BACKOFF_MS.length - 1)] ?? 30000
    this.attempt += 1
    if (this.retryTimer !== null) {
      window.clearTimeout(this.retryTimer)
    }
    this.retryTimer = window.setTimeout(() => {
      this.retryTimer = null
      if (!this.stopped) {
        this.open()
      }
    }, delay)
  }

  private report(state: ConnectionState): void {
    if (this.state === state) {
      return
    }
    this.state = state
    this.handlers.onState?.(state)
  }
}
