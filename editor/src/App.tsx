/*
 * The editor shell and visual dashboard editor (design.md §10).
 * Pairing and connection state stay here beside #32's draft, preview and
 * publication lifecycle so a reconnect cannot silently publish stale work.
 */

import { useCallback, useEffect, useRef, useState } from 'react'

import {
  ApiError,
  DeviceClient,
  type Config,
  type DeviceInfo,
  type DeviceStatus,
  type Resource,
} from './lib/api'
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
  const [draft, setDraft] = useState<Config | null>(null)
  const [persistedConfig, setPersistedConfig] = useState<Config | null>(null)
  const [activePageId, setActivePageId] = useState<string | null>(null)
  const [dirty, setDirty] = useState(false)
  const [previewState, setPreviewState] = useState<
    'idle' | 'waiting' | 'sending' | 'live' | 'error'
  >('idle')
  const [previewMessage, setPreviewMessage] = useState<string | null>(null)
  const [publishing, setPublishing] = useState(false)
  const [connection, setConnection] = useState<ConnectionState>('connecting')
  const [mode, setMode] = useState<'normal' | 'edit'>('normal')
  const [logs, setLogs] = useState<LogLine[]>([])
  const [movingTo, setMovingTo] = useState<string | null>(null)

  const socketRef = useRef<DeviceSocket | null>(null)
  const logSequence = useRef(0)
  const modeRef = useRef<'normal' | 'edit'>('normal')
  const dirtyRef = useRef(false)
  const previewSequence = useRef(0)

  const client = useCallback((withToken: string | null) => new DeviceClient({ token: withToken }), [])

  const acceptDeviceConfig = useCallback((fetched: Config | null) => {
    previewSequence.current += 1
    setDraft(fetched)
    setPersistedConfig(fetched)
    setDirty(false)
    dirtyRef.current = false
    setPreviewState('idle')
    setPreviewMessage(null)
    setActivePageId((current) => {
      if (fetched?.pages.some((page) => page.id === current)) {
        return current
      }
      return fetched?.home_page ?? fetched?.pages[0]?.id ?? null
    })
  }, [])

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

  /*
   * A token is accepted only after the device has answered with it — the
   * alternative is an editor that looks paired and fails on the first request
   * that matters.
   *
   * "Refused" and "did not answer" are separate outcomes and not one failure,
   * because only the first is about the token. A panel too busy to answer
   * `/status` for five seconds would otherwise throw away a credential that
   * was never wrong, and send somebody to look for a QR code they do not need.
   */
  const admit = useCallback(
    async (candidate: string): Promise<'paired' | 'refused' | 'silent'> => {
      try {
        setStatus(await client(candidate).status())
      } catch (error) {
        if (error instanceof ApiError && error.isUnauthorized) {
          setPairingError('The panel does not know that token. Scan the pairing QR again.')
          return 'refused'
        }
        setPairingError('The panel did not answer. Check that it is on the network.')
        return 'silent'
      }

      storeToken(candidate)
      setToken(candidate)
      setPairingError(null)
      setPhase('ready')
      return 'paired'
    },
    [client],
  )

  const unpair = useCallback(() => {
    if (dirtyRef.current && !window.confirm('Discard unpublished changes and unpair this browser?')) {
      return
    }
    previewSequence.current += 1
    socketRef.current?.setMode('normal')
    socketRef.current?.stop()
    socketRef.current = null
    forgetToken()
    setToken(null)
    setStatus(null)
    setHeartbeat(null)
    setDraft(null)
    setPersistedConfig(null)
    setDirty(false)
    dirtyRef.current = false
    setPreviewState('idle')
    setPreviewMessage(null)
    setMode('normal')
    modeRef.current = 'normal'
    setLogs([])
    setPhase('pairing')
  }, [])

  /**
   * Reach the panel and decide which view this session starts in.
   *
   * Shared by start-up and by the "try again" button, so that a panel which
   * came back answers the same way whichever of the two asked.
   */
  const open = useCallback(async () => {
    const fetched = await loadInfo()
    if (fetched === null) {
      return
    }
    if (token === null) {
      setPhase('pairing')
      return
    }
    const outcome = await admit(token)
    if (outcome === 'refused') {
      setPhase('pairing')
    } else if (outcome === 'silent') {
      /* The token is kept: the panel not answering says nothing about it. */
      setPhase('unreachable')
    }
  }, [admit, loadInfo, token])

  useEffect(() => {
    void open()
    /* Deliberately once, at start-up. Re-pairing goes through admit(), which
     * sets the phase itself, and the retry button calls open() directly. */
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
          modeRef.current = 'normal'
          /* setMode also updates DeviceSocket.desiredMode, preventing a later
           * reconnect from silently restoring edit mode behind the UI. */
          socketRef.current?.setMode('normal')
          if (dirtyRef.current) {
            setPreviewState('waiting')
            setPreviewMessage('Connection lost; the draft remains in this browser.')
          }
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
         * somebody else made. A local transient replacement is already the
         * draft on screen; fetching it back would race the next drag sample. */
        if (dirtyRef.current) {
          return
        }
        void client(token)
          .config()
          .then(acceptDeviceConfig)
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
  }, [phase, token, client, acceptDeviceConfig])

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
      .then(acceptDeviceConfig)
      .catch(() => undefined)
  }, [phase, token, client, acceptDeviceConfig])

  /* --- §4.3's fallback to the mDNS name --------------------------------- */

  /*
   * The address in the bookmark stopped answering. The panel advertises
   * `slate-<mac>.local` for exactly this, so the editor asks that name whether
   * the same panel is there and moves the page — carrying the token, because
   * the name is a different origin with a different localStorage.
   */
  useEffect(() => {
    const name = info?.name
    if (name === undefined || movingTo !== null || dirty || !servedByDevice()) {
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
  }, [phase, connection, dirty, info, token, movingTo])

  /* --- Edit mode (§6.5) -------------------------------------------------- */

  const toggleMode = useCallback(() => {
    const next = mode === 'edit' ? 'normal' : 'edit'
    if (next === 'normal') {
      previewSequence.current += 1
    }
    socketRef.current?.setMode(next)
    modeRef.current = next
    setMode(next)
    if (dirtyRef.current) {
      setPreviewState('waiting')
      setPreviewMessage(next === 'edit' ? 'Waiting to update the panel.' : 'Preview is paused.')
    }
  }, [mode])

  const identifyPanel = useCallback(async () => {
    if (token === null) {
      throw new Error('not paired')
    }
    await client(token).identify()
  }, [client, token])

  const factoryReset = useCallback(async () => {
    if (token === null) {
      throw new Error('not paired')
    }
    await client(token).factoryReset()

    previewSequence.current += 1
    socketRef.current?.stop()
    socketRef.current = null
    forgetToken()
    setToken(null)
    setInfo(null)
    setStatus(null)
    setHeartbeat(null)
    setDraft(null)
    setPersistedConfig(null)
    setDirty(false)
    dirtyRef.current = false
    setMode('normal')
    modeRef.current = 'normal'
    setLogs([])
    setPairingError('Factory reset complete. Wait for the panel to restart, then scan its new pairing QR.')
    setPhase('pairing')
  }, [client, token])

  const changeDraft = useCallback((next: Config) => {
    setDraft(next)
    setDirty(true)
    dirtyRef.current = true
    setPreviewState(modeRef.current === 'edit' ? 'sending' : 'waiting')
    setPreviewMessage(
      modeRef.current === 'edit' ? null : 'Preview is paused. Enter preview mode to update the panel.',
    )
  }, [])

  useEffect(() => {
    if (!dirty || draft === null) {
      return
    }
    if (mode !== 'edit' || connection !== 'online' || token === null) {
      previewSequence.current += 1
      setPreviewState('waiting')
      setPreviewMessage(
        mode !== 'edit'
          ? 'Preview is paused. Enter preview mode to update the panel.'
          : 'The panel is offline; the draft remains in this browser.',
      )
      return
    }

    const sequence = ++previewSequence.current
    setPreviewState('sending')
    const timer = window.setTimeout(() => {
      const document = JSON.stringify(draft)
      void client(token)
        .previewConfig(document)
        .then(() => {
          if (previewSequence.current === sequence) {
            setPreviewState('live')
            setPreviewMessage(null)
          }
        })
        .catch((error: unknown) => {
          if (previewSequence.current === sequence) {
            setPreviewState('error')
            setPreviewMessage(previewError(error))
          }
        })
    }, 300)
    return () => window.clearTimeout(timer)
  }, [client, connection, dirty, draft, mode, token])

  useEffect(() => {
    if (!dirty) {
      return
    }
    const protectDraft = (event: BeforeUnloadEvent) => {
      event.preventDefault()
      event.returnValue = ''
    }
    window.addEventListener('beforeunload', protectDraft)
    return () => window.removeEventListener('beforeunload', protectDraft)
  }, [dirty])

  /* --- Configuration files (§10) --------------------------------------- */

  const validateImportedConfig = useCallback(
    async (document: string) => {
      if (token === null) {
        throw new ApiError(401, 'unauthorized')
      }
      await client(token).validateConfig(document)
    },
    [client, token],
  )

  const publishImportedConfig = useCallback(
    async (document: string, imported: Config) => {
      if (token === null) {
        throw new ApiError(401, 'unauthorized')
      }
      await client(token).publishConfig(document)
      acceptDeviceConfig(imported)
    },
    [acceptDeviceConfig, client, token],
  )

  const publishDraft = useCallback(async () => {
    if (token === null || draft === null || publishing) {
      return
    }
    setPublishing(true)
    setPreviewMessage(null)
    try {
      await client(token).publishConfig(JSON.stringify(draft))
      acceptDeviceConfig(draft)
    } catch (error) {
      setPreviewState('error')
      setPreviewMessage(previewError(error))
    } finally {
      setPublishing(false)
    }
  }, [acceptDeviceConfig, client, draft, publishing, token])

  const discardDraft = useCallback(() => {
    if (!dirtyRef.current || !window.confirm('Discard all unpublished changes?')) {
      return
    }
    previewSequence.current += 1
    socketRef.current?.setMode('normal')
    modeRef.current = 'normal'
    setMode('normal')
    acceptDeviceConfig(persistedConfig)
  }, [acceptDeviceConfig, persistedConfig])

  const loadResources = useCallback(
    async (provider: string): Promise<Resource[]> => {
      if (token === null) {
        throw new ApiError(401, 'unauthorized')
      }
      return client(token).resources(provider)
    },
    [client, token],
  )

  const createDashboard = useCallback(() => {
    const firstTheme = info?.themes[0] ?? 'midnight'
    const next: Config = {
      schema: 1,
      theme: firstTheme,
      home_page: 'home',
      pages: [{ id: 'home', title: 'Home', tiles: [] }],
    }
    setActivePageId('home')
    changeDraft(next)
  }, [changeDraft, info])

  const addPage = useCallback(() => {
    if (draft === null) {
      createDashboard()
      return
    }
    const used = new Set(draft.pages.map((page) => page.id))
    let suffix = draft.pages.length + 1
    while (used.has(`page-${suffix}`)) suffix += 1
    const id = `page-${suffix}`
    changeDraft({
      ...draft,
      home_page: draft.home_page ?? draft.pages[0]?.id ?? id,
      pages: [...draft.pages, { id, title: `Page ${suffix}`, tiles: [] }],
    })
    setActivePageId(id)
  }, [changeDraft, createDashboard, draft])

  useEffect(() => {
    if (draft === null || draft.pages.length === 0) {
      setActivePageId(null)
      return
    }
    if (!draft.pages.some((page) => page.id === activePageId)) {
      setActivePageId(draft.home_page ?? draft.pages[0]?.id ?? null)
    }
  }, [activePageId, draft])

  const retry = useCallback(() => {
    setPhase('starting')
    void open()
  }, [open])

  const providers =
    heartbeat !== null
      ? Object.entries(heartbeat.providers).map(([id, providerStatus]) => ({
          id,
          status: providerStatus,
        }))
      : (status?.providers.map((providerStatus) => ({
          id: providerStatus.id,
          status: providerStatus.status,
        })) ?? [])

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
        config={draft}
        providers={providers}
        connection={connection}
        mode={mode}
        activePageId={activePageId}
        dirty={dirty}
        publishing={publishing}
        onSelectPage={setActivePageId}
        onAddPage={addPage}
        onThemeChange={(theme) => {
          if (draft !== null) changeDraft({ ...draft, theme })
        }}
        onPublish={() => void publishDraft()}
        onDiscard={discardDraft}
        onToggleMode={toggleMode}
        onUnpair={unpair}
      />
      <div className="shell__body">
        <Workspace
          config={draft}
          activePageId={activePageId}
          providers={providers}
          previewState={previewState}
          previewMessage={previewMessage}
          dirty={dirty}
          deviceName={info?.name ?? 'slate'}
          onCreate={createDashboard}
          onChange={changeDraft}
          onLoadResources={loadResources}
          onValidateConfig={validateImportedConfig}
          onPublishConfig={publishImportedConfig}
        />
        <aside className="sidebar">
          <DevicePanel
            info={info}
            status={status}
            heartbeat={heartbeat}
            onIdentify={identifyPanel}
            onFactoryReset={factoryReset}
          />
          <LogPanel lines={logs} connection={connection} />
        </aside>
      </div>
    </div>
  )
}

function previewError(error: unknown): string {
  if (!(error instanceof ApiError)) {
    return 'The panel could not apply the preview.'
  }
  if (error.code === 'unreachable') {
    return 'The panel did not answer; the draft remains in this browser.'
  }
  if (error.code === 'edit_mode_required') {
    return 'Enter preview mode before sending live changes.'
  }
  if (error.code === 'invalid_config' && typeof error.body === 'object' && error.body !== null) {
    const body = error.body as {
      config_errors?: { code?: string }[]
      tile_errors?: Record<string, { code?: string }[]>
    }
    const first = body.config_errors?.[0]?.code
    if (first !== undefined) return `The draft is not valid yet (${first}).`
    const tileEntry = Object.entries(body.tile_errors ?? {})[0]
    if (tileEntry !== undefined) {
      const [tileId, errors] = tileEntry
      const code = errors[0]?.code
      return code === undefined
        ? `Tile ${tileId} is not valid yet.`
        : `Tile ${tileId} is not valid yet (${code}).`
    }
  }
  return `The panel rejected the preview (${error.code}).`
}
