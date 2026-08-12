import { useEffect, useMemo, useState } from 'react'

import { ApiError, type Binding, type ProviderStatus, type Resource, type Tile } from '../lib/api'
import { COMPONENTS, bindingFor, definitionFor, withSize, type TileSize } from '../lib/editor'

interface Props {
  tile: Tile | null
  providers: Pick<ProviderStatus, 'id' | 'status'>[]
  onLoadResources: (provider: string) => Promise<Resource[]>
  onUpdate: (tile: Tile) => void
  onDelete: () => void
}

export function Inspector({ tile, providers, onLoadResources, onUpdate, onDelete }: Props) {
  const [activeBinding, setActiveBinding] = useState(0)
  const [resources, setResources] = useState<Resource[]>([])
  const [catalogState, setCatalogState] = useState<'idle' | 'loading' | 'ready' | 'error'>('idle')
  const [catalogMessage, setCatalogMessage] = useState('')
  const [query, setQuery] = useState('')
  const [area, setArea] = useState('')

  const bindings = useMemo(() => {
    if (tile === null) {
      return []
    }
    return tile.bindings ?? [bindingFor(tile)]
  }, [tile])
  const selectedBinding = bindings[Math.min(activeBinding, Math.max(0, bindings.length - 1))]
  const provider = selectedBinding?.provider ?? ''

  useEffect(() => {
    setActiveBinding(0)
    setQuery('')
    setArea('')
  }, [tile?.id])

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
  }, [provider, onLoadResources])

  if (tile === null) {
    return (
      <section className="editor-panel inspector">
        <h3 className="editor-panel__title">Inspector</h3>
        <p className="editor-panel__empty">Select a tile to edit its resource and presentation.</p>
      </section>
    )
  }

  const definition = definitionFor(tile.type)
  const providerIds = Array.from(
    new Set([...providers.map((entry) => entry.id), ...bindings.map((binding) => binding.provider)]),
  ).filter(Boolean)
  const areas = Array.from(new Set(resources.map((resource) => resource.area).filter(Boolean))).sort()
  const needle = query.trim().toLowerCase()
  const matching = resources.filter(
    (resource) =>
      resource.kind === tile.type &&
      (area === '' || resource.area === area) &&
      (needle === '' ||
        resource.resource.toLowerCase().includes(needle) ||
        resource.name?.toLowerCase().includes(needle)),
  )

  const updateBinding = (index: number, binding: Binding) => {
    if (tile.bindings !== undefined) {
      const next = [...tile.bindings]
      next[index] = binding
      onUpdate({ ...tile, bindings: next })
    } else {
      onUpdate({ ...tile, binding })
    }
  }

  const changeType = (type: string) => {
    const nextDefinition = definitionFor(type)
    let next: Tile = { ...tile, type }
    if (nextDefinition !== undefined) {
      next = withSize(next, nextDefinition.defaultSize)
    }
    onUpdate(next)
  }

  const changeSize = (size: TileSize) => {
    onUpdate(withSize(tile, size))
  }

  return (
    <section className="editor-panel inspector" aria-labelledby="inspector-title">
      <div className="editor-panel__heading">
        <h3 id="inspector-title" className="editor-panel__title">
          Inspector
        </h3>
        <code>{tile.id}</code>
      </div>

      <label className="field">
        <span>Component</span>
        <select value={tile.type} onChange={(event) => changeType(event.currentTarget.value)}>
          {definition === undefined ? <option value={tile.type}>{tile.type} (newer firmware)</option> : null}
          {COMPONENTS.map((component) => (
            <option key={component.type} value={component.type}>
              {component.title}
            </option>
          ))}
        </select>
      </label>

      {definition !== undefined ? (
        <fieldset className="field field--sizes">
          <legend>Size</legend>
          <div>
            {definition.sizes.map((size) => (
              <button
                key={`${size[0]}x${size[1]}`}
                type="button"
                className={tile.size[0] === size[0] && tile.size[1] === size[1] ? 'selected' : ''}
                onClick={() => changeSize(size)}
              >
                {size[0]}×{size[1]}
              </button>
            ))}
          </div>
        </fieldset>
      ) : null}

      <label className="field">
        <span>Label override</span>
        <input
          value={tile.label ?? ''}
          placeholder="Use the resource name"
          onChange={(event) => {
            const value = event.currentTarget.value
            const next = { ...tile }
            if (value === '') delete next.label
            else next.label = value
            onUpdate(next)
          }}
        />
      </label>

      <label className="field">
        <span>Icon override</span>
        <input
          value={tile.icon ?? ''}
          placeholder="e.g. lightbulb-outline"
          onChange={(event) => {
            const value = event.currentTarget.value
            const next = { ...tile }
            if (value === '') delete next.icon
            else next.icon = value
            onUpdate(next)
          }}
        />
      </label>

      {tile.bindings !== undefined ? (
        <div className="binding-tabs" aria-label="Scene bindings">
          {tile.bindings.map((binding, index) => (
            <button
              key={`${index}-${binding.provider}-${binding.resource}`}
              type="button"
              className={index === activeBinding ? 'selected' : ''}
              onClick={() => setActiveBinding(index)}
            >
              Scene {index + 1}
            </button>
          ))}
          {tile.bindings.length < 5 ? (
            <button
              type="button"
              onClick={() => {
                const next = [...tile.bindings!, { provider: providerIds[0] ?? '', resource: '' }]
                onUpdate({ ...tile, bindings: next })
                setActiveBinding(next.length - 1)
              }}
            >
              + Scene
            </button>
          ) : null}
        </div>
      ) : null}

      {selectedBinding !== undefined ? (
        <>
          <label className="field">
            <span>Provider</span>
            <select
              value={selectedBinding.provider}
              onChange={(event) => {
                updateBinding(activeBinding, { provider: event.currentTarget.value, resource: '' })
                setQuery('')
                setArea('')
              }}
            >
              <option value="">Choose a provider</option>
              {providerIds.map((id) => (
                <option key={id} value={id}>
                  {id} — {providers.find((entry) => entry.id === id)?.status ?? 'unknown'}
                </option>
              ))}
            </select>
          </label>

          <label className="field">
            <span>Resource ID</span>
            <input
              value={selectedBinding.resource}
              placeholder="Choose below or type an ID"
              onChange={(event) =>
                updateBinding(activeBinding, {
                  ...selectedBinding,
                  resource: event.currentTarget.value,
                })
              }
            />
          </label>

          {catalogState === 'loading' ? <p className="catalog-note">Loading resources…</p> : null}
          {catalogState === 'error' ? <p className="catalog-note catalog-note--error">{catalogMessage}</p> : null}
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
                  <p>No matching {tile.type} resources. You can still type an ID above.</p>
                ) : (
                  matching.slice(0, 80).map((resource) => (
                    <button
                      key={`${resource.provider}:${resource.resource}`}
                      type="button"
                      className={resource.resource === selectedBinding.resource ? 'selected' : ''}
                      onClick={() =>
                        updateBinding(activeBinding, {
                          provider: resource.provider,
                          resource: resource.resource,
                        })
                      }
                    >
                      <strong>{resource.name ?? resource.resource}</strong>
                      <span>{resource.area ?? 'No area'} · {resource.resource}</span>
                    </button>
                  ))
                )}
              </div>
            </div>
          ) : null}
        </>
      ) : null}

      <div className="inspector__actions">
        {tile.bindings !== undefined && tile.bindings.length > 2 ? (
          <button
            type="button"
            className="button button--secondary"
            onClick={() => {
              const next = tile.bindings!.filter((_, index) => index !== activeBinding)
              onUpdate({ ...tile, bindings: next })
              setActiveBinding(Math.max(0, activeBinding - 1))
            }}
          >
            Remove scene
          </button>
        ) : null}
        <button type="button" className="button button--danger" onClick={onDelete}>
          Delete tile
        </button>
      </div>
    </section>
  )
}

function resourceError(error: unknown, provider: string): string {
  if (!(error instanceof ApiError)) {
    return `The ${provider} catalog could not be loaded.`
  }
  if (error.code === 'provider_unconfigured') {
    return `${provider} is not configured on this panel yet.`
  }
  if (error.code === 'unreachable') {
    return 'The panel did not answer. Resource IDs can still be entered manually.'
  }
  return `The ${provider} catalog is unavailable (${error.code}).`
}
