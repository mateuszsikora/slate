import { useEffect, useMemo, useState } from 'react'

import { type Binding, type ProviderStatus, type ResourceCatalog, type Tile } from '../lib/api'
import { COMPONENTS, bindingFor, definitionFor, withSize, type TileSize } from '../lib/editor'
import { ResourcePicker } from './ResourcePicker'

interface Props {
  tile: Tile | null
  providers: Pick<ProviderStatus, 'id' | 'status' | 'reason'>[]
  onLoadResources: (provider: string) => Promise<ResourceCatalog>
  onUpdate: (tile: Tile) => void
  onDelete: () => void
}

export function Inspector({ tile, providers, onLoadResources, onUpdate, onDelete }: Props) {
  const [activeBinding, setActiveBinding] = useState(0)

  const bindings = useMemo(() => {
    if (tile === null) {
      return []
    }
    return tile.bindings ?? [bindingFor(tile)]
  }, [tile])
  const selectedBinding = bindings[Math.min(activeBinding, Math.max(0, bindings.length - 1))]
  const selectedBindingIndex = Math.min(activeBinding, Math.max(0, bindings.length - 1))

  useEffect(() => {
    setActiveBinding(0)
  }, [tile?.id])

  useEffect(() => {
    if (bindings.length > 0 && activeBinding >= bindings.length) {
      setActiveBinding(bindings.length - 1)
    }
  }, [activeBinding, bindings.length])

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
          {tile.bindings.map((_, index) => (
            <button
              key={index}
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
        <ResourcePicker
          binding={selectedBinding}
          providers={providers}
          kinds={[tile.type]}
          resetKey={`${tile.id}:${selectedBindingIndex}`}
          onLoadResources={onLoadResources}
          onChange={(binding) => updateBinding(selectedBindingIndex, binding)}
        />
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
