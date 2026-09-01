import { useState, type FormEvent } from 'react'

import type { DeviceInfo } from '../lib/api'

interface Props {
  info: DeviceInfo | null
  error: string | null
  onSubmit: (pin: string) => Promise<unknown>
}

export function Unlock({ info, error, onSubmit }: Props) {
  const [pin, setPin] = useState('')
  const [busy, setBusy] = useState(false)

  const submit = (event: FormEvent) => {
    event.preventDefault()
    if (!/^[0-9]{4,12}$/.test(pin)) return
    setBusy(true)
    void onSubmit(pin).finally(() => setBusy(false))
  }

  return (
    <main className="access-view">
      <div className="card">
        <h1 className="card__title">Open the editor</h1>

        {info === null ? null : (
          <dl className="facts">
            <dt>Device</dt>
            <dd>{info.name}</dd>
            <dt>Model</dt>
            <dd>{info.model}</dd>
            <dt>Firmware</dt>
            <dd>{info.firmware_version}</dd>
            <dt>Address</dt>
            <dd>{info.network.ip ?? '—'}</dd>
          </dl>
        )}

        <p className="card__note">Enter the administrator PIN configured for this panel.</p>

        <form className="unlock-form" onSubmit={submit}>
          <input
            className="input"
            type="password"
            inputMode="numeric"
            pattern="[0-9]*"
            minLength={4}
            maxLength={12}
            autoComplete="current-password"
            placeholder="Administrator PIN"
            aria-label="Administrator PIN"
            value={pin}
            onChange={(event) => setPin(event.currentTarget.value.replace(/\D/g, '').slice(0, 12))}
            autoFocus
            disabled={busy}
          />
          <button className="button" type="submit" disabled={busy || pin.length < 4}>
            {busy ? 'Opening…' : 'Open'}
          </button>
        </form>

        {error === null ? null : <p className="error">{error}</p>}
        <p className="card__note">Forgot the PIN? Hold the panel for 10 seconds to factory-reset it.</p>
      </div>
    </main>
  )
}
