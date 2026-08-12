/*
 * The editor shell (design.md §10, issue #31).
 *
 * What lives here is everything the editor needs before there is anything to
 * edit: pairing with the token from §4.3's QR, the connection to the device
 * and the frame the grid, inspector and library of #32 mount into.
 */

import { useCallback, useEffect, useRef, useState } from 'react'

import { ApiError, DeviceClient, type Config, type DeviceInfo, type DeviceStatus } from './lib/api'
import { deviceAnswersAt, mdnsOrigin, servedByDevice } from './lib/discovery'
import { DeviceSocket, type ConnectionState, type LogFrame, type StatusFrame } from './lib/socket'
import { claimTokenFromUrl, forgetToken, readStoredToken, storeToken } from './lib/token'
import { DevicePanel } from './ui/DevicePanel'
import { LogPanel, type LogLine } from './ui/LogPanel'
import { Pairing } from './ui/Pairing'
import { TopBar } from './ui/TopBar'
import { Unreachable } from './ui/Unreachable'
import { Workspace } from './ui/Workspace'

/** §11.3 keeps an 8 KB ring on the device; this is the browser's share of it. */
const LOG_LIMIT = 400

/** `GET /status` carries what the 15 s heartbeat does not: uptime, reset reason, storage. */
const STATUS_POLL_MS = 30000

type Phase = 'starting' | 'pairing' | 'ready' | 'unreachable'

export function App() {
  const [token, setToken] = useState<string | null>(() => claimTokenFromUrl() ?? readStoredToken())
  const [phase, setPhase] = useState<Phase>('starting')
  const [pairingError, setPairingError] = useState<string | null>(null)
  const [info, setInfo] = useState<DeviceInfo | null>(null)
  const [status, setStatus] = useState<DeviceStatus | null>(null)
  const [heartbeat, setHeartbeat] = useState<StatusFrame | null>(null)
  const [config, setConfig] = useState<Config | null>(null)
  const [connection, setConnection] = useState<ConnectionState>('connecting')
  const [mode, setMode] = useState<'normal' | 'edit'>('normal')
  const [logs, setLogs] = useState<LogLine[]>([])
  const [movingTo, setMovingTo] = useState<string | null>(null)

  const socketRef = useRef<DeviceSocket | null>(null)
  const logSequence = useRef(0)

  const client = useCallback((withToken: string | null) => new DeviceClient({ token: withToken }), [])

  /* --- Pairing ---------------------------------------------------------- */

  /*
   * `GET /info` is the one route that answers without a token (§4.1), so it is
   * both the reachability check and what the pairing view has to show: a panel
   * nobody has paired with yet can still say which panel it is.
   */
  const loadInfo = useCallback(async (): Promise<DeviceInfo | null> => {
    try {
      const fetched = await client(null).info()
      setInfo(fetched)
      return fetched
    } catch {
      setPhase('unreachable')
      return null
    }
  }, [client])

  const admit = useCallback(
    async (candidate: string): Promise<boolean> => {
      /* A token is accepted only after the device has answered with it. The
       * alternative is an editor that looks paired and fails on the first
       * request that matters. */
      try {
        setStatus(await client(candidate).status())
      } catch (error) {
        if (error instanceof ApiError && error.isUnauthorized) {
          setPairingError('The panel does not know that token. Scan the pairing QR again.')
          return false
        }
        setPairingError('The panel did not answer. Check that it is on the network.')
        return false
      }

      storeToken(candidate)
      setToken(candidate)
      setPairingError(null)
      setPhase('ready')
      return true
    },
    [client],
  )

  const unpair = useCallback(() => {
    socketRef.current?.stop()
    socketRef.current = null
    forgetToken()
    setToken(null)
    setStatus(null)
    setHeartbeat(null)
    setConfig(null)
    setLogs([])
    setPhase('pairing')
  }, [])

  useEffect(() => {
    let cancelled = false
    void (async () => {
      const fetched = await loadInfo()
      if (cancelled || fetched === null) {
        return
      }
      if (token === null) {
        setPhase('pairing')
        return
      }
      if (!(await admit(token))) {
        if (!cancelled) {
          setPhase('pairing')
        }
      }
    })()
    return () => {
      cancelled = true
    }
    /* Deliberately once, at start-up: re-pairing goes through admit(), which
     * sets the phase itself rather than re-running this. */
  }, [])

  /* --- The live connection (§4.2) --------------------------------------- */

  useEffect(() => {
    if (phase !== 'ready' || token === null) {
      return
    }

    const socket = new DeviceSocket('', token, {
      onState: (state) => {
        setConnection(state)
        if (state === 'unauthorized') {
          setPairingError('The panel refused the token. Scan the pairing QR again.')
          setPhase('pairing')
        }
        if (state !== 'online') {
          /* The panel returns to normal 60 s after the pings stop (§4.2), so
           * the badge must not keep claiming edit mode through an outage. */
          setMode('normal')
        }
      },
      onStatus: setHeartbeat,
      onLog: (log: LogFrame) => {
        setLogs((previous) => {
          const line: LogLine = { id: logSequence.current++, level: log.level, msg: log.msg }
          const next = previous.length >= LOG_LIMIT ? previous.slice(1) : previous.slice()
          next.push(line)
          return next
        })
      },
      onReloaded: () => {
        /* §4.1 publishes this after every activated replacement, including one
         * somebody else made. The document on screen is stale from here. */
        void client(token)
          .config()
          .then(setConfig)
          .catch(() => undefined)
      },
    })

    socketRef.current = socket
    socket.start()

    const leave = () => socket.setMode('normal')
    window.addEventListener('pagehide', leave)

    return () => {
      window.removeEventListener('pagehide', leave)
      leave()
      socket.stop()
      socketRef.current = null
    }
  }, [phase, token, client])

  /* --- What the heartbeat does not carry -------------------------------- */

  useEffect(() => {
    if (phase !== 'ready' || token === null) {
      return
    }

    let cancelled = false
    const poll = () => {
      client(token)
        .status()
        .then((fetched) => {
          if (!cancelled) {
            setStatus(fetched)
          }
        })
        .catch(() => undefined)
    }

    const timer = window.setInterval(poll, STATUS_POLL_MS)
    poll()
    return () => {
      cancelled = true
      window.clearInterval(timer)
    }
  }, [phase, token, client])

  useEffect(() => {
    if (phase !== 'ready' || token === null) {
      return
    }
    client(token)
      .config()
      .then(setConfig)
      .catch(() => undefined)
  }, [phase, token, client])

  /* --- §4.3's fallback to the mDNS name --------------------------------- */

  /*
   * The address in the bookmark stopped answering. The panel advertises
   * `slate-<mac>.local` for exactly this, so the editor asks that name whether
   * the same panel is there and moves the page — carrying the token, because
   * the name is a different origin with a different localStorage.
   */
  useEffect(() => {
    const name = info?.name
    if (name === undefined || movingTo !== null || !servedByDevice()) {
      return
    }
    const offline = phase === 'unreachable' || (phase === 'ready' && connection === 'offline')
    if (!offline) {
      return
    }

    const origin = mdnsOrigin(name)
    if (window.location.origin === origin) {
      return
    }

    let cancelled = false
    const timer = window.setTimeout(() => {
      void deviceAnswersAt(origin, name).then((answered) => {
        if (answered && !cancelled) {
          setMovingTo(origin)
          const target = token === null ? origin : `${origin}/?t=${encodeURIComponent(token)}`
          window.location.assign(target)
        }
      })
      /* Not immediately: §9.4's first reconnect attempt is a second away, and
       * a page that jumps origin during a two-second router hiccup is worse
       * than one that waits for the hiccup to end. */
    }, 5000)

    return () => {
      cancelled = true
      window.clearTimeout(timer)
    }
  }, [phase, connection, info, token, movingTo])

  /* --- Edit mode (§6.5) -------------------------------------------------- */

  const toggleMode = useCallback(() => {
    const next = mode === 'edit' ? 'normal' : 'edit'
    socketRef.current?.setMode(next)
    setMode(next)
  }, [mode])

  const retry = useCallback(() => {
    setPhase('starting')
    void (async () => {
      const fetched = await loadInfo()
      if (fetched === null) {
        return
      }
      if (token === null || !(await admit(token))) {
        setPhase('pairing')
      }
    })()
  }, [admit, loadInfo, token])

  /* --- Views ------------------------------------------------------------- */

  if (movingTo !== null) {
    return <Unreachable movingTo={movingTo} onRetry={retry} />
  }

  if (phase === 'starting') {
    return (
      <div className="boot">
        <span className="boot__mark">Slate</span>
        <span className="boot__note">Reaching the panel…</span>
      </div>
    )
  }

  if (phase === 'unreachable') {
    return <Unreachable movingTo={null} onRetry={retry} />
  }

  if (phase === 'pairing') {
    return <Pairing info={info} error={pairingError} onSubmit={admit} />
  }

  return (
    <div className="shell">
      <TopBar
        info={info}
        config={config}
        connection={connection}
        mode={mode}
        onToggleMode={toggleMode}
        onUnpair={unpair}
      />
      <div className="shell__body">
        <Workspace config={config} />
        <aside className="sidebar">
          <DevicePanel info={info} status={status} heartbeat={heartbeat} />
          <LogPanel lines={logs} connection={connection} />
        </aside>
      </div>
    </div>
  )
}
