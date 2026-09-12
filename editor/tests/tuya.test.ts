import assert from 'node:assert/strict'
import test from 'node:test'

import { ApiError, normalizeResourceCatalog } from '../src/lib/api.ts'
import { providerReason } from '../src/lib/providers.ts'

test('a legacy resource response is accepted as a ready catalog', () => {
  assert.deepEqual(normalizeResourceCatalog({ resources: [] }), {
    resources: [],
    catalog_state: 'ready',
  })
})

test('all Tuya catalog states survive client parsing', () => {
  for (const catalog_state of ['empty', 'loading', 'ready', 'error'] as const) {
    assert.equal(normalizeResourceCatalog({ resources: [], catalog_state }).catalog_state, catalog_state)
  }
})

test('a malformed catalog does not masquerade as an empty account', () => {
  assert.throws(
    () => normalizeResourceCatalog({ resources: [], catalog_state: 'finished' }),
    (error: unknown) => error instanceof ApiError && error.code === 'malformed_catalog',
  )
  assert.throws(() => normalizeResourceCatalog({ catalog_state: 'ready' }), ApiError)
})

test('Tuya provider reasons are distinct and disappear on recovery', () => {
  assert.match(providerReason('auth')!, /credentials/)
  assert.match(providerReason('quota')!, /quota/)
  assert.notEqual(providerReason('auth'), providerReason('quota'))
  assert.equal(providerReason(undefined), null)
})
