import assert from 'node:assert/strict'
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
