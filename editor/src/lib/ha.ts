import type { Resource } from './api'

export interface HaCatalogPayloads {
  entities: unknown
  devices: unknown
  areas: unknown
  states: unknown
}

interface EntityMeta {
  device: string
  area: string
}

/** Join Home Assistant's configuration catalog in browser memory, not on ESP32. */
export function assembleHaResources(payloads: HaCatalogPayloads): Resource[] {
  const entities = new Map<string, EntityMeta>()
  const entityResult = record(payloads.entities)
  for (const value of array(entityResult?.['entities'])) {
    const item = record(value)
    const id = text(item?.['ei']) ?? text(item?.['entity_id'])
    if (id !== undefined) {
      entities.set(id, {
        device: text(item?.['di']) ?? text(item?.['device_id']) ?? '',
        area: text(item?.['ai']) ?? text(item?.['area_id']) ?? '',
      })
    }
  }

  const devices = new Map<string, string>()
  for (const value of array(payloads.devices)) {
    const item = record(value)
    const id = text(item?.['id'])
    if (id !== undefined) devices.set(id, text(item?.['area_id']) ?? '')
  }

  const areas = new Map<string, string>()
  for (const value of array(payloads.areas)) {
    const item = record(value)
    const id = text(item?.['area_id'])
    const name = text(item?.['name'])
    if (id !== undefined && name !== undefined) areas.set(id, name)
  }

  const resources: Resource[] = []
  for (const value of array(payloads.states)) {
    const item = record(value)
    const resource = text(item?.['entity_id'])
    const rawState = text(item?.['state'])
    if (resource === undefined || rawState === undefined) continue

    const domain = resourceDomain(resource)
    const kind = domain === undefined ? undefined : resourceKind(domain)
    if (domain === undefined || kind === undefined) continue

    const attributes = record(item?.['attributes'])
    const meta = entities.get(resource)
    const areaId = meta?.area || (meta?.device ? devices.get(meta.device) : '') || ''
    const area = areaId ? areas.get(areaId) : undefined
    const name = text(attributes?.['friendly_name'])

    resources.push({
      provider: 'ha',
      resource,
      kind,
      name,
      area,
      available: resourceAvailable(domain, rawState),
      state: {},
    })
  }

  return resources
}

function resourceDomain(resource: string): string | undefined {
  const dot = resource.indexOf('.')
  if (dot <= 0 || dot === resource.length - 1) return undefined
  return resource.slice(0, dot)
}

/** The same four kinds the firmware adapter publishes — see DESIGN.md §5.6. */
function resourceKind(domain: string): Resource['kind'] | undefined {
  if (domain === 'light' || domain === 'cover' || domain === 'sensor' || domain === 'scene') {
    return domain
  }
  // A binary_sensor is a read-only sensor whose value is a word, not a light:
  // §5.2 ties `light` to a toggle a door contact cannot honour.
  return domain === 'binary_sensor' ? 'sensor' : undefined
}

/**
 * Keyed on the domain rather than the kind, because two domains now share
 * `sensor` and they do not share a rule: a `sensor` may legitimately read
 * `unknown`, while a `binary_sensor` that says so has not answered.
 */
function resourceAvailable(domain: string, state: string): boolean {
  if (domain === 'light' || domain === 'binary_sensor') return state === 'on' || state === 'off'
  if (domain === 'cover') {
    return ['open', 'closed', 'opening', 'closing', 'stopped'].includes(state)
  }
  return state !== 'unavailable'
}

function record(value: unknown): Record<string, unknown> | undefined {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
    ? (value as Record<string, unknown>)
    : undefined
}

function array(value: unknown): unknown[] {
  return Array.isArray(value) ? value : []
}

function text(value: unknown): string | undefined {
  return typeof value === 'string' && value !== '' ? value : undefined
}
