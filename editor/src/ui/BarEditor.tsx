import type { BarItem, ProviderStatus, ResourceCatalog } from '../lib/api'
import {
  BAR_BOUND_MIN_SPAN,
  BAR_RESOURCE_OPTION,
  BAR_SLOT_COUNT,
  barErrors,
  firstFreeSlot,
  isBoundBarItem,
  retype,
} from '../lib/bar'
import { providerLabel } from '../lib/providers'
import { ResourcePicker } from './ResourcePicker'

const KNOWN_TYPES = [
  { value: 'clock', label: 'Clock' },
  { value: 'title', label: 'Page title' },
  { value: 'page_indicator', label: 'Page number' },
  { value: 'badge', label: 'Provider badge' },
] as const

/**
 * A bound item's `type` is the component it renders, so the select offers one
 * "Resource" entry and this list is what the entry expands into. Picking an
 * entity sets it; the select is here for a resource typed by hand.
 */
const BOUND_TYPES = [
  { value: 'sensor', label: 'Sensor reading' },
  { value: 'light', label: 'Light' },
  { value: 'cover', label: 'Cover' },
] as const

interface Props {
  bar: BarItem[] | undefined
  providers: Pick<ProviderStatus, 'id' | 'status' | 'reason'>[]
  onLoadResources: (provider: string) => Promise<ResourceCatalog>
  onChange: (bar: BarItem[] | undefined) => void
}

export function BarEditor({ bar, providers, onLoadResources, onChange }: Props) {
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
  // A clock wants two slots so the time is never the thing a user has to widen
  // by hand, but one free slot is still a place to put an item — the panel
  // renders a single-slot clock in the caption font rather than clipping it.
  const pairSlot = firstFreeSlot(bar, 2)
  const newItem: BarItem | null =
    pairSlot !== null
      ? { type: 'clock', slot: pairSlot, span: 2 }
      : (() => {
          const single = firstFreeSlot(bar, 1)
          return single === null ? null : { type: 'clock', slot: single, span: 1 }
        })()
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
            disabled={newItem === null}
            title={newItem === null ? 'All twelve slots are occupied' : undefined}
            onClick={() => {
              if (newItem !== null) onChange([...bar, newItem])
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
        {Array.from({ length: BAR_SLOT_COUNT }, (_, slot) => {
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
          const bound = isBoundBarItem(item)
          const selectValue = bound ? BAR_RESOURCE_OPTION : item.type
          const known = bound || KNOWN_TYPES.some((type) => type.value === item.type)
          const providerIds = Array.from(
            new Set([...providers.map((provider) => provider.id), item.provider ?? '']),
          ).filter(Boolean)
          return (
            // Keyed by position alone: picking an entity of another kind
            // rewrites `type`, and keying on that would unmount the picker
            // mid-search and refetch the catalog. retype() already clears the
            // fields a real type change invalidates.
            <div className="bar-item" key={index}>
              <label className="field">
                <span>Item</span>
                <select
                  value={selectValue}
                  onChange={(event) =>
                    update(index, retype(item, event.currentTarget.value, providerIds[0] ?? ''))
                  }
                >
                  {!known ? (
                    <option value={item.type}>{item.type} (newer firmware)</option>
                  ) : null}
                  {KNOWN_TYPES.map((type) => (
                    <option key={type.value} value={type.value}>{type.label}</option>
                  ))}
                  <option value={BAR_RESOURCE_OPTION}>Resource</option>
                </select>
              </label>
              <label className="field bar-item__number">
                <span>Start slot</span>
                <input
                  type="number"
                  min="1"
                  max={BAR_SLOT_COUNT}
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
                  min={bound ? BAR_BOUND_MIN_SPAN : 1}
                  max={BAR_SLOT_COUNT}
                  value={item.span}
                  onChange={(event) =>
                    update(index, { ...item, span: Number(event.currentTarget.value) })
                  }
                />
              </label>
              {item.type === 'badge' ? (
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
              ) : null}
              {bound ? (
                <>
                  <label className="field">
                    <span>Shows</span>
                    <select
                      value={item.type}
                      onChange={(event) => update(index, { ...item, type: event.currentTarget.value })}
                    >
                      {BOUND_TYPES.map((type) => (
                        <option key={type.value} value={type.value}>{type.label}</option>
                      ))}
                    </select>
                  </label>
                  <ResourcePicker
                    binding={{ provider: item.provider ?? '', resource: item.resource ?? '' }}
                    providers={providers}
                    kinds={BOUND_TYPES.map((type) => type.value)}
                    resetKey={`bar-${index}`}
                    onLoadResources={onLoadResources}
                    onChange={(binding, picked) =>
                      update(index, {
                        ...item,
                        // The picked entity's kind is what the panel will
                        // publish, so it decides the presentation rather than
                        // leaving the two free to disagree.
                        type: picked?.kind ?? item.type,
                        provider: binding.provider,
                        resource: binding.resource,
                      })
                    }
                  />
                </>
              ) : null}
              {item.type === 'badge' || bound ? (
                <label className="field">
                  <span>Label override</span>
                  <input
                    value={item.label ?? ''}
                    placeholder={bound ? 'Use the resource name' : 'Use provider name'}
                    onChange={(event) => {
                      const next = { ...item }
                      if (event.currentTarget.value === '') delete next.label
                      else next.label = event.currentTarget.value
                      update(index, next)
                    }}
                  />
                </label>
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

function itemLabel(item: BarItem): string {
  if (isBoundBarItem(item)) {
    return item.label ?? (item.resource !== undefined && item.resource !== '' ? item.resource : 'Resource')
  }
  return KNOWN_TYPES.find((type) => type.value === item.type)?.label ?? item.type
}
