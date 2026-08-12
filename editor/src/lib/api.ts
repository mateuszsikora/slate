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

export interface Binding {
  provider: string
  resource: string
}

/** The provider-neutral discovery vocabulary from design.md §5.2. */
export interface Resource {
  provider: string
  resource: string
  kind: string
  name?: string
  area?: string
  available: boolean
  state: Record<string, unknown>
  capabilities?: Record<string, unknown>
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
  binding?: Binding
  bindings?: Binding[]
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
  readonly body: unknown

  constructor(status: number, code: string, message?: string, body?: unknown) {
    super(message ?? `${status} ${code}`)
    this.name = 'ApiError'
    this.status = status
    this.code = code
    this.body = body
  }

  /** Whether the device refused the token rather than the request. */
  get isUnauthorized(): boolean {
    return this.status === 401
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

/* Formatting the 3.75 MB LittleFS partition is intentionally synchronous: a
 * 204 means the destructive work finished. It can legitimately take longer
 * than an ordinary LAN request without meaning the panel is unreachable. */
const FACTORY_RESET_TIMEOUT_MS = 30000

export class DeviceClient {
  private readonly origin: string
  private readonly token: string | null
  private readonly timeoutMs: number

  constructor(options: ClientOptions = {}) {
    this.origin = options.origin ?? ''
    this.token = options.token ?? null
    this.timeoutMs = options.timeoutMs ?? DEFAULT_TIMEOUT_MS
  }

  private url(path: string): string {
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

  /** `POST /config/validate` — checks a complete document without changing the panel. */
  validateConfig(document: string): Promise<void> {
    return this.request<void>('POST', '/config/validate', { body: document })
  }

  /** Persistent `PUT /config`; unlike live preview this writes the document to flash. */
  publishConfig(document: string): Promise<void> {
    return this.request<void>('PUT', '/config', { body: document })
  }

  /** RAM-only replacement used by §10's live panel preview. */
  previewConfig(document: string): Promise<void> {
    return this.request<void>('PUT', '/config?transient=1', { body: document })
  }

  /** Device-wide mode control for scripts and clients that do not own a WebSocket. */
  setMode(mode: 'normal' | 'edit'): Promise<void> {
    return this.request<void>('POST', '/mode', { body: JSON.stringify({ mode }) })
  }

  /** Flash the panel so several devices can be told apart. */
  identify(): Promise<void> {
    return this.request<void>('POST', '/identify')
  }

  /** Wipe NVS and LittleFS. A successful response is followed by a reboot. */
  factoryReset(): Promise<void> {
    return this.request<void>('POST', '/factory_reset', {
      timeoutMs: FACTORY_RESET_TIMEOUT_MS,
    })
  }

  resources(provider: string): Promise<Resource[]> {
    return this.request<{ resources: Resource[] }>(
      'GET',
      `/resources?provider=${encodeURIComponent(provider)}`,
    ).then((response) => response.resources)
  }

  private async request<T>(
    method: string,
    path: string,
    options: { authenticated?: boolean; body?: string; timeoutMs?: number } = {},
  ): Promise<T> {
    const headers: Record<string, string> = {}
    if (options.authenticated !== false && this.token !== null) {
      headers['Authorization'] = `Bearer ${this.token}`
    }
    if (options.body !== undefined) {
      headers['Content-Type'] = 'application/json; charset=utf-8'
    }

    const controller = new AbortController()
    const timer = window.setTimeout(() => controller.abort(), options.timeoutMs ?? this.timeoutMs)
    let response: Response
    try {
      response = await fetch(this.url(path), {
        method,
        headers,
        body: options.body,
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
      const body = await errorBody(response)
      const code = isObject(body) && typeof body['error'] === 'string' ? body['error'] : 'unknown'
      throw new ApiError(response.status, code, undefined, body)
    }
    if (response.status === 204) {
      return undefined as T
    }
    return (await response.json()) as T
  }
}

async function errorBody(response: Response): Promise<unknown> {
  try {
    return (await response.json()) as unknown
  } catch {
    /* §4 promises `{"error": …}` on every failure, but a proxy or a truncated
     * response is not the device and does not owe us the shape. */
  }
  return null
}

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
}
