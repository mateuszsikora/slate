import { useEffect, useRef, useState, type FormEvent } from 'react'

import {
  ApiError,
  type CreatedIntegrationKey,
  type DeviceClient,
  type HaConfiguration,
  type HaDiscoveredInstance,
  type IntegrationKey,
  type ProviderStatus,
  type TuyaConfiguration,
} from '../lib/api'
import { providerReason } from '../lib/providers'

interface Props {
  client: DeviceClient
  providers: Pick<ProviderStatus, 'id' | 'status' | 'reason'>[]
  onChanged: () => void
  onClose: () => void
}

const EMPTY_HA: HaConfiguration = { configured: false, url: null }
const EMPTY_TUYA: TuyaConfiguration = { configured: false, region: null, uid: null }

const TUYA_REGIONS: { value: string; label: string }[] = [
  { value: 'eu', label: 'Europe (eu)' },
  { value: 'us', label: 'Americas (us)' },
  { value: 'cn', label: 'China (cn)' },
  { value: 'in', label: 'India (in)' },
  { value: 'ueaz', label: 'US East Azure (ueaz)' },
  { value: 'weaz', label: 'EU West Azure (weaz)' },
]

export function IntegrationsDialog({ client, providers, onChanged, onClose }: Props) {
  const [ha, setHa] = useState<HaConfiguration>(EMPTY_HA)
  const [haUrl, setHaUrl] = useState('')
  const [haToken, setHaToken] = useState('')
  const [haInstances, setHaInstances] = useState<HaDiscoveredInstance[]>([])
  const [haDiscoveryDone, setHaDiscoveryDone] = useState(false)
  const [haBusy, setHaBusy] = useState(false)
  const [haMessage, setHaMessage] = useState<string | null>(null)
  const [tuya, setTuya] = useState<TuyaConfiguration>(EMPTY_TUYA)
  const [tuyaRegion, setTuyaRegion] = useState('eu')
  const [tuyaAccessId, setTuyaAccessId] = useState('')
  const [tuyaSecret, setTuyaSecret] = useState('')
  const [tuyaUid, setTuyaUid] = useState('')
  const [tuyaBusy, setTuyaBusy] = useState(false)
  const [tuyaMessage, setTuyaMessage] = useState<string | null>(null)
  const [keys, setKeys] = useState<IntegrationKey[]>([])
  const [keyName, setKeyName] = useState('')
  const [keyBusy, setKeyBusy] = useState(false)
  const [keyMessage, setKeyMessage] = useState<string | null>(null)
  const [created, setCreated] = useState<CreatedIntegrationKey | null>(null)
  const tokenInput = useRef<HTMLInputElement>(null)

  useEffect(() => {
    let cancelled = false

    void client
      .haConfiguration()
      .then((configuration) => {
        if (cancelled) return
        setHa(configuration)
        setHaUrl(configuration.url ?? '')
      })
      .catch((error) => {
        if (!cancelled) setHaMessage(apiMessage(error, 'Home Assistant settings could not be loaded.'))
      })

    void client
      .discoverHomeAssistant()
      .then((instances) => {
        if (cancelled) return
        setHaInstances(instances)
        setHaDiscoveryDone(true)
        if (instances.length === 1) {
          setHaUrl((current) => current || instances[0]!.url)
        }
      })
      .catch(() => {
        if (!cancelled) setHaDiscoveryDone(true)
      })

    void client
      .tuyaConfiguration()
      .then((configuration) => {
        if (cancelled) return
        setTuya(configuration)
        if (configuration.region !== null) setTuyaRegion(configuration.region)
        setTuyaUid(configuration.uid ?? '')
      })
      .catch((error) => {
        if (!cancelled) setTuyaMessage(apiMessage(error, 'Tuya settings could not be loaded.'))
      })

    void client
      .integrationKeys()
      .then((fetched) => {
        if (!cancelled) setKeys(fetched)
      })
      .catch((error) => {
        if (!cancelled) setKeyMessage(apiMessage(error, 'External API keys could not be loaded.'))
      })

    return () => {
      cancelled = true
    }
  }, [client])

  const requestClose = () => {
    if (
      created !== null &&
      !window.confirm('This key is shown only once. Close without copying it?')
    ) {
      return
    }
    setCreated(null)
    onClose()
  }

  const configureHa = async (event: FormEvent) => {
    event.preventDefault()
    setHaBusy(true)
    setHaMessage(null)
    try {
      await client.configureHomeAssistant(haUrl.trim(), haToken)
      const configuration = await client.haConfiguration()
      setHa(configuration)
      setHaUrl(configuration.url ?? haUrl.trim())
      setHaToken('')
      setHaMessage('Connected. Home Assistant resources are now available in the tile picker.')
      onChanged()
    } catch (error) {
      setHaMessage(haError(error))
    } finally {
      setHaBusy(false)
    }
  }

  const disconnectHa = async () => {
    if (!window.confirm('Disconnect Home Assistant and remove its stored token?')) return
    setHaBusy(true)
    setHaMessage(null)
    try {
      await client.disconnectHomeAssistant()
      setHa(EMPTY_HA)
      setHaUrl('')
      setHaToken('')
      setHaMessage('Home Assistant disconnected.')
      onChanged()
    } catch (error) {
      setHaMessage(apiMessage(error, 'Home Assistant could not be disconnected.'))
    } finally {
      setHaBusy(false)
    }
  }

  const configureTuya = async (event: FormEvent) => {
    event.preventDefault()
    setTuyaBusy(true)
    setTuyaMessage(null)
    try {
      await client.configureTuya(tuyaRegion, tuyaAccessId.trim(), tuyaSecret, tuyaUid.trim())
      const configuration = await client.tuyaConfiguration()
      setTuya(configuration)
      setTuyaAccessId('')
      setTuyaSecret('')
      setTuyaMessage('Connected. Tuya resources are now available in the tile picker.')
      onChanged()
    } catch (error) {
      setTuyaMessage(tuyaError(error))
    } finally {
      setTuyaBusy(false)
    }
  }

  const disconnectTuya = async () => {
    if (!window.confirm('Disconnect Tuya and remove its stored credentials?')) return
    setTuyaBusy(true)
    setTuyaMessage(null)
    try {
      await client.disconnectTuya()
      setTuya(EMPTY_TUYA)
      setTuyaAccessId('')
      setTuyaSecret('')
      setTuyaMessage('Tuya disconnected.')
      onChanged()
    } catch (error) {
      setTuyaMessage(apiMessage(error, 'Tuya could not be disconnected.'))
    } finally {
      setTuyaBusy(false)
    }
  }

  const createKey = async (event: FormEvent) => {
    event.preventDefault()
    setKeyBusy(true)
    setKeyMessage(null)
    setCreated(null)
    try {
      const key = await client.createIntegrationKey(keyName.trim())
      setKeys((current) => [...current, { id: key.id, name: key.name }])
      setCreated(key)
      setKeyName('')
      setKeyMessage('Key created. Copy it now; the panel cannot show it again.')
      window.setTimeout(() => tokenInput.current?.select(), 0)
    } catch (error) {
      setKeyMessage(apiMessage(error, 'The key could not be created.'))
    } finally {
      setKeyBusy(false)
    }
  }

  const revokeKey = async (key: IntegrationKey) => {
    if (!window.confirm(`Revoke “${key.name}”? Its active connection will close immediately.`)) {
      return
    }
    setKeyBusy(true)
    setKeyMessage(null)
    try {
      await client.revokeIntegrationKey(key.id)
      setKeys((current) => current.filter((entry) => entry.id !== key.id))
      if (created?.id === key.id) setCreated(null)
      setKeyMessage(`Revoked “${key.name}”.`)
    } catch (error) {
      setKeyMessage(apiMessage(error, 'The key could not be revoked.'))
    } finally {
      setKeyBusy(false)
    }
  }

  const copyCreatedKey = async () => {
    if (created === null) return
    try {
      await navigator.clipboard.writeText(created.token)
      setKeyMessage('Key copied.')
    } catch {
      tokenInput.current?.select()
      document.execCommand('copy')
      setKeyMessage('Key selected. Copy it before closing this window.')
    }
  }

  const haStatus = providers.find((provider) => provider.id === 'ha')?.status ?? 'unknown'
  const tuyaProvider = providers.find((provider) => provider.id === 'tuya')
  const tuyaStatus = tuyaProvider?.status ?? 'unknown'
  const tuyaStatusReason = providerReason(tuyaProvider?.reason)
  const externalStatus =
    providers.find((provider) => provider.id === 'direct')?.status ?? 'unknown'

  return (
    <div className="dialog-backdrop" onMouseDown={(event) => event.target === event.currentTarget && requestClose()}>
      <section className="integrations-dialog" role="dialog" aria-modal="true" aria-labelledby="integrations-title">
        <header className="dialog-header">
          <div>
            <span className="dialog-eyebrow">Panel settings</span>
            <h2 id="integrations-title">Integrations</h2>
          </div>
          <button type="button" className="dialog-close" onClick={requestClose} aria-label="Close integrations">
            ×
          </button>
        </header>

        <div className="integration-grid">
          <article className="integration-card">
            <div className="integration-card__heading">
              <div>
                <h3>Home Assistant</h3>
                <p>Use entities and actions from your local Home Assistant instance.</p>
              </div>
              <span className={`chip chip--${haStatus}`}>{haStatus}</span>
            </div>

            {haInstances.length > 0 ? (
              <div className="discovered-list">
                <span>Discovered on this network</span>
                {haInstances.map((instance) => (
                  <button type="button" key={instance.uuid || instance.url} onClick={() => setHaUrl(instance.url)}>
                    <strong>{instance.name || 'Home Assistant'}</strong>
                    <span>{instance.url}</span>
                  </button>
                ))}
              </div>
            ) : haDiscoveryDone ? (
              <p className="integration-note">No instance was discovered. Enter its local URL manually.</p>
            ) : (
              <p className="integration-note">Looking for Home Assistant on this network…</p>
            )}

            <form className="integration-form" onSubmit={(event) => void configureHa(event)}>
              <label className="field">
                <span>Home Assistant URL</span>
                <input
                  type="url"
                  value={haUrl}
                  placeholder="http://homeassistant.local:8123"
                  required
                  disabled={haBusy}
                  onChange={(event) => setHaUrl(event.currentTarget.value)}
                />
              </label>
              <label className="field">
                <span>{ha.configured ? 'New long-lived access token' : 'Long-lived access token'}</span>
                <input
                  type="password"
                  value={haToken}
                  autoComplete="new-password"
                  required
                  disabled={haBusy}
                  onChange={(event) => setHaToken(event.currentTarget.value)}
                />
              </label>
              <p className="integration-note">
                In Home Assistant, open your profile and create a long-lived token under Security. Use a dedicated account in the <code>system-users</code> group.
              </p>
              <div className="integration-actions">
                <button className="button" type="submit" disabled={haBusy || haUrl.trim() === '' || haToken === ''}>
                  {haBusy ? 'Testing…' : ha.configured ? 'Test and replace' : 'Test and connect'}
                </button>
                {ha.configured ? (
                  <button className="button button--danger" type="button" disabled={haBusy} onClick={() => void disconnectHa()}>
                    Disconnect
                  </button>
                ) : null}
              </div>
            </form>
            {haMessage !== null ? <p className="integration-message" role="status">{haMessage}</p> : null}
          </article>

          <article className="integration-card">
            <div className="integration-card__heading">
              <div>
                <h3>Tuya</h3>
                <p>Use devices from your Tuya Smart or Smart Life app through the Tuya cloud.</p>
              </div>
              <span className={`chip chip--${tuyaStatus}`}>{tuyaStatus}</span>
            </div>
            <p className="integration-note" role="status" aria-live="polite">
              {tuyaStatusReason ?? `Tuya status: ${tuyaStatus}.`}
            </p>

            <form className="integration-form" onSubmit={(event) => void configureTuya(event)}>
              <label className="field">
                <span>Region</span>
                <select
                  value={tuyaRegion}
                  required
                  disabled={tuyaBusy}
                  onChange={(event) => setTuyaRegion(event.currentTarget.value)}
                >
                  {TUYA_REGIONS.map((region) => (
                    <option key={region.value} value={region.value}>
                      {region.label}
                    </option>
                  ))}
                </select>
              </label>
              <label className="field">
                <span>Access ID</span>
                <input
                  value={tuyaAccessId}
                  placeholder="jt4kpqtkheqmwkcxdrew"
                  required
                  disabled={tuyaBusy}
                  onChange={(event) => setTuyaAccessId(event.currentTarget.value)}
                />
              </label>
              <label className="field">
                <span>{tuya.configured ? 'New Access Secret' : 'Access Secret'}</span>
                <input
                  type="password"
                  value={tuyaSecret}
                  autoComplete="new-password"
                  required
                  disabled={tuyaBusy}
                  onChange={(event) => setTuyaSecret(event.currentTarget.value)}
                />
              </label>
              <label className="field">
                <span>App account UID</span>
                <input
                  value={tuyaUid}
                  placeholder="az1680123456789ABCDE"
                  required
                  disabled={tuyaBusy}
                  onChange={(event) => setTuyaUid(event.currentTarget.value)}
                />
              </label>
              <p className="integration-note">
                Create a cloud project at iot.tuya.com, link your Smart Life or Tuya Smart app to it, and enter its Access ID, Access Secret, region and app account UID.
              </p>
              <div className="integration-actions">
                <button
                  className="button"
                  type="submit"
                  disabled={tuyaBusy || tuyaAccessId.trim() === '' || tuyaSecret === '' || tuyaUid.trim() === ''}
                >
                  {tuyaBusy ? 'Testing…' : tuya.configured ? 'Test and replace' : 'Test and connect'}
                </button>
                {tuya.configured ? (
                  <button className="button button--danger" type="button" disabled={tuyaBusy} onClick={() => void disconnectTuya()}>
                    Disconnect
                  </button>
                ) : null}
              </div>
            </form>
            {tuyaMessage !== null ? <p className="integration-message" role="status">{tuyaMessage}</p> : null}
          </article>

          <article className="integration-card">
            <div className="integration-card__heading">
              <div>
                <h3>External API</h3>
                <p>Push normalized state from a script, Node-RED, or another local application.</p>
              </div>
              <span className={`chip chip--${externalStatus}`}>{externalStatus}</span>
            </div>

            <div className="integration-flow" aria-label="External API data flow">
              <span>Your app</span><b>→ state →</b><span>Slate</span><b>→ actions →</b><span>Your app</span>
            </div>
            <p className="integration-note">
              External API does not poll arbitrary URLs. Publish complete resource snapshots to Slate; optionally keep a WebSocket open to receive panel actions.
            </p>

            <div className="key-list">
              <div className="key-list__header"><span>Access keys</span><small>{keys.length}/4</small></div>
              {keys.length === 0 ? <p>No keys yet.</p> : keys.map((key) => (
                <div className="key-row" key={key.id}>
                  <div><strong>{key.name}</strong><code>{key.id}</code></div>
                  <button type="button" className="link link--danger" disabled={keyBusy} onClick={() => void revokeKey(key)}>Revoke</button>
                </div>
              ))}
            </div>

            <form className="key-create" onSubmit={(event) => void createKey(event)}>
              <label className="field">
                <span>Key name</span>
                <input
                  value={keyName}
                  maxLength={32}
                  placeholder="Node-RED downstairs"
                  required
                  disabled={keyBusy || keys.length >= 4}
                  onChange={(event) => setKeyName(event.currentTarget.value)}
                />
              </label>
              <button className="button" type="submit" disabled={keyBusy || keys.length >= 4 || keyName.trim() === ''}>
                {keyBusy ? 'Creating…' : 'Create key'}
              </button>
            </form>

            {created !== null ? (
              <div className="created-key">
                <strong>Copy this key now</strong>
                <div>
                  <input ref={tokenInput} readOnly value={created.token} aria-label="New External API key" />
                  <button type="button" className="button button--secondary" onClick={() => void copyCreatedKey()}>Copy</button>
                </div>
                <span>It is stored only as a hash and cannot be displayed again.</span>
              </div>
            ) : null}

            <details className="api-example">
              <summary>Publish a light resource</summary>
              <pre>{`export SLATE_API_KEY='paste-key-here'
curl -X POST ${window.location.origin}/api/v1/direct/state \\
  -H "Authorization: Bearer $SLATE_API_KEY" \\
  -H 'Content-Type: application/json' \\
  -d '{"resource":"living-room","kind":"light","available":true,"state":{"power":"on","brightness":62}}'`}</pre>
              <p>Bind a tile to provider <code>direct</code> and resource <code>living-room</code>, then publish the dashboard before sending its first state.</p>
              <p>To receive touch actions, run the repository reference client with the same environment variable: <code>tools/direct/agent.py {window.location.host} --resource living-room</code>.</p>
            </details>
            {keyMessage !== null ? <p className="integration-message" role="status">{keyMessage}</p> : null}
          </article>
        </div>
      </section>
    </div>
  )
}

function apiMessage(error: unknown, fallback: string): string {
  if (!(error instanceof ApiError)) return fallback
  if (error.code === 'unreachable') return 'The panel did not answer.'
  if (error.code === 'key_limit') return 'This panel already has four External API keys.'
  if (error.code === 'name_invalid') return 'Use a name between 1 and 32 characters.'
  return `${fallback} (${error.code})`
}

function haError(error: unknown): string {
  if (!(error instanceof ApiError)) return 'Home Assistant could not be configured.'
  if (error.code === 'ha_auth_invalid') return 'Home Assistant rejected this token.'
  if (error.code === 'ha_unreachable') return 'The panel could not reach Home Assistant at this URL.'
  if (error.code === 'bad_url') return 'Enter an http:// or https:// Home Assistant URL.'
  if (error.code === 'unreachable') return 'The panel stopped answering while testing the connection.'
  return `Home Assistant could not be configured (${error.code}).`
}

const TUYA_FIELD_CODES = new Set([
  'region_required',
  'region_unknown',
  'access_id_required',
  'access_id_too_long',
  'secret_required',
  'secret_too_long',
  'uid_required',
  'uid_too_long',
])

function tuyaError(error: unknown): string {
  if (!(error instanceof ApiError)) return 'Tuya could not be configured.'
  if (error.code === 'tuya_auth_failed') return 'Tuya rejected those credentials.'
  if (error.code === 'tuya_quota') return 'Tuya cloud subscription or quota problem — check iot.tuya.com.'
  if (error.code === 'tuya_unreachable') return 'Could not reach the Tuya cloud.'
  if (error.code === 'tuya_busy') return 'Panel busy — try again.'
  if (TUYA_FIELD_CODES.has(error.code)) return 'Check the four fields.'
  if (error.code === 'unreachable') return 'The panel stopped answering while testing the connection.'
  return `Tuya could not be configured (${error.code}).`
}
