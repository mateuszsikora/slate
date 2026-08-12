/*
 * The device API, contract v1 (design.md §4.1).
 *
 * ADR-3 and ADR-4: the browser talks to the device and to nothing else, over
 * the same documented API any other client uses. There is no backend here to
 * hide a fetch behind, so this file is the whole client.
 */

export const API_BASE = '/api/v1'

export interface Ipv4Static {
  state: string
  address: string
  gateway: string
  dns: string[]
}

export interface NetworkState {
  mode: string
  ssid: string | null
  ip: string | null
  sta_ssid: string | null
  ipv4?: { mode: string; static?: Ipv4Static }
  last_error: string | null
}

export interface DeviceInfo {
  model: string
  firmware_version: string
  schema_max: number
  name: string
  themes: string[]
  pairing: string
  network: NetworkState
}

export interface ProviderStatus {
  id: string
  status: string
  resource_count: number
}

export interface DeviceStatus {
  network: NetworkState
  providers: ProviderStatus[]
  rssi: number
  uptime_s: number
  heap_free: number
  lvgl_heap_free: number
  lvgl_heap_total: number
  lvgl_frag_pct: number
  reset_reason: string
  reboot_count: number
  resource_count: number
  storage_reset: boolean
}

export interface Tile {
  id: string
  type: string
  pos: [number, number]
  size: [number, number]
  label?: string
  icon?: string
  binding?: { provider: string; resource: string }
  bindings?: { provider: string; resource: string }[]
}

export interface Page {
  id: string
  title?: string
  tiles: Tile[]
}

export interface Config {
  schema: number
  theme?: string
  home_page?: string
  settings?: Record<string, unknown>
  pages: Page[]
}

/**
 * A failed request, carrying §4's `error` string rather than a message this
 * client invented. `code` is `unauthorized`, `not_found`, `apply_failed` and
 * the rest of the vocabulary the device answers with; it is empty when the
 * failure never reached the device at all.
 */
export class ApiError extends Error {
  readonly status: number
  readonly code: string

  constructor(status: number, code: string, message?: string) {
    super(message ?? `${status} ${code}`)
    this.name = 'ApiError'
    this.status = status
    this.code = code
  }

  /** Whether the device refused the token rather than the request. */
  get isUnauthorized(): boolean {
    return this.status === 401
  }

  /** Whether the device was not reached at all — see §9.4's outage handling. */
  get isOffline(): boolean {
    return this.status === 0
  }
}

export interface ClientOptions {
  /** Origin to talk to. Empty means "the device that served this page". */
  origin?: string
  token?: string | null
  timeoutMs?: number
}

/**
 * The panel answers from a single-core HTTP server that is also driving a
 * display, and an unreachable device has to be told apart from a slow one
 * before a person gets bored. Five seconds is well past the slowest response
 * observed on a LAN and well short of the browser's own default.
 */
const DEFAULT_TIMEOUT_MS = 5000

export class DeviceClient {
  readonly origin: string
  private token: string | null
  private readonly timeoutMs: number

  constructor(options: ClientOptions = {}) {
    this.origin = options.origin ?? ''
    this.token = options.token ?? null
    this.timeoutMs = options.timeoutMs ?? DEFAULT_TIMEOUT_MS
  }

  withToken(token: string | null): DeviceClient {
    this.token = token
    return this
  }

  url(path: string): string {
    return `${this.origin}${API_BASE}${path}`
  }

  /** `GET /info` — the one route §4.1 serves without a token. */
  info(): Promise<DeviceInfo> {
    return this.request<DeviceInfo>('GET', '/info', { authenticated: false })
  }

  status(): Promise<DeviceStatus> {
    return this.request<DeviceStatus>('GET', '/status')
  }

  /** `GET /config`. Null when the panel has none — §6.5's error mode, not a fault. */
  async config(): Promise<Config | null> {
    try {
      return await this.request<Config>('GET', '/config')
    } catch (error) {
      if (error instanceof ApiError && error.status === 404) {
        return null
      }
      throw error
    }
  }

  private async request<T>(
    method: string,
    path: string,
    options: { authenticated?: boolean } = {},
  ): Promise<T> {
    const headers: Record<string, string> = {}
    if (options.authenticated !== false && this.token !== null) {
      headers['Authorization'] = `Bearer ${this.token}`
    }

    const controller = new AbortController()
    const timer = window.setTimeout(() => controller.abort(), this.timeoutMs)
    let response: Response
    try {
      response = await fetch(this.url(path), {
        method,
        headers,
        signal: controller.signal,
        cache: 'no-store',
      })
    } catch {
      /* A refused connection, a DNS failure and an abort are one condition to
       * the editor: the panel is not answering at this address. Which of them
       * it was is not something fetch() is willing to say. */
      throw new ApiError(0, 'unreachable', `${method} ${path}: the device did not answer`)
    } finally {
      window.clearTimeout(timer)
    }

    if (!response.ok) {
      throw new ApiError(response.status, await errorCode(response))
    }
    if (response.status === 204) {
      return undefined as T
    }
    return (await response.json()) as T
  }
}

async function errorCode(response: Response): Promise<string> {
  try {
    const body = (await response.json()) as { error?: unknown }
    if (typeof body.error === 'string') {
      return body.error
    }
  } catch {
    /* §4 promises `{"error": …}` on every failure, but a proxy or a truncated
     * response is not the device and does not owe us the shape. */
  }
  return 'unknown'
}
