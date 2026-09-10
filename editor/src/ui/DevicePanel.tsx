/*
 * What the panel is doing, from the two places §4 puts it: the 15 s `status`
 * heartbeat of §4.2 and `GET /status` for the fields the heartbeat leaves out.
 */

import { useState } from 'react'

import type { DeviceInfo, DeviceStatus } from '../lib/api'
import { providerLabel } from '../lib/providers'
import type { StatusFrame } from '../lib/socket'

interface Props {
  info: DeviceInfo | null
  status: DeviceStatus | null
  heartbeat: StatusFrame | null
  onIdentify: () => Promise<void>
  onFactoryReset: () => Promise<void>
}

function uptime(seconds: number): string {
  const days = Math.floor(seconds / 86400)
  const hours = Math.floor((seconds % 86400) / 3600)
  const minutes = Math.floor((seconds % 3600) / 60)
  if (days > 0) {
    return `${days} d ${hours} h`
  }
  if (hours > 0) {
    return `${hours} h ${minutes} min`
  }
  return `${minutes} min`
}

function kilobytes(bytes: number): string {
  return `${Math.round(bytes / 1024).toLocaleString()} KB`
}

export function DevicePanel({ info, status, heartbeat, onIdentify, onFactoryReset }: Props) {
  const [identifying, setIdentifying] = useState(false)
  const [resetting, setResetting] = useState(false)
  const [actionError, setActionError] = useState<string | null>(null)
  /* The heartbeat is the fresher of the two for everything it carries. */
  const providers =
    heartbeat !== null
      ? Object.entries(heartbeat.providers).map(([id, state]) => ({ id, status: state }))
      : (status?.providers.map((provider) => ({ id: provider.id, status: provider.status })) ?? [])
  const rssi = heartbeat?.wifi ?? status?.rssi
  const heap = heartbeat?.heap_free ?? status?.heap_free

  return (
    <section className="panel">
      <h3 className="panel__title">Device</h3>

      <div className="providers">
        {providers.map((provider) => (
          <span key={provider.id} className={`chip chip--${provider.status}`}>
            {providerLabel(provider.id)}
            <span className="chip__state">{provider.status}</span>
          </span>
        ))}
      </div>

      <dl className="facts facts--compact">
        <dt>Address</dt>
        <dd>{info?.network.ip ?? '—'}</dd>
        <dt>Network</dt>
        <dd>{info?.network.ssid ?? info?.network.sta_ssid ?? '—'}</dd>
        <dt>Signal</dt>
        <dd>{rssi === undefined ? '—' : `${rssi} dBm`}</dd>
        <dt>Uptime</dt>
        <dd>{status === null ? '—' : uptime(status.uptime_s)}</dd>
        <dt>Free heap</dt>
        <dd>{heap === undefined ? '—' : kilobytes(heap)}</dd>
        <dt>LVGL heap</dt>
        <dd>
          {heartbeat === null && status === null
            ? '—'
            : `${kilobytes(heartbeat?.lvgl_heap_free ?? status?.lvgl_heap_free ?? 0)} free, ${
                heartbeat?.lvgl_frag_pct ?? status?.lvgl_frag_pct ?? 0
              }% fragmented`}
        </dd>
        <dt>Resources</dt>
        <dd>{status?.resource_count ?? '—'}</dd>
        <dt>Last reset</dt>
        <dd>{status === null ? '—' : `${status.reset_reason}, boot #${status.reboot_count}`}</dd>
        <dt>Firmware</dt>
        <dd>{info?.firmware_version ?? '—'}</dd>
      </dl>

      {status?.storage_reset === true ? (
        <p className="warning">
          Storage was reset during boot: the stored configuration and credentials are gone.
        </p>
      ) : null}

      <div className="device-actions">
        <button
          type="button"
          className="button button--secondary"
          disabled={identifying || resetting}
          onClick={() => {
            setIdentifying(true)
            setActionError(null)
            void onIdentify()
              .catch(() => setActionError('The panel could not start the identify flash.'))
              .finally(() => setIdentifying(false))
          }}
        >
          {identifying ? 'Identifying…' : 'Identify panel'}
        </button>
        <button
          type="button"
          className="button button--danger"
          disabled={identifying || resetting}
          onClick={() => {
            const confirmed = window.confirm(
              'Factory reset this panel? This permanently erases the dashboard, Wi-Fi settings, administrator security settings, External API keys, Home Assistant credentials, and Tuya credentials.',
            )
            if (!confirmed) return
            setResetting(true)
            setActionError(null)
            void onFactoryReset()
              .catch(() => {
                setActionError('Factory reset did not complete. The panel may still reboot.')
                setResetting(false)
              })
          }}
        >
          {resetting ? 'Resetting…' : 'Factory reset'}
        </button>
      </div>

      {actionError !== null ? (
        <p className="error" role="alert">
          {actionError}
        </p>
      ) : null}
    </section>
  )
}
