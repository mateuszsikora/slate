export function providerLabel(id: string): string {
  if (id === 'direct') return 'External API'
  if (id === 'ha') return 'Home Assistant'
  if (id === 'tuya') return 'Tuya'
  return id
}

export function providerReason(reason: string | undefined): string | null {
  if (reason === undefined || reason === '') return null
  if (reason === 'auth') return 'Tuya rejected the stored credentials. Reconnect with a valid Access ID and Secret.'
  if (reason === 'quota') return 'The Tuya cloud subscription or quota is unavailable. Check the project at iot.tuya.com.'
  return `Tuya reported ${reason}. Reconnect the integration if the problem continues.`
}
