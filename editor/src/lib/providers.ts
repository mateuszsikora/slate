export function providerLabel(id: string): string {
  if (id === 'direct') return 'External API'
  if (id === 'ha') return 'Home Assistant'
  return id
}
