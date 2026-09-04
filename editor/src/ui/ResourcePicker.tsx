import { useEffect, useState } from 'react'

import { ApiError, type Binding, type ProviderStatus, type Resource } from '../lib/api'
import { providerLabel } from '../lib/providers'

interface Props {
  binding: Binding
  providers: Pick<ProviderStatus, 'id' | 'status'>[]
  /** Normalized kinds this consumer can render; the catalog is filtered to them. */
  kinds: string[]
  /** Changing this resets the search, as moving to another tile or bar item should. */
  resetKey: string
  onLoadResources: (provider: string) => Promise<Resource[]>
  onChange: (binding: Binding, picked?: Resource) => void
}

/**
 * The provider and resource half of a binding (DESIGN.md §3.3), with §5.7's
 * catalog behind it. Tiles and system-bar items ask the same question, so they
 * ask it with the same control rather than with two that drift apart.
 */
export function ResourcePicker({
  binding,
  providers,
  kinds,
  resetKey,
  onLoadResources,
  onChange,
}: Props) {
  const [resources, setResources] = useState<Resource[]>([])
  const [catalogState, setCatalogState] = useState<'idle' | 'loading' | 'ready' | 'error'>('idle')
  const [catalogMessage, setCatalogMessage] = useState('')
  const [catalogRetry, setCatalogRetry] = useState(0)
  const [query, setQuery] = useState('')
  const [area, setArea] = useState('')

  const provider = binding.provider

  useEffect(() => {
    setQuery('')
    setArea('')
  }, [resetKey])

  useEffect(() => {
    if (provider === '') {
      setResources([])
      setCatalogState('idle')
      return
    }
    let cancelled = false
    setCatalogState('loading')
    setCatalogMessage('')
    void onLoadResources(provider)
      .then((catalog) => {
        if (!cancelled) {
          setResources(catalog)
          setCatalogState('ready')
        }
      })
      .catch((error: unknown) => {
        if (!cancelled) {
          setResources([])
          setCatalogState('error')
          setCatalogMessage(resourceError(error, provider))
        }
      })
    return () => {
      cancelled = true
    }
  }, [catalogRetry, provider, onLoadResources])

  const providerIds = Array.from(
    new Set([...providers.map((entry) => entry.id), binding.provider]),
  ).filter(Boolean)
  const areas = Array.from(new Set(resources.map((resource) => resource.area).filter(Boolean))).sort()
  const needle = query.trim().toLowerCase()
  const matching = resources.filter(
    (resource) =>
      kinds.includes(resource.kind) &&
      (area === '' || resource.area === area) &&
      (needle === '' ||
        resource.resource.toLowerCase().includes(needle) ||
        resource.name?.toLowerCase().includes(needle)),
  )

  return (
    <>
      <label className="field">
        <span>Provider</span>
        <select
          value={binding.provider}
          onChange={(event) => {
            onChange({ provider: event.currentTarget.value, resource: '' })
            setQuery('')
            setArea('')
          }}
        >
          <option value="">Choose a provider</option>
          {providerIds.map((id) => (
            <option key={id} value={id}>
              {providerLabel(id)} — {providers.find((entry) => entry.id === id)?.status ?? 'unknown'}
            </option>
          ))}
        </select>
      </label>

      <label className="field">
        <span>Resource ID</span>
        <input
          value={binding.resource}
          placeholder="Choose below or type an ID"
          onChange={(event) => onChange({ ...binding, resource: event.currentTarget.value })}
        />
      </label>

      {catalogState === 'loading' ? <p className="catalog-note">Loading resources…</p> : null}
      {catalogState === 'error' ? (
        <div className="catalog-note catalog-note--error">
          {catalogMessage}{' '}
          <button type="button" className="link" onClick={() => setCatalogRetry((value) => value + 1)}>
            Retry
          </button>
        </div>
      ) : null}
      {catalogState === 'ready' ? (
        <div className="catalog">
          <div className="catalog__filters">
            <input
              type="search"
              value={query}
              placeholder="Search resources"
              onChange={(event) => setQuery(event.currentTarget.value)}
            />
            <select value={area} onChange={(event) => setArea(event.currentTarget.value)}>
              <option value="">All areas</option>
              {areas.map((name) => (
                <option key={name} value={name}>
                  {name}
                </option>
              ))}
            </select>
          </div>
          <div className="catalog__results">
            {matching.length === 0 ? (
              <p>
                {provider === 'direct'
                  ? 'No External API resources have published state yet. Type an ID above, publish the dashboard, then send that resource to the panel.'
                  : `No matching ${kinds.length === 1 ? kinds[0] : 'bindable'} resources. You can still type an ID above.`}
              </p>
            ) : (
              matching.slice(0, 80).map((resource) => (
                <button
                  key={`${resource.provider}:${resource.resource}`}
                  type="button"
                  className={resource.resource === binding.resource ? 'selected' : ''}
                  onClick={() =>
                    onChange({ provider: resource.provider, resource: resource.resource }, resource)
                  }
                >
                  <strong>{resource.name ?? resource.resource}</strong>
                  <span>
                    {resource.area ?? 'No area'} · {resource.resource}
                    {resource.available ? '' : ' · unavailable'}
                  </span>
                </button>
              ))
            )}
          </div>
        </div>
      ) : null}
    </>
  )
}

function resourceError(error: unknown, provider: string): string {
  if (!(error instanceof ApiError)) {
    return `The ${providerLabel(provider)} catalog could not be loaded.`
  }
  if (error.code === 'provider_unconfigured') {
    return `${providerLabel(provider)} is not configured on this panel yet. Open Integrations to connect it.`
  }
  if (error.code === 'unreachable') {
    return 'The panel did not answer. Resource IDs can still be entered manually.'
  }
  return `The ${providerLabel(provider)} catalog is unavailable (${error.code}).`
}
