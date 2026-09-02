import type { BarItem } from './api'

export const BAR_SLOT_COUNT = 12

export function firstFreeSlot(bar: BarItem[], span = 1): number | null {
  if (!Number.isInteger(span) || span < 1 || span > BAR_SLOT_COUNT) return null

  const occupied = Array<boolean>(BAR_SLOT_COUNT).fill(false)
  for (const item of bar) {
    if (!Number.isInteger(item.slot) || !Number.isInteger(item.span) || item.span < 1) continue
    const start = Math.max(0, item.slot)
    const end = Math.min(BAR_SLOT_COUNT, item.slot + item.span)
    for (let slot = start; slot < end; slot += 1) occupied[slot] = true
  }

  for (let slot = 0; slot <= BAR_SLOT_COUNT - span; slot += 1) {
    if (occupied.slice(slot, slot + span).every((value) => !value)) return slot
  }
  return null
}
