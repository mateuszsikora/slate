/* The page, theme, publish and connection controls from design.md §10. */

import type { Config, DeviceInfo, ProviderStatus } from '../lib/api'
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
  providers: Pick<ProviderStatus, 'id' | 'status'>[]
  connection: ConnectionState
  mode: 'normal' | 'edit'
  activePageId: string | null
  dirty: boolean
  publishing: boolean
  onSelectPage: (id: string) => void
  onAddPage: () => void
  onThemeChange: (theme: string) => void
  onPublish: () => void
  onToggleMode: () => void
  onUnpair: () => void
}

export function TopBar({
  info,
  config,
  providers,
  connection,
  mode,
  activePageId,
  dirty,
  publishing,
  onSelectPage,
  onAddPage,
  onThemeChange,
  onPublish,
  onToggleMode,
  onUnpair,
}: Props) {
  const pages = config?.pages ?? []
  const themes = info?.themes ?? []

  return (
    <header className="topbar">
      <div className="topbar__identity">
        <span className="topbar__mark">Slate</span>
        <span className="topbar__device">{info?.name ?? 'panel'}</span>
      </div>

      <nav className="topbar__pages" aria-label="Dashboard pages">
        {pages.map((page) => (
          <button
            key={page.id}
            type="button"
            className={`page-tab${page.id === activePageId ? ' page-tab--active' : ''}${
              page.id === config?.home_page ? ' page-tab--home' : ''
            }`}
            onClick={() => onSelectPage(page.id)}
            title={page.id === config?.home_page ? 'Home page' : undefined}
          >
            {page.title ?? page.id}
          </button>
        ))}
        <button type="button" className="page-tab page-tab--add" onClick={onAddPage}>
          + Page
        </button>
      </nav>

      <div className="topbar__controls">
        <label className="topbar__field">
          <span>Theme</span>
          <select
            value={config?.theme ?? themes[0] ?? ''}
            onChange={(event) => onThemeChange(event.currentTarget.value)}
            disabled={config === null || themes.length === 0}
          >
            {themes.map((theme) => (
              <option key={theme} value={theme}>
                {theme}
              </option>
            ))}
          </select>
        </label>

        <div className="topbar__providers" aria-label="Provider connections">
          {providers.map((provider) => (
            <span key={provider.id} className={`provider-dot provider-dot--${provider.status}`}>
              <span aria-hidden="true" />
              {provider.id}
            </span>
          ))}
        </div>

        <button
          type="button"
          className={`mode${mode === 'edit' ? ' mode--on' : ''}`}
          onClick={onToggleMode}
          disabled={connection !== 'online'}
          title="Edit mode makes the panel preview changes and suppresses provider actions"
        >
          {mode === 'edit' ? 'Previewing' : 'Preview'}
        </button>

        <button
          type="button"
          className="button button--publish"
          onClick={onPublish}
          disabled={!dirty || publishing || connection !== 'online'}
        >
          {publishing ? 'Publishing…' : dirty ? 'Publish changes' : 'Published'}
        </button>

        <span className={`indicator indicator--${connection}`} title={CONNECTION_LABEL[connection]}>
          <span className="indicator__dot" aria-hidden="true" />
          <span className="visually-hidden">{CONNECTION_LABEL[connection]}</span>
        </span>

        <button type="button" className="link" onClick={onUnpair} title="Forget the device token">
          Unpair
        </button>
      </div>
    </header>
  )
}
