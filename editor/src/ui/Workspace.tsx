/*
 * Where §10's grid, inspector and library mount (#32).
 *
 * Until they land this area says what the panel is currently showing, read out
 * of `GET /config`. It deliberately does not draw the 4 × 3 grid: that view is
 * #32's, and a half-built one here would be the thing #32 has to delete before
 * it can start.
 */

import type { Config } from '../lib/api'
import { ConfigTransfer } from './ConfigTransfer'

interface Props {
  config: Config | null
  deviceName: string
  onValidateConfig: (document: string) => Promise<void>
  onPublishConfig: (document: string, config: Config) => Promise<void>
}

export function Workspace({ config, deviceName, onValidateConfig, onPublishConfig }: Props) {
  return (
    <main className="workspace">
      <div className="workspace__pending">
        <h2>The grid arrives with #32</h2>
        <p>
          This shell pairs with the panel, keeps the connection and reports what it is running.
          Arranging tiles — the grid, the inspector, the library and the live preview — is the next
          issue. Configuration files can already be backed up, shared and restored below.
        </p>
      </div>

      <ConfigTransfer
        config={config}
        deviceName={deviceName}
        onValidate={onValidateConfig}
        onPublish={onPublishConfig}
      />

      <section className="configuration">
        <h3 className="configuration__title">Running configuration</h3>
        {config === null ? (
          <p className="configuration__empty">
            The panel has no configuration. It is showing the error screen of §6.5 until one is
            published.
          </p>
        ) : (
          <ul className="pages">
            {config.pages.map((page) => (
              <li key={page.id} className="pages__page">
                <div className="pages__header">
                  <span className="pages__title">{page.title ?? page.id}</span>
                  <span className="pages__count">
                    {page.tiles.length} {page.tiles.length === 1 ? 'tile' : 'tiles'}
                  </span>
                </div>
                <ul className="tiles">
                  {page.tiles.map((tile) => {
                    const bindings = tile.bindings ?? (tile.binding ? [tile.binding] : [])
                    return (
                      <li key={tile.id} className="tiles__tile">
                        <span className="tiles__type">{tile.type}</span>
                        <span className="tiles__geometry">
                          {tile.size[0]}×{tile.size[1]} at [{tile.pos[0]}, {tile.pos[1]}]
                        </span>
                        <span className="tiles__binding">
                          {bindings.length === 0
                            ? '—'
                            : bindings.map((binding) => `${binding.provider}:${binding.resource}`).join(', ')}
                        </span>
                      </li>
                    )
                  })}
                </ul>
              </li>
            ))}
          </ul>
        )}
      </section>
    </main>
  )
}
