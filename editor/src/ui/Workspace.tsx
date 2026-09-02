import { useEffect, useState } from 'react'

import type { Config, ProviderStatus, Resource, Tile } from '../lib/api'
import {
  definitionFor,
  firstFreePosition,
  nextTileId,
  tileFits,
  withSize,
  type TileSize,
} from '../lib/editor'
import { ComponentLibrary } from './ComponentLibrary'
import { BarEditor } from './BarEditor'
import { ConfigTransfer } from './ConfigTransfer'
import { DashboardGrid } from './DashboardGrid'
import { Inspector } from './Inspector'

interface Props {
  config: Config | null
  activePageId: string | null
  providers: Pick<ProviderStatus, 'id' | 'status'>[]
  previewState: 'idle' | 'waiting' | 'sending' | 'live' | 'error'
  previewMessage: string | null
  dirty: boolean
  deviceName: string
  onCreate: () => void
  onChange: (config: Config) => void
  onLoadResources: (provider: string) => Promise<Resource[]>
  onValidateConfig: (document: string) => Promise<void>
  onPublishConfig: (document: string, config: Config) => Promise<void>
}

export function Workspace({
  config,
  activePageId,
  providers,
  previewState,
  previewMessage,
  dirty,
  deviceName,
  onCreate,
  onChange,
  onLoadResources,
  onValidateConfig,
  onPublishConfig,
}: Props) {
  const [selectedId, setSelectedId] = useState<string | null>(null)
  const [notice, setNotice] = useState<string | null>(null)

  const page = config?.pages.find((entry) => entry.id === activePageId) ?? config?.pages[0] ?? null
  const selected = page?.tiles.find((tile) => tile.id === selectedId) ?? null

  useEffect(() => {
    if (selectedId !== null && selected === null) {
      setSelectedId(null)
    }
  }, [selected, selectedId])

  if (config === null || page === null) {
    return (
      <main className="workspace workspace--empty">
        <section className="empty-dashboard">
          <span className="empty-dashboard__grid" aria-hidden="true" />
          <h2>Build your first page</h2>
          <p>
            Start with a four-by-three dashboard. Components, bindings and placement can all be
            changed here; the panel remains the visual preview.
          </p>
          <button type="button" className="button" onClick={onCreate}>
            Create dashboard
          </button>
        </section>
      </main>
    )
  }

  const replacePage = (nextPage: typeof page) => {
    onChange({
      ...config,
      pages: config.pages.map((entry) => (entry.id === page.id ? nextPage : entry)),
    })
  }

  const updateTile = (nextTile: Tile) => {
    if (!tileFits(page.tiles, nextTile, nextTile.id)) {
      setNotice('That size or position overlaps another tile or leaves the grid.')
      return
    }
    setNotice(null)
    replacePage({
      ...page,
      tiles: page.tiles.map((tile) => (tile.id === nextTile.id ? nextTile : tile)),
    })
  }

  const addTile = (type: string, requested?: [number, number]) => {
    const definition = definitionFor(type)
    if (definition === undefined) {
      return
    }
    const exact: Tile = {
      id: nextTileId(config, type),
      type,
      pos: requested ?? [0, 0],
      size: [definition.defaultSize[0], definition.defaultSize[1]],
      binding: { provider: providers[0]?.id ?? 'direct', resource: '' },
    }
    const fallback = firstFreePosition(page.tiles, definition.defaultSize)
    if (!tileFits(page.tiles, exact)) {
      if (fallback === null) {
        setNotice(`There is no ${definition.defaultSize[0]}×${definition.defaultSize[1]} space left.`)
        return
      }
      exact.pos = fallback
    }
    replacePage({ ...page, tiles: [...page.tiles, exact] })
    setSelectedId(exact.id)
    setNotice('Tile added. Choose its provider and resource in the inspector.')
  }

  const deleteTile = () => {
    if (selected === null) return
    replacePage({ ...page, tiles: page.tiles.filter((tile) => tile.id !== selected.id) })
    setSelectedId(null)
    setNotice('Tile removed from the draft.')
  }

  const deletePage = () => {
    if (config.pages.length <= 1) {
      setNotice('A dashboard must keep at least one page.')
      return
    }
    const pages = config.pages.filter((entry) => entry.id !== page.id)
    onChange({
      ...config,
      pages,
      home_page: config.home_page === page.id ? pages[0]?.id : config.home_page,
    })
    setSelectedId(null)
  }

  return (
    <main className="workspace">
      <div className="workspace__status">
        <div>
          <label className="page-title-field">
            <span>Page</span>
            <input
              value={page.title ?? ''}
              placeholder={page.id}
              onChange={(event) => {
                const title = event.currentTarget.value
                const next = { ...page }
                if (title === '') delete next.title
                else next.title = title
                replacePage(next)
              }}
            />
          </label>
          <button
            type="button"
            className={`home-toggle${config.home_page === page.id ? ' selected' : ''}`}
            onClick={() => onChange({ ...config, home_page: page.id })}
          >
            {config.home_page === page.id ? 'Home page' : 'Make home'}
          </button>
          <button
            type="button"
            className="link link--danger"
            onClick={deletePage}
            disabled={config.pages.length <= 1}
            title={config.pages.length <= 1 ? 'A dashboard must keep at least one page' : undefined}
          >
            Delete page
          </button>
        </div>
        <span
          className={`preview-state preview-state--${previewState}`}
          role="status"
          aria-live="polite"
        >
          {previewLabel(previewState, previewMessage)}
        </span>
      </div>

      {notice !== null ? (
        <p className="workspace__notice" role="status">
          {notice}
        </p>
      ) : null}

      <BarEditor
        bar={config.bar}
        providers={providers}
        onChange={(bar) => {
          const next = { ...config }
          if (bar === undefined) delete next.bar
          else next.bar = bar
          onChange(next)
        }}
      />

      <div className="editor-layout">
        <ComponentLibrary onAdd={(type) => addTile(type)} />
        <DashboardGrid
          page={page}
          selectedId={selectedId}
          onSelect={setSelectedId}
          onMove={(id, pos) => {
            const tile = page.tiles.find((entry) => entry.id === id)
            if (tile !== undefined) updateTile({ ...tile, pos })
          }}
          onResize={(id, size: TileSize) => {
            const tile = page.tiles.find((entry) => entry.id === id)
            if (tile !== undefined) updateTile(withSize(tile, size))
          }}
          onAdd={(type, pos) => addTile(type, pos)}
        />
        <Inspector
          tile={selected}
          providers={providers}
          onLoadResources={onLoadResources}
          onUpdate={updateTile}
          onDelete={deleteTile}
        />
      </div>

      <details className="workspace__files">
        <summary>Import or export JSON</summary>
        <ConfigTransfer
          config={config}
          deviceName={deviceName}
          hasUnpublishedChanges={dirty}
          onValidate={onValidateConfig}
          onPublish={onPublishConfig}
        />
      </details>
    </main>
  )
}

function previewLabel(state: Props['previewState'], message: string | null): string {
  if (state === 'waiting') return message ?? 'Preview is waiting'
  if (state === 'sending') return 'Updating panel…'
  if (state === 'live') return 'Live on panel'
  if (state === 'error') return message ?? 'Preview rejected'
  return 'No unpublished changes'
}
