import type { BarItem, ProviderStatus } from '../lib/api'
import { providerLabel } from '../lib/providers'

const SLOT_COUNT = 12

const KNOWN_TYPES = [
  { value: 'clock', label: 'Clock' },
  { value: 'title', label: 'Page title' },
  { value: 'page_indicator', label: 'Page number' },
  { value: 'badge', label: 'Provider badge' },
] as const

interface Props {
  bar: BarItem[] | undefined
  providers: Pick<ProviderStatus, 'id' | 'status'>[]
  onChange: (bar: BarItem[] | undefined) => void
}

export function BarEditor({ bar, providers, onChange }: Props) {
  if (bar === undefined) {
    return (
      <section className="bar-editor" aria-labelledby="bar-editor-title">
        <div className="bar-editor__header">
          <div>
            <h3 id="bar-editor-title">System bar</h3>
            <p>The panel is using the compatible clock, title, status and page-number layout.</p>
          </div>
          <button
            type="button"
            className="button button--secondary"
            onClick={() => onChange(defaultBar(providers))}
          >
            Customize bar
          </button>
        </div>
      </section>
    )
  }

  const errors = barErrors(bar)
  const freeSlot = firstFreeSlot(bar)
  const update = (index: number, item: BarItem) => {
    const next = [...bar]
    next[index] = item
    onChange(next)
  }

  return (
    <section className="bar-editor" aria-labelledby="bar-editor-title">
      <div className="bar-editor__header">
        <div>
          <h3 id="bar-editor-title">System bar</h3>
          <p>Place non-interactive items in twelve 64 px slots. Empty slots stay blank.</p>
        </div>
        <div className="bar-editor__actions">
          <button
            type="button"
            className="button button--secondary"
            disabled={freeSlot === null}
            title={freeSlot === null ? 'All twelve slots are occupied' : undefined}
            onClick={() => {
              if (freeSlot !== null) onChange([...bar, { type: 'clock', slot: freeSlot, span: 1 }])
            }}
          >
            Add item
          </button>
          <button type="button" className="link" onClick={() => onChange(undefined)}>
            Use compatible layout
          </button>
        </div>
      </div>

      <div className="bar-slots" aria-label="System bar slot preview">
        {Array.from({ length: SLOT_COUNT }, (_, slot) => {
          const item = bar.find((entry) => slot >= entry.slot && slot < entry.slot + entry.span)
          return (
            <span
              key={slot}
              className={`bar-slot${item === undefined ? '' : ' bar-slot--used'}`}
            >
              <small>{slot + 1}</small>
              {item !== undefined && item.slot === slot ? itemLabel(item) : null}
            </span>
          )
        })}
      </div>

      {errors.length > 0 ? (
        <ul className="bar-editor__errors" role="alert">
          {errors.map((error) => (
            <li key={error}>{error}</li>
          ))}
        </ul>
      ) : null}

      {bar.length === 0 ? (
        <p className="bar-editor__empty">
          This configuration intentionally leaves the system bar blank.
        </p>
      ) : null}

      <div className="bar-items">
        {bar.map((item, index) => {
          const known = KNOWN_TYPES.some((type) => type.value === item.type)
          const providerIds = Array.from(
            new Set([...providers.map((provider) => provider.id), item.provider ?? '']),
          ).filter(Boolean)
          return (
            <div className="bar-item" key={`${index}-${item.type}`}>
              <label className="field">
                <span>Item</span>
                <select
                  value={item.type}
                  onChange={(event) =>
                    update(index, { ...item, type: event.currentTarget.value })
                  }
                >
                  {!known ? (
                    <option value={item.type}>{item.type} (newer firmware)</option>
                  ) : null}
                  {KNOWN_TYPES.map((type) => (
                    <option key={type.value} value={type.value}>{type.label}</option>
                  ))}
                </select>
              </label>
              <label className="field bar-item__number">
                <span>Start slot</span>
                <input
                  type="number"
                  min="1"
                  max={SLOT_COUNT}
                  value={item.slot + 1}
                  onChange={(event) =>
                    update(index, { ...item, slot: Number(event.currentTarget.value) - 1 })
                  }
                />
              </label>
              <label className="field bar-item__number">
                <span>Width</span>
                <input
                  type="number"
                  min="1"
                  max={SLOT_COUNT}
                  value={item.span}
                  onChange={(event) =>
                    update(index, { ...item, span: Number(event.currentTarget.value) })
                  }
                />
              </label>
              {item.type === 'badge' ? (
                <>
                  <label className="field">
                    <span>Provider</span>
                    <select
                      value={item.provider ?? ''}
                      onChange={(event) =>
                        update(index, { ...item, provider: event.currentTarget.value })
                      }
                    >
                      <option value="">Choose provider</option>
                      {providerIds.map((provider) => (
                        <option key={provider} value={provider}>{providerLabel(provider)}</option>
                      ))}
                    </select>
                  </label>
                  <label className="field">
                    <span>Label override</span>
                    <input
                      value={item.label ?? ''}
                      placeholder="Use provider name"
                      onChange={(event) => {
                        const next = { ...item }
                        if (event.currentTarget.value === '') delete next.label
                        else next.label = event.currentTarget.value
                        update(index, next)
                      }}
                    />
                  </label>
                </>
              ) : null}
              <button
                type="button"
                className="link link--danger bar-item__remove"
                onClick={() => onChange(bar.filter((_, entry) => entry !== index))}
              >
                Remove
              </button>
            </div>
          )
        })}
      </div>
    </section>
  )
}

function defaultBar(providers: Pick<ProviderStatus, 'id' | 'status'>[]): BarItem[] {
  const provider = providers.find((entry) => entry.id === 'ha')?.id ?? providers[0]?.id ?? 'direct'
  return [
    { type: 'clock', slot: 0, span: 2 },
    { type: 'title', slot: 2, span: 6 },
    { type: 'page_indicator', slot: 8, span: 1 },
    { type: 'badge', slot: 9, span: 3, provider },
  ]
}

function firstFreeSlot(bar: BarItem[]): number | null {
  const occupied = new Set<number>()
  for (const item of bar) {
    for (let slot = item.slot; slot < item.slot + item.span; slot += 1) occupied.add(slot)
  }
  return Array.from({ length: SLOT_COUNT }, (_, index) => index).find(
    (index) => !occupied.has(index),
  ) ?? null
}

function itemLabel(item: BarItem): string {
  return KNOWN_TYPES.find((type) => type.value === item.type)?.label ?? item.type
}

function barErrors(bar: BarItem[]): string[] {
  const errors: string[] = []
  const owners = new Map<number, number>()
  bar.forEach((item, index) => {
    if (!Number.isInteger(item.slot) || item.slot < 0 || item.slot >= SLOT_COUNT) {
      errors.push(`Item ${index + 1} must start in slots 1–${SLOT_COUNT}.`)
    }
    if (
      !Number.isInteger(item.span) ||
      item.span < 1 ||
      item.span > SLOT_COUNT ||
      item.slot + item.span > SLOT_COUNT
    ) {
      errors.push(`Item ${index + 1} extends beyond the twelve-slot bar.`)
    }
    if (item.type === 'badge' && !item.provider) errors.push(`Item ${index + 1} needs a provider.`)
    for (let slot = Math.max(0, item.slot); slot < Math.min(SLOT_COUNT, item.slot + item.span); slot += 1) {
      const owner = owners.get(slot)
      if (owner !== undefined) {
        errors.push(`Items ${owner + 1} and ${index + 1} overlap at slot ${slot + 1}.`)
      }
      else owners.set(slot, index)
    }
  })
  return errors
}
