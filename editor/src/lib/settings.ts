import type { Config } from './api'

/** Return the configured zone without pretending that an absent key is stored. */
export function timezoneFor(config: Config): string | null {
  const timezone = config.settings?.timezone
  return typeof timezone === 'string' ? timezone : null
}

/** Change only timezone; imported and future settings remain byte-for-byte values. */
export function withTimezone(config: Config, timezone: string | null): Config {
  const settings = { ...config.settings }
  if (timezone === null) {
    delete settings.timezone
  } else {
    settings.timezone = timezone
  }
  return {
    ...config,
    settings,
  }
}
