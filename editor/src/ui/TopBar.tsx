/*
 * §10's top bar: pages, theme, the device connection indicator — and the
 * publish button, which arrives with the grid that gives it something to
 * publish (#32).
 */

import type { Config, DeviceInfo } from '../lib/api'
import type { ConnectionState } from '../lib/socket'

const CONNECTION_LABEL: Record<ConnectionState, string> = {
  connecting: 'Connecting',
  online: 'Connected',
  offline: 'Offline',
  unauthorized: 'Not paired',
}

interface Props {
  info: DeviceInfo | null
  config: Config | null
  connection: ConnectionState
  mode: 'normal' | 'edit'
  onToggleMode: () => void
  onUnpair: () => void
}

export function TopBar({ info, config, connection, mode, onToggleMode, onUnpair }: Props) {
  const pages = config?.pages ?? []
  const home = config?.home_page

  return (
    <header className="topbar">
      <div className="topbar__identity">
        <span className="topbar__mark">Slate</span>
        <span className="topbar__device">{info?.name ?? 'panel'}</span>
      </div>

      <nav className="topbar__pages" aria-label="Pages">
        {pages.length === 0 ? (
          <span className="topbar__empty">no pages</span>
        ) : (
          pages.map((page) => (
            <span
              key={page.id}
              className={`page-tab${page.id === home ? ' page-tab--home' : ''}`}
              title={page.id === home ? 'The page the panel starts on' : undefined}
            >
              {page.title ?? page.id}
            </span>
          ))
        )}
      </nav>

      <div className="topbar__controls">
        <span className="theme" title="The theme this configuration selects">
          {config?.theme ?? info?.themes[0] ?? '—'}
        </span>

        <button
          type="button"
          className={`mode${mode === 'edit' ? ' mode--on' : ''}`}
          onClick={onToggleMode}
          disabled={connection !== 'online'}
          title="Edit mode stops the panel emitting actions on touch (§6.5)"
        >
          {mode === 'edit' ? 'Edit mode' : 'Normal mode'}
        </button>

        <span className={`indicator indicator--${connection}`}>
          <span className="indicator__dot" aria-hidden="true" />
          {CONNECTION_LABEL[connection]}
        </span>

        <button type="button" className="link" onClick={onUnpair} title="Forget the device token">
          Unpair
        </button>
      </div>
    </header>
  )
}
