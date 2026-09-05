import type { Binding, Config, Tile } from './api'

export const GRID_COLUMNS = 4
export const GRID_ROWS = 3

export type TileSize = readonly [number, number]

export interface ComponentDefinition {
  type: string
  title: string
  description: string
  sizes: readonly TileSize[]
  defaultSize: TileSize
}

export const COMPONENTS: readonly ComponentDefinition[] = [
  {
    type: 'light',
    title: 'Light',
    description: 'Toggle, brightness and colour temperature when available.',
    sizes: [
      [1, 1],
      [2, 1],
      [2, 2],
    ],
    defaultSize: [1, 1],
  },
  {
    type: 'cover',
    title: 'Cover',
    description: 'Position-aware cover controls.',
    sizes: [
      [1, 1],
      [1, 2],
      [2, 1],
    ],
    defaultSize: [1, 1],
  },
  {
    type: 'sensor',
    title: 'Sensor',
    description: 'Read-only value, unit and an icon the resource chooses.',
    sizes: [
      [1, 1],
      [2, 1],
      [2, 2],
    ],
    defaultSize: [1, 1],
  },
  {
    type: 'scene',
    title: 'Scene',
    description: 'One scene or a full-width scene bar.',
    sizes: [
      [1, 1],
      [4, 1],
    ],
    defaultSize: [1, 1],
  },
]

export function definitionFor(type: string): ComponentDefinition | undefined {
  return COMPONENTS.find((component) => component.type === type)
}

export function tileFits(tiles: readonly Tile[], candidate: Tile, ignoredId?: string): boolean {
  const [column, row] = candidate.pos
  const [width, height] = candidate.size
  if (
    column < 0 ||
    row < 0 ||
    width < 1 ||
    height < 1 ||
    column + width > GRID_COLUMNS ||
    row + height > GRID_ROWS
  ) {
    return false
  }

  return tiles.every((tile) => {
    if (tile.id === ignoredId) {
      return true
    }
    return (
      column + width <= tile.pos[0] ||
      tile.pos[0] + tile.size[0] <= column ||
      row + height <= tile.pos[1] ||
      tile.pos[1] + tile.size[1] <= row
    )
  })
}

export function firstFreePosition(
  tiles: readonly Tile[],
  size: TileSize,
): [number, number] | null {
  for (let row = 0; row <= GRID_ROWS - size[1]; row += 1) {
    for (let column = 0; column <= GRID_COLUMNS - size[0]; column += 1) {
      const candidate = { id: '', type: '', pos: [column, row], size } as Tile
      if (tileFits(tiles, candidate)) {
        return [column, row]
      }
    }
  }
  return null
}

export function nextTileId(config: Config, type: string): string {
  const used = new Set(config.pages.flatMap((page) => page.tiles.map((tile) => tile.id)))
  for (let suffix = 1; ; suffix += 1) {
    const candidate = `${type}-${suffix}`
    if (!used.has(candidate)) {
      return candidate
    }
  }
}

export function bindingFor(tile: Tile): Binding {
  return tile.binding ?? tile.bindings?.[0] ?? { provider: '', resource: '' }
}

export function withSize(tile: Tile, size: TileSize): Tile {
  const next: Tile = { ...tile, size: [size[0], size[1]] }
  if (tile.type === 'scene' && size[0] === 4) {
    const bindings = [...(tile.bindings ?? (tile.binding ? [tile.binding] : []))]
    while (bindings.length < 2) {
      bindings.push({ provider: bindings[0]?.provider ?? '', resource: '' })
    }
    delete next.binding
    next.bindings = bindings
  } else if (tile.bindings !== undefined) {
    const binding = tile.bindings[0]
    delete next.bindings
    if (binding !== undefined) {
      next.binding = binding
    }
  }
  return next
}
