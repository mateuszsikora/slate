import { useRef, type KeyboardEvent, type PointerEvent } from 'react'

import type { Page, Tile } from '../lib/api'
import { providerLabel } from '../lib/providers'
import { COMPONENTS, GRID_COLUMNS, GRID_ROWS, definitionFor, type TileSize } from '../lib/editor'

interface Props {
  page: Page
  selectedId: string | null
  onSelect: (id: string) => void
  onMove: (id: string, pos: [number, number]) => void
  onResize: (id: string, size: TileSize) => void
  onAdd: (type: string, pos: [number, number]) => void
}

interface Gesture {
  kind: 'move' | 'resize'
  tile: Tile
  x: number
  y: number
}

export function DashboardGrid({ page, selectedId, onSelect, onMove, onResize, onAdd }: Props) {
  const surface = useRef<HTMLDivElement>(null)
  const gesture = useRef<Gesture | null>(null)

  const cellAt = (clientX: number, clientY: number): [number, number] => {
    const bounds = surface.current?.getBoundingClientRect()
    if (bounds === undefined) {
      return [0, 0]
    }
    return [
      Math.max(0, Math.min(GRID_COLUMNS - 1, Math.floor(((clientX - bounds.left) / bounds.width) * GRID_COLUMNS))),
      Math.max(0, Math.min(GRID_ROWS - 1, Math.floor(((clientY - bounds.top) / bounds.height) * GRID_ROWS))),
    ]
  }

  const begin = (event: PointerEvent, tile: Tile, kind: Gesture['kind']) => {
    event.preventDefault()
    event.stopPropagation()
    event.currentTarget.setPointerCapture(event.pointerId)
    gesture.current = { kind, tile, x: event.clientX, y: event.clientY }
    onSelect(tile.id)
  }

  const move = (event: PointerEvent) => {
    const active = gesture.current
    const bounds = surface.current?.getBoundingClientRect()
    if (active === null || bounds === undefined) {
      return
    }

    const dx = Math.round(((event.clientX - active.x) / bounds.width) * GRID_COLUMNS)
    const dy = Math.round(((event.clientY - active.y) / bounds.height) * GRID_ROWS)
    if (active.kind === 'move') {
      onMove(active.tile.id, [active.tile.pos[0] + dx, active.tile.pos[1] + dy])
      return
    }

    const definition = definitionFor(active.tile.type)
    if (definition === undefined) {
      return
    }
    const targetWidth = Math.max(1, active.tile.size[0] + dx)
    const targetHeight = Math.max(1, active.tile.size[1] + dy)
    const nearest = [...definition.sizes].sort(
      (left, right) =>
        Math.abs(left[0] - targetWidth) + Math.abs(left[1] - targetHeight) -
        (Math.abs(right[0] - targetWidth) + Math.abs(right[1] - targetHeight)),
    )[0]
    if (nearest !== undefined) {
      onResize(active.tile.id, nearest)
    }
  }

  const end = () => {
    gesture.current = null
  }

  const keyboardMove = (event: KeyboardEvent<HTMLElement>, tile: Tile) => {
    if (event.target !== event.currentTarget) {
      return
    }
    const deltas: Partial<Record<string, [number, number]>> = {
      ArrowLeft: [-1, 0],
      ArrowRight: [1, 0],
      ArrowUp: [0, -1],
      ArrowDown: [0, 1],
    }
    const delta = deltas[event.key]
    if (delta !== undefined) {
      event.preventDefault()
      onMove(tile.id, [tile.pos[0] + delta[0], tile.pos[1] + delta[1]])
    } else if (event.key === 'Enter' || event.key === ' ') {
      event.preventDefault()
      onSelect(tile.id)
    }
  }

  const keyboardResize = (event: KeyboardEvent<HTMLButtonElement>, tile: Tile) => {
    if (event.key !== 'Enter' && event.key !== ' ') {
      return
    }
    event.preventDefault()
    event.stopPropagation()
    const sizes = definitionFor(tile.type)?.sizes
    if (sizes === undefined || sizes.length === 0) {
      return
    }
    const current = sizes.findIndex(
      (size) => size[0] === tile.size[0] && size[1] === tile.size[1],
    )
    onResize(tile.id, sizes[(current + 1) % sizes.length] ?? sizes[0]!)
  }

  return (
    <section className="canvas" aria-label={`Page ${page.title ?? page.id}`}>
      <div
        ref={surface}
        className="dashboard-grid"
        onPointerMove={move}
        onPointerUp={end}
        onPointerCancel={end}
        onDragOver={(event) => event.preventDefault()}
        onDrop={(event) => {
          event.preventDefault()
          const type = event.dataTransfer.getData('application/x-slate-component')
          if (COMPONENTS.some((component) => component.type === type)) {
            onAdd(type, cellAt(event.clientX, event.clientY))
          }
        }}
      >
        {Array.from({ length: GRID_COLUMNS * GRID_ROWS }, (_, index) => (
          <span
            key={index}
            className="dashboard-grid__cell"
            style={{
              gridColumn: (index % GRID_COLUMNS) + 1,
              gridRow: Math.floor(index / GRID_COLUMNS) + 1,
            }}
            aria-hidden="true"
          />
        ))}
        {page.tiles.map((tile) => {
          const bindings = tile.bindings ?? (tile.binding ? [tile.binding] : [])
          return (
            <article
              key={tile.id}
              className={`grid-tile${tile.id === selectedId ? ' grid-tile--selected' : ''}`}
              style={{
                gridColumn: `${tile.pos[0] + 1} / span ${tile.size[0]}`,
                gridRow: `${tile.pos[1] + 1} / span ${tile.size[1]}`,
              }}
              onPointerDown={(event) => begin(event, tile, 'move')}
              onClick={() => onSelect(tile.id)}
              onKeyDown={(event) => keyboardMove(event, tile)}
              tabIndex={0}
              role="group"
              aria-label={`${tile.type} tile ${tile.label ?? tile.id}; position ${tile.pos[0] + 1}, ${tile.pos[1] + 1}; size ${tile.size[0]} by ${tile.size[1]}`}
            >
              <span className="grid-tile__type">{tile.type}</span>
              <strong>{tile.label ?? bindings[0]?.resource ?? 'Choose a resource'}</strong>
              <span className="grid-tile__binding">
                {bindings.length === 0
                  ? 'not bound'
                  : bindings
                      .map((binding) => `${providerLabel(binding.provider)}: ${binding.resource}`)
                      .join(' · ')}
              </span>
              <span className="grid-tile__size">
                {tile.size[0]}×{tile.size[1]}
              </span>
              {definitionFor(tile.type) !== undefined ? (
                <button
                  type="button"
                  className="grid-tile__resize"
                  aria-label={`Resize ${tile.id}`}
                  title="Drag to a supported size, or press Enter to cycle sizes"
                  onPointerDown={(event) => begin(event, tile, 'resize')}
                  onKeyDown={(event) => keyboardResize(event, tile)}
                />
              ) : null}
            </article>
          )
        })}
      </div>
      <p className="canvas__hint">
        Drag tiles or use arrow keys to move them. The corner handle snaps to supported sizes.
      </p>
    </section>
  )
}
