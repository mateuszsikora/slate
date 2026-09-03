/*
 * §11.4's release channel, from the editor: what is running, what is offered,
 * and the one button that installs it.
 *
 * The panel checks once a day on its own and this page never changes that
 * schedule. What it does is show the answer, let somebody ask for a fresh one,
 * and carry the accept — which is the whole user-facing part of "installation
 * only on explicit request". The confirmation says the panel will restart,
 * because that is the part somebody standing in front of a wall panel cares
 * about, and that the dashboard and credentials are kept, because §11.4's
 * promise is worth stating where it is being relied on.
 */

import { useCallback, useEffect, useState } from 'react'

import { ApiError, type UpdateStatus } from '../lib/api'
import { checkedAgo, updateError, updateHeadline, updatePercent } from '../lib/update'

interface Props {
  onFetch: () => Promise<UpdateStatus>
  onCheck: () => Promise<void>
  onInstall: (version: string) => Promise<void>
}

/* Idle is a day-scale question and does not deserve a poll a second. A job in
 * flight is: a download is a couple of minutes with a bar on it. */
const IDLE_POLL_MS = 60000
const BUSY_POLL_MS = 1500

export function UpdatePanel({ onFetch, onCheck, onInstall }: Props) {
  const [update, setUpdate] = useState<UpdateStatus | null>(null)
  const [failure, setFailure] = useState<string | null>(null)
  const [pending, setPending] = useState(false)

  /* A failed fetch keeps the last answer rather than blanking the section: the
   * device stops answering the moment it reboots into the image it was just
   * told to install, and that is the success case, not a fault to report. */
  const refresh = useCallback(async () => {
    try {
      const fetched = await onFetch()
      setUpdate(fetched)
      return fetched
    } catch {
      return null
    }
  }, [onFetch])

  useEffect(() => {
    let cancelled = false
    let timer = 0

    const tick = () => {
      void refresh().then((fetched) => {
        if (cancelled) return
        const busy = fetched?.state === 'checking' || fetched?.state === 'downloading'
        timer = window.setTimeout(tick, busy ? BUSY_POLL_MS : IDLE_POLL_MS)
      })
    }

    tick()
    return () => {
      cancelled = true
      window.clearTimeout(timer)
    }
  }, [refresh])

  if (update === null) {
    return null
  }

  const run = (action: () => Promise<void>) => {
    setPending(true)
    setFailure(null)
    void action()
      .then(() => refresh())
      .catch((error: unknown) => {
        setFailure(
          error instanceof ApiError && error.code !== 'unknown'
            ? updateError(error.code)
            : 'The panel did not accept that request.',
        )
      })
      .finally(() => setPending(false))
  }

  const install = () => {
    const offer = update.available
    if (offer === null) return
    const confirmed = window.confirm(
      `Install Slate ${offer.version}? The panel downloads the release, verifies its checksum ` +
        'and restarts. The dashboard, credentials and settings are kept.',
    )
    if (confirmed) run(() => onInstall(offer.version))
  }

  const busy = update.state === 'checking' || update.state === 'downloading'
  const frozen = busy || pending || update.state === 'installed'
  const percent = updatePercent(update)

  return (
    <section className="panel">
      <h3 className="panel__title">Firmware updates</h3>

      {update.manifest_url === null ? (
        <p className="update__note">
          This firmware was built without an update channel, so the panel never checks for
          releases. Installing one is <code>tools/ota/upload.sh</code>.
        </p>
      ) : (
        <>
          <p className="update__state" role="status" aria-live="polite">
            {updateHeadline(update)}
          </p>

          {update.state === 'downloading' ? (
            <div
              className="update__progress"
              role="progressbar"
              aria-valuenow={percent}
              aria-valuemin={0}
              aria-valuemax={100}
            >
              <span style={{ width: `${percent}%` }} />
            </div>
          ) : null}

          {update.error !== null ? <p className="warning">{updateError(update.error)}</p> : null}

          {failure !== null ? (
            <p className="error" role="alert">
              {failure}
            </p>
          ) : null}

          <div className="device-actions">
            <button
              type="button"
              className="button button--secondary"
              disabled={frozen}
              onClick={() => run(onCheck)}
            >
              {update.state === 'checking' ? 'Checking…' : 'Check now'}
            </button>
            {update.available !== null ? (
              <button type="button" className="button" disabled={frozen} onClick={install}>
                Install {update.available.version}
              </button>
            ) : null}
          </div>

          <p className="update__note">
            {update.checked_s_ago === null
              ? 'The panel has not checked yet; it looks once a day.'
              : `Checked ${checkedAgo(update.checked_s_ago)}. The panel looks once a day and installs nothing on its own.`}
          </p>
        </>
      )}
    </section>
  )
}
