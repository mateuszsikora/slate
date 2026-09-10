/*
 * The device API, contract v1 (DESIGN.md §4.1).
 *
 * ADR-3 and ADR-4: the browser talks to the device and to nothing else, over
 * the same documented API any other client uses. There is no backend here to
 * hide a fetch behind, so this file is the whole client.
 */

import { assembleHaResources, type HaCatalogPayloads } from './ha.ts'

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
  authentication: 'open' | 'pin'
  network: NetworkState
}

export interface DeviceSession {
  token: string
}

export interface ProviderStatus {
  id: string
  status: string
  resource_count: number
  reason?: string
}

export interface HaConfiguration {
  configured: boolean
  url: string | null
}

export interface HaDiscoveredInstance {
  name: string
  uuid: string
  url: string
}

export interface TuyaConfiguration {
  configured: boolean
  region: string | null
  uid: string | null
}

export interface IntegrationKey {
  id: string
  name: string
}

export interface CreatedIntegrationKey extends IntegrationKey {
  token: string
}

type HaCatalogStage = keyof HaCatalogPayloads

interface HaCatalogStarted {
  request: number
}

interface HaCatalogPending {
  status: 'pending'
}

interface HaCatalogResult {
  type: 'result'
  success: boolean
  result?: unknown
  error?: unknown
}

export interface Binding {
  provider: string
  resource: string
}

/** The provider-neutral discovery vocabulary from DESIGN.md §5.2. */
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

export type CatalogState = 'empty' | 'loading' | 'ready' | 'error'

export interface ResourceCatalog {
  resources: Resource[]
  /** Absent on older firmware; normalized to `ready` by the client. */
  catalog_state: CatalogState
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

/**
 * `GET /update` — §11.4's release channel as the panel sees it.
 *
 * `available` is an offer and nothing more: the panel has downloaded a few
 * hundred bytes of manifest and compared them against itself. Nothing is
 * installed, and nothing reboots, until `POST /update/install`.
 */
export interface UpdateStatus {
  current: string
  manifest_url: string | null
  /**
   * Whether the panel looks on its own. False on a build with no channel, and
   * false on a panel that has one and was told to stop — `manifest_url` is what
   * separates those two, and only the second can be changed from here.
   */
  scheduled: boolean
  state: 'idle' | 'checking' | 'downloading' | 'installed'
  checked_s_ago: number | null
  available: { version: string; url: string; sha256: string } | null
  error: string | null
  progress: { received: number; total: number } | null
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

export interface BarItem {
  type: string
  slot: number
  span: number
  provider?: string
  resource?: string
  label?: string
}

export interface Config {
  schema: number
  theme?: string
  home_page?: string
  settings?: Record<string, unknown>
  bar?: BarItem[]
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

  /** Whether the device refused the current session credential. */
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

/* Credential testing includes a WebSocket connection to Home Assistant. */
const HA_CONFIGURATION_TIMEOUT_MS = 20000

/* Tuya credential testing is a signed round trip to the Tuya cloud. */
const TUYA_CONFIGURATION_TIMEOUT_MS = 20000

/* Registry data changes rarely, while one real catalog is hundreds of KB. A
 * picker gets the session cache immediately and at most starts one refresh in
 * the background after this age; switching tiles never waits on duplicate HA
 * requests. Reloading the editor naturally drops this in-memory cache. */
const HA_CATALOG_REFRESH_MS = 5 * 60 * 1000

interface HaCatalogCacheEntry {
  resources?: Resource[]
  fetchedAt: number
  refresh?: Promise<Resource[]>
}

const haCatalogCache = new Map<string, HaCatalogCacheEntry>()

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

  /** `GET /info` — public metadata used before a browser session exists. */
  info(): Promise<DeviceInfo> {
    return this.request<DeviceInfo>('GET', '/info', { authenticated: false })
  }

  /** Start a browser session. The device token stays an internal transport credential. */
  session(pin?: string): Promise<DeviceSession> {
    return this.request<DeviceSession>('POST', '/session', {
      authenticated: false,
      body: pin === undefined ? undefined : JSON.stringify({ pin }),
    })
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

  /** `GET /update` — what is running, what is offered, and what the last job did. */
  update(): Promise<UpdateStatus> {
    return this.request<UpdateStatus>('GET', '/update')
  }

  /** Ask for a check now rather than waiting for the daily one. Answers before it runs. */
  checkForUpdate(): Promise<void> {
    return this.request<void>('POST', '/update/check')
  }

  /**
   * Install the offered release, naming it. The panel refuses a version other
   * than the one it is offering, so the release somebody accepted is the
   * release that gets installed even if the channel moved in between.
   */
  installUpdate(version: string): Promise<void> {
    return this.request<void>('POST', '/update/install', { body: JSON.stringify({ version }) })
  }

  /**
   * Turn the daily check on or off. This is the only one of the four that
   * changes what the panel does when nobody is looking at it — and the only
   * way to stop the one request it makes outside the LAN without rebuilding
   * the firmware.
   */
  setUpdateSchedule(scheduled: boolean): Promise<void> {
    return this.request<void>('POST', '/update/settings', {
      body: JSON.stringify({ scheduled }),
    })
  }

  resources(provider: string): Promise<ResourceCatalog> {
    if (provider === 'ha') {
      return this.homeAssistantResources().then((resources) => ({
        resources,
        catalog_state: 'ready',
      }))
    }
    return this.request<unknown>('GET', `/resources?provider=${encodeURIComponent(provider)}`).then(
      normalizeResourceCatalog,
    )
  }

  haConfiguration(): Promise<HaConfiguration> {
    return this.request<HaConfiguration>('GET', '/ha')
  }

  discoverHomeAssistant(): Promise<HaDiscoveredInstance[]> {
    return this.request<{ instances: HaDiscoveredInstance[] }>('GET', '/ha/discover').then(
      (response) => response.instances,
    )
  }

  configureHomeAssistant(url: string, token: string): Promise<void> {
    return this.request<void>('POST', '/ha', {
      body: JSON.stringify({ url, token }),
      timeoutMs: HA_CONFIGURATION_TIMEOUT_MS,
    }).then(() => this.invalidateHomeAssistantCatalog())
  }

  disconnectHomeAssistant(): Promise<void> {
    return this.request<void>('DELETE', '/ha').then(() => this.invalidateHomeAssistantCatalog())
  }

  tuyaConfiguration(): Promise<TuyaConfiguration> {
    return this.request<TuyaConfiguration>('GET', '/tuya')
  }

  configureTuya(region: string, accessId: string, secret: string, uid: string): Promise<void> {
    return this.request<void>('POST', '/tuya', {
      body: JSON.stringify({ region, access_id: accessId, secret, uid }),
      timeoutMs: TUYA_CONFIGURATION_TIMEOUT_MS,
    })
  }

  disconnectTuya(): Promise<void> {
    return this.request<void>('DELETE', '/tuya')
  }

  private homeAssistantResources(): Promise<Resource[]> {
    const key = `${this.origin}\n${this.token ?? ''}`
    let cached = haCatalogCache.get(key)
    if (cached === undefined) {
      cached = { fetchedAt: 0 }
      haCatalogCache.set(key, cached)
    }

    if (cached.resources !== undefined) {
      if (Date.now() - cached.fetchedAt >= HA_CATALOG_REFRESH_MS && cached.refresh === undefined) {
        void this.refreshHomeAssistantResources(key, cached).catch(() => undefined)
      }
      return Promise.resolve(cached.resources)
    }
    return cached.refresh ?? this.refreshHomeAssistantResources(key, cached)
  }

  private async refreshHomeAssistantResources(
    key: string,
    cached: HaCatalogCacheEntry,
  ): Promise<Resource[]> {
    const refresh = this.fetchHomeAssistantResources()
      .then((resources) => {
        if (haCatalogCache.get(key) === cached) {
          cached.resources = resources
          cached.fetchedAt = Date.now()
          cached.refresh = undefined
        }
        return resources
      })
      .catch((error: unknown) => {
        if (haCatalogCache.get(key) === cached) cached.refresh = undefined
        throw error
      })
    cached.refresh = refresh
    return refresh
  }

  private async fetchHomeAssistantResources(): Promise<Resource[]> {
    const stages: HaCatalogStage[] = ['entities', 'devices', 'areas', 'states']
    const payloads: Partial<HaCatalogPayloads> = {}
    for (const stage of stages) {
      try {
        payloads[stage] = await this.homeAssistantCatalogStage(stage)
      } catch (error) {
        if (stage === 'states') throw error
        payloads[stage] = stage === 'entities' ? { entities: [] } : []
      }
    }
    return assembleHaResources(payloads as HaCatalogPayloads)
  }

  private invalidateHomeAssistantCatalog(): void {
    haCatalogCache.delete(`${this.origin}\n${this.token ?? ''}`)
  }

  private async homeAssistantCatalogStage(stage: HaCatalogStage): Promise<unknown> {
    const started = await this.request<HaCatalogStarted>('POST', '/ha/catalog', {
      body: JSON.stringify({ stage }),
    })
    for (let attempt = 0; attempt < 150; attempt++) {
      const response = await this.request<HaCatalogPending | HaCatalogResult>(
        'GET',
        `/ha/catalog?request=${encodeURIComponent(started.request)}`,
      )
      if ('status' in response && response.status === 'pending') {
        await delay(200)
        continue
      }
      if ('type' in response && response.type === 'result' && response.success) {
        return response.result
      }
      throw new ApiError(
        502,
        'ha_catalog_failed',
        undefined,
        'error' in response ? response.error : response,
      )
    }
    throw new ApiError(504, 'catalog_timeout')
  }

  integrationKeys(): Promise<IntegrationKey[]> {
    return this.request<{ keys: IntegrationKey[] }>('GET', '/integration-keys').then(
      (response) => response.keys,
    )
  }

  createIntegrationKey(name: string): Promise<CreatedIntegrationKey> {
    return this.request<CreatedIntegrationKey>('POST', '/integration-keys', {
      body: JSON.stringify({ name }),
    })
  }

  revokeIntegrationKey(id: string): Promise<void> {
    return this.request<void>(
      'DELETE',
      `/integration-keys?id=${encodeURIComponent(id)}`,
    )
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

function delay(milliseconds: number): Promise<void> {
  return new Promise((resolve) => window.setTimeout(resolve, milliseconds))
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

/** Accept the pre-catalog-state response while rejecting a broken new response. */
export function normalizeResourceCatalog(value: unknown): ResourceCatalog {
  if (!isObject(value) || !Array.isArray(value['resources'])) {
    throw new ApiError(502, 'malformed_catalog')
  }
  const rawState = value['catalog_state']
  const catalogState = rawState === undefined ? 'ready' : rawState
  if (
    catalogState !== 'empty' &&
    catalogState !== 'loading' &&
    catalogState !== 'ready' &&
    catalogState !== 'error'
  ) {
    throw new ApiError(502, 'malformed_catalog')
  }
  return { resources: value['resources'] as Resource[], catalog_state: catalogState }
}
