import assert from 'node:assert/strict'
import test from 'node:test'

import { firstFreeSlot } from '../src/lib/bar.ts'

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
