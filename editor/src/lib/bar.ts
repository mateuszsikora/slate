import type { BarItem } from './api'

export const BAR_SLOT_COUNT = 12

/**
 * The component names a bar item may carry (design.md §3.2). A bound item names
 * its component exactly as a tile does, which is where the binding's kind comes
 * from; `scene` is stateless and has no bar presentation.
 */
export const BAR_BOUND_TYPES = ['sensor', 'light', 'cover'] as const

/** One 64 px slot holds a caption or a reading, never both. */
export const BAR_BOUND_MIN_SPAN = 2

export function isBoundBarItem(item: BarItem): boolean {
  return (BAR_BOUND_TYPES as readonly string[]).includes(item.type)
}

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

/**
 * The panel's own bar rules, phrased for a person. The device validates the
 * same document again and its codes are the contract; this exists so a draft
 * can say what is wrong before it is published.
 */
export function barErrors(bar: BarItem[]): string[] {
  const errors: string[] = []
  const owners = new Map<number, number>()
  bar.forEach((item, index) => {
    const bound = isBoundBarItem(item)
    const minimumSpan = bound ? BAR_BOUND_MIN_SPAN : 1
    if (!Number.isInteger(item.slot) || item.slot < 0 || item.slot >= BAR_SLOT_COUNT) {
      errors.push(`Item ${index + 1} must start in slots 1–${BAR_SLOT_COUNT}.`)
    }
    if (
      !Number.isInteger(item.span) ||
      item.span < minimumSpan ||
      item.span > BAR_SLOT_COUNT ||
      item.slot + item.span > BAR_SLOT_COUNT
    ) {
      errors.push(
        bound && Number.isInteger(item.span) && item.span < BAR_BOUND_MIN_SPAN
          ? `Item ${index + 1} needs at least ${BAR_BOUND_MIN_SPAN} slots to show a value and a name.`
          : `Item ${index + 1} extends beyond the twelve-slot bar.`,
      )
    }
    if (item.type === 'badge' && !item.provider) errors.push(`Item ${index + 1} needs a provider.`)
    if (bound && !item.provider) errors.push(`Item ${index + 1} needs a provider.`)
    if (bound && !item.resource) errors.push(`Item ${index + 1} needs a resource.`)
    if (item.type === 'scene') {
      errors.push(`Item ${index + 1} cannot be a scene: scenes have no state to show.`)
    }
    for (let slot = Math.max(0, item.slot); slot < Math.min(BAR_SLOT_COUNT, item.slot + item.span); slot += 1) {
      const owner = owners.get(slot)
      if (owner !== undefined) {
        errors.push(`Items ${owner + 1} and ${index + 1} overlap at slot ${slot + 1}.`)
      }
      else owners.set(slot, index)
    }
  })
  return errors
}
