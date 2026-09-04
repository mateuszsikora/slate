import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'

import type { UpdateStatus } from '../src/lib/api.ts'
import {
  UPDATE_MESSAGES,
  checkedAgo,
  updateError,
  updateHeadline,
  updatePercent,
} from '../src/lib/update.ts'

const IDLE: UpdateStatus = {
  current: '1.0.0',
  manifest_url: 'https://example.invalid/ota/manifest.json',
  state: 'idle',
  checked_s_ago: 60,
  available: null,
  error: null,
  progress: null,
}

test('an offer and a current panel read differently', () => {
  assert.equal(updateHeadline(IDLE), 'Slate 1.0.0 is the current release.')
  assert.equal(
    updateHeadline({
      ...IDLE,
      available: { version: '1.1.0', url: 'https://example.invalid/f.bin', sha256: 'a'.repeat(64) },
    }),
    'Slate 1.1.0 is available.',
  )
})

test('progress is bounded and survives a server that announced nothing', () => {
  assert.equal(updatePercent(IDLE), 0)
  assert.equal(updatePercent({ ...IDLE, progress: { received: 10, total: 0 } }), 0)
  assert.equal(updatePercent({ ...IDLE, progress: { received: 500, total: 1000 } }), 50)
  assert.equal(updatePercent({ ...IDLE, progress: { received: 2000, total: 1000 } }), 100)
})

test('how long ago the panel looked is coarse', () => {
  assert.equal(checkedAgo(5), 'just now')
  assert.equal(checkedAgo(600), '10 min ago')
  assert.equal(checkedAgo(7200), '2 h ago')
  assert.equal(checkedAgo(86400 * 3), '3 days ago')
})

test('an unrecognised code is shown rather than swallowed', () => {
  assert.equal(updateError('something_new'), 'The last update job failed: something_new.')
  assert.match(updateError('checksum_mismatch'), /checksum/)
})

/*
 * The device returns codes and no presentation text (ADR-4), which only works
 * while both halves agree on the vocabulary. A code the firmware can report and
 * the editor has never heard of would reach a person as the raw identifier.
 */
test('every failure the update component can report has a sentence', () => {
  const source = readFileSync(
    new URL('../../firmware/components/slate_update/slate_update.c', import.meta.url),
    'utf8',
  )
  /*
   * A code can leave that component through any of these shapes, and the scan
   * has already been narrower than the component twice: once missing the route
   * refusals, once missing the argument that names what a 404 meant. So each
   * shape has to find something. A pattern that silently matches nothing is the
   * same failure as a missing sentence, one level up, and it passes quietly.
   */
  const shapes: [string, RegExp][] = [
    ["the worker's fail()", /fail\("([a-z_]+)"\)/g],
    ['a step assigning to `error`', /\*?error = "([a-z_]+)"/g],
    ['the arms of ota_error_code()', /return "([a-z_]+)";/g],
    ['a route refusal', /slate_api_refuse\(req,\s*"[^"]+",\s*"([a-z_]+)"/g],
    ['what a 404 was called', /open_with_redirects\([^)]*?"([a-z_]+)"/g],
  ]

  const codes = new Set<string>()
  for (const [what, pattern] of shapes) {
    const found = [...source.matchAll(pattern)].map((match) => match[1])
    assert.ok(found.length > 0, `the scan for ${what} matched nothing`)
    found.forEach((code) => codes.add(code))
  }

  assert.ok(codes.has('checksum_mismatch'), 'the scan found the component')
  for (const code of codes) {
    assert.ok(code in UPDATE_MESSAGES, `no editor message for ${code}`)
  }
})
