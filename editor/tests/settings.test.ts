import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'

import { TIMEZONES } from '../src/generated/timezones.ts'
import { timezoneFor, withTimezone } from '../src/lib/settings.ts'

test('changing timezone preserves every unrelated setting', () => {
  const config = {
    schema: 1,
    settings: {
      timezone: 'UTC',
      brightness: 73,
      future_setting: { enabled: true },
    },
    pages: [],
  }

  const changed = withTimezone(config, 'Europe/Warsaw')

  assert.equal(timezoneFor(changed), 'Europe/Warsaw')
  assert.deepEqual(changed.settings, {
    timezone: 'Europe/Warsaw',
    brightness: 73,
    future_setting: { enabled: true },
  })
  assert.equal(timezoneFor(config), 'UTC')

  const firmwareDefault = withTimezone(changed, null)
  assert.equal(timezoneFor(firmwareDefault), null)
  assert.deepEqual(firmwareDefault.settings, {
    brightness: 73,
    future_setting: { enabled: true },
  })
})

test('the editor list is exactly the firmware timezone list', () => {
  const table = readFileSync(
    new URL('../../firmware/components/slate_time/slate_time_zones.inc', import.meta.url),
    'ascii',
  )
  const firmwareZones = [...table.matchAll(/^\{"([^"]+)",/gm)].map((match) => match[1])

  assert.deepEqual([...TIMEZONES], firmwareZones)
  assert.ok(TIMEZONES.includes('Europe/Warsaw'))
})
