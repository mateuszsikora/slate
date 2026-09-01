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

    const kind = resourceKind(resource)
    if (kind === undefined) continue

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
      available: resourceAvailable(kind, rawState),
      state: {},
    })
  }

  return resources
}

function resourceKind(resource: string): Resource['kind'] | undefined {
  const dot = resource.indexOf('.')
  if (dot <= 0 || dot === resource.length - 1) return undefined
  const domain = resource.slice(0, dot)
  return domain === 'light' || domain === 'cover' || domain === 'sensor' || domain === 'scene'
    ? domain
    : undefined
}

function resourceAvailable(kind: string, state: string): boolean {
  if (kind === 'light') return state === 'on' || state === 'off'
  if (kind === 'cover') {
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
