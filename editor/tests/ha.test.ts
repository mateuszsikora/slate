import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'

import { assembleHaResources } from '../src/lib/ha.ts'

/** The four registry stages of §5.7, with only the fields the join reads. */
function catalog(states: unknown[]) {
  return { entities: { entities: [] }, devices: [], areas: [], states }
}

function state(entityId: string, rawState: string, attributes: Record<string, unknown> = {}) {
  return { entity_id: entityId, state: rawState, attributes }
}

test('a binary_sensor is offered to the picker as a sensor', () => {
  const resources = assembleHaResources(
    catalog([state('binary_sensor.front_door', 'on', { friendly_name: 'Front door' })]),
  )

  assert.deepEqual(resources, [
    {
      provider: 'ha',
      resource: 'binary_sensor.front_door',
      kind: 'sensor',
      name: 'Front door',
      area: undefined,
      available: true,
      state: {},
    },
  ])
})

test('a binary_sensor is available only when it answers on or off', () => {
  const available = (rawState: string) =>
    assembleHaResources(catalog([state('binary_sensor.front_door', rawState)]))[0]?.available

  assert.equal(available('on'), true)
  assert.equal(available('off'), true)
  assert.equal(available('unknown'), false)
  assert.equal(available('unavailable'), false)
})

test('a plain sensor still reads unknown as a value it has', () => {
  // The rule is the domain's, not the kind's: both are `sensor` here, and only
  // one of them treats `unknown` as an answer.
  const resources = assembleHaResources(
    catalog([
      state('sensor.air_quality', 'unknown'),
      state('sensor.room_temperature', 'unavailable'),
    ]),
  )

  assert.equal(resources[0]?.available, true)
  assert.equal(resources[1]?.available, false)
})

test('the documented binary_sensor phrasings are exactly the firmware ones', () => {
  // The words are a product decision that lives in two places: the adapter
  // table the panel reads from, and the table in CONFIGURATION.md a user reads
  // from. Nothing in either file would notice the two drifting apart, and a
  // documented word the firmware does not say is worse than no table at all.
  // Same trick as the timezone list in settings.test.ts.
  const source = readFileSync(
    new URL('../../firmware/components/slate_ha/slate_ha_entities.c', import.meta.url),
    'utf8',
  )
  const table = source.slice(
    source.indexOf('k_binary_phrasings[] = {'),
    source.indexOf('};', source.indexOf('k_binary_phrasings[] = {')),
  )
  const firmware = [...table.matchAll(/\{"([^"]+)", "([^"]+)", "([^"]+)"\}/g)].map(
    (m) => [m[1], m[2], m[3]] as const,
  )
  assert.ok(firmware.length > 20, `parsed only ${firmware.length} rows from the adapter`)

  const doc = readFileSync(new URL('../../docs/CONFIGURATION.md', import.meta.url), 'utf8')
  // Only the phrasing table: the file holds other three-column tables whose
  // first column is also a backticked name.
  const heading = doc.indexOf('| `device_class` | on | off |')
  assert.notEqual(heading, -1, 'the phrasing table is gone from CONFIGURATION.md')
  const section = doc
    .slice(heading, doc.indexOf('\n\n', heading))
    .split('\n')
    .slice(2) // the header row and the |---| rule under it
    .join('\n')

  const documented = new Map<string, readonly [string, string]>()
  for (const row of section.matchAll(/^\| (`[^|]+`) \| ([^|]+?) \| ([^|]+?) \|$/gm)) {
    const [, classes, on, off] = row
    for (const name of classes.matchAll(/`([a-z_]+)`/g)) {
      documented.set(name[1], [on.trim(), off.trim()])
    }
  }

  for (const [deviceClass, on, off] of firmware) {
    assert.deepEqual(
      documented.get(deviceClass),
      [on, off],
      `CONFIGURATION.md disagrees with the adapter about ${deviceClass}`,
    )
  }
  assert.deepEqual(
    [...documented.keys()].sort(),
    firmware.map(([c]) => c).sort(),
    'CONFIGURATION.md documents a device_class the adapter does not know',
  )
})

test('the documented device_class icons are exactly the firmware ones', () => {
  // Same reason as the phrasings above: which glyph a contact gets is a product
  // decision written down twice, and a table that quietly drifts is worse than
  // no table. The doc names the icon in English rather than by enum member, so
  // what is compared is the grouping — which classes are documented, and which
  // of them the adapter puts on the same icon.
  const source = readFileSync(
    new URL('../../firmware/components/slate_ha/slate_ha_entities.c', import.meta.url),
    'utf8',
  )
  const start = source.indexOf('k_categories[] = {')
  const table = source.slice(start, source.indexOf('};', start))
  const firmware = new Map<string, string>()
  for (const row of table.matchAll(/\{"([a-z0-9_]+)", (SLATE_CATEGORY_[A-Z_]+)\}/g)) {
    firmware.set(row[1], row[2])
  }
  assert.ok(firmware.size > 40, `parsed only ${firmware.size} rows from the adapter`)

  const doc = readFileSync(new URL('../../docs/CONFIGURATION.md', import.meta.url), 'utf8')
  const heading = doc.indexOf('| `device_class` | Icon |')
  assert.notEqual(heading, -1, 'the icon table is gone from CONFIGURATION.md')
  const section = doc.slice(heading, doc.indexOf('\n\n', heading)).split('\n').slice(2)

  const documented: string[][] = []
  for (const line of section) {
    const classes = [...line.matchAll(/`([a-z0-9_]+)`/g)].map((match) => match[1])
    if (classes.length > 0) documented.push(classes)
  }

  assert.deepEqual(
    documented.flat().sort(),
    [...firmware.keys()].sort(),
    'CONFIGURATION.md and the adapter disagree about which classes have an icon',
  )
  const seen = new Map<string, string[]>()
  for (const row of documented) {
    const categories = new Set(row.map((name) => firmware.get(name)))
    assert.equal(
      categories.size,
      1,
      `CONFIGURATION.md puts ${row.join(', ')} on one icon, the adapter does not`,
    )
    const [category] = categories
    const earlier = seen.get(category!)
    assert.equal(earlier, undefined, `${category} is documented twice: ${earlier?.join(', ')}`)
    seen.set(category!, row)
  }
})

test('the picker still refuses a domain no component can render', () => {
  const resources = assembleHaResources(
    catalog([
      state('automation.morning', 'on'),
      state('light.kitchen', 'on'),
      state('cover.office_blind', 'opening'),
      state('scene.relax', '2026-08-12T12:00:00.000000+00:00'),
    ]),
  )

  assert.deepEqual(
    resources.map((resource) => resource.resource),
    ['light.kitchen', 'cover.office_blind', 'scene.relax'],
  )
})
