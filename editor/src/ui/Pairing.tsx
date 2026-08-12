/*
 * Pairing (design.md §4.3).
 *
 * Scanning the QR on the panel opens this page with `?t=` already set and this
 * view is never seen — which is the point of the QR. It is what remains for
 * everyone who arrives another way: a bookmark that outlived a token, a second
 * browser, or a phone that scanned the code and could not open the link.
 */

import { useState, type FormEvent } from 'react'

import type { DeviceInfo } from '../lib/api'
import { parseToken } from '../lib/token'

interface Props {
  info: DeviceInfo | null
  error: string | null
  /* Resolves when the panel has answered, whatever it answered: this view only
   * needs to know when to stop saying "Checking", and the outcome arrives back
   * as `error` or as a different view entirely. */
  onSubmit: (token: string) => Promise<unknown>
}

export function Pairing({ info, error, onSubmit }: Props) {
  const [value, setValue] = useState('')
  const [busy, setBusy] = useState(false)
  const [localError, setLocalError] = useState<string | null>(null)

  /* Whichever came last: a token this view could not parse, or one the panel
   * refused. */
  const failure = localError ?? error

  const submit = (event: FormEvent) => {
    event.preventDefault()
    const token = parseToken(value)
    if (token === null) {
      setLocalError('That is not a device token. It is 32 characters, and the pairing URL contains one.')
      return
    }
    setLocalError(null)
    setBusy(true)
    void onSubmit(token).finally(() => setBusy(false))
  }

  return (
    <main className="pairing">
      <div className="card">
        <h1 className="card__title">Pair with the panel</h1>

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

        <p className="card__note">
          The panel shows a pairing QR with its address and token. Scanning it opens this editor
          already paired. If you cannot scan it, type the token — or paste the whole URL — here.
        </p>

        <form className="pair-form" onSubmit={submit}>
          <input
            className="input"
            type="text"
            inputMode="text"
            autoComplete="off"
            spellCheck={false}
            placeholder="Device token or pairing URL"
            aria-label="Device token or pairing URL"
            value={value}
            onChange={(event) => setValue(event.target.value)}
            disabled={busy}
          />
          <button className="button" type="submit" disabled={busy || value.trim() === ''}>
            {busy ? 'Checking…' : 'Pair'}
          </button>
        </form>

        {failure === null ? null : <p className="error">{failure}</p>}
      </div>
    </main>
  )
}
