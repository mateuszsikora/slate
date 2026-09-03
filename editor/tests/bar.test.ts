import assert from 'node:assert/strict'
import test from 'node:test'

import { barErrors, firstFreeSlot, isBoundBarItem } from '../src/lib/bar.ts'

test('finds the first contiguous range for a new bar item', () => {
  const bar = [
    { type: 'title', slot: 0, span: 1 },
    { type: 'badge', slot: 2, span: 1, provider: 'direct' },
  ]

  assert.equal(firstFreeSlot(bar, 1), 1)
  assert.equal(firstFreeSlot(bar, 2), 3)
})

test('bounds malformed item geometry to the twelve-slot bar', () => {
  assert.equal(firstFreeSlot([{ type: 'clock', slot: 0, span: 1_000_000_000 }]), null)
  assert.equal(firstFreeSlot([{ type: 'clock', slot: 0, span: Number.POSITIVE_INFINITY }]), 0)
  assert.equal(firstFreeSlot([{ type: 'clock', slot: Number.NaN, span: 2 }], 2), 0)
})

test('a bar item bound to a resource needs both halves of the binding', () => {
  const complete = { type: 'sensor', slot: 0, span: 2, provider: 'ha', resource: 'sensor.hall' }

  assert.ok(isBoundBarItem(complete))
  assert.deepEqual(barErrors([complete]), [])
  assert.deepEqual(barErrors([{ ...complete, provider: '' }]), ['Item 1 needs a provider.'])
  assert.deepEqual(barErrors([{ ...complete, resource: '' }]), ['Item 1 needs a resource.'])
})

test('a bound bar item is refused in one slot and a scene is refused outright', () => {
  const narrow = { type: 'light', slot: 0, span: 1, provider: 'ha', resource: 'light.hall' }
  const scene = { type: 'scene', slot: 0, span: 2, provider: 'ha', resource: 'scene.relax' }

  assert.deepEqual(barErrors([narrow]), [
    'Item 1 needs at least 2 slots to show a value and a name.',
  ])
  assert.deepEqual(barErrors([scene]), [
    'Item 1 cannot be a scene: scenes have no state to show.',
  ])
  assert.equal(isBoundBarItem(scene), false)
})

test('unbound items keep their existing rules', () => {
  assert.deepEqual(barErrors([{ type: 'clock', slot: 0, span: 1 }]), [])
  assert.deepEqual(barErrors([{ type: 'badge', slot: 0, span: 2 }]), ['Item 1 needs a provider.'])
})
