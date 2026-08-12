/*
 * §11.3's log stream, in the browser.
 *
 * The retained backlog arrives on every connection, so this panel is populated
 * before anything new happens — which is what makes it useful after a
 * reconnect rather than only during one.
 */

import { useEffect, useRef, useState } from 'react'

import type { ConnectionState } from '../lib/socket'

export interface LogLine {
  id: number
  level: string
  msg: string
}

interface Props {
  lines: LogLine[]
  connection: ConnectionState
}

export function LogPanel({ lines, connection }: Props) {
  const [follow, setFollow] = useState(true)
  const viewport = useRef<HTMLDivElement>(null)

  useEffect(() => {
    if (follow && viewport.current !== null) {
      viewport.current.scrollTop = viewport.current.scrollHeight
    }
  }, [lines, follow])

  /* Scrolling up is how somebody reads a line that has already gone past, so
   * it stops the follow rather than fighting it. */
  const onScroll = () => {
    const element = viewport.current
    if (element === null) {
      return
    }
    const atBottom = element.scrollHeight - element.scrollTop - element.clientHeight < 24
    setFollow(atBottom)
  }

  return (
    <section className="panel panel--logs">
      <h3 className="panel__title">
        Log
        {follow ? null : (
          <button type="button" className="link" onClick={() => setFollow(true)}>
            follow
          </button>
        )}
      </h3>
      <div className="log" ref={viewport} onScroll={onScroll}>
        {lines.length === 0 ? (
          <p className="log__empty">
            {connection === 'online' ? 'Nothing logged yet.' : 'Waiting for the panel…'}
          </p>
        ) : (
          lines.map((line) => (
            <div key={line.id} className={`log__line log__line--${line.level}`}>
              {line.msg}
            </div>
          ))
        )}
      </div>
    </section>
  )
}
