/*
 * The device token, as §4.3 delivers it.
 *
 * It arrives in the URL of the pairing QR — `http://192.168.1.42/?t=Xk7p…` —
 * is kept in localStorage, and is taken out of the address bar as soon as it
 * has been kept: the URL a token is in is the URL a browser puts in its
 * history and shares in a screenshot, and it is the same reason §4.2 refuses
 * to accept the token in the WebSocket URL.
 */

const STORAGE_KEY = 'slate.device_token'

/** §4.3's token: 32 characters drawn from the URL-safe base64 alphabet. */
const TOKEN_PATTERN = /^[A-Za-z0-9_-]{32}$/

export function isToken(value: string): boolean {
  return TOKEN_PATTERN.test(value)
}

/**
 * A token out of anything a person can reasonably hand this field: the token
 * itself, or the whole pairing URL they copied out of a phone that scanned the
 * QR and then would not open the page.
 */
export function parseToken(input: string): string | null {
  const trimmed = input.trim()
  if (isToken(trimmed)) {
    return trimmed
  }
  try {
    const fromQuery = new URL(trimmed).searchParams.get('t')
    return fromQuery !== null && isToken(fromQuery) ? fromQuery : null
  } catch {
    return null
  }
}

export function readStoredToken(): string | null {
  try {
    const stored = window.localStorage.getItem(STORAGE_KEY)
    return stored !== null && isToken(stored) ? stored : null
  } catch {
    /* Private browsing, or storage denied for this origin. The editor still
     * works for as long as the tab is open; it just pairs again next time. */
    return null
  }
}

export function storeToken(token: string): void {
  try {
    window.localStorage.setItem(STORAGE_KEY, token)
  } catch {
    /* See above. Not being able to remember the token is not a reason to
     * refuse to use it. */
  }
}

export function forgetToken(): void {
  try {
    window.localStorage.removeItem(STORAGE_KEY)
  } catch {
    /* Nothing to do: there was nothing to forget. */
  }
}

/**
 * Take the token out of `?t=` and out of the address bar.
 *
 * Returns it whether or not the URL is rewritten — a page opened from a
 * `file://` origin or an old browser that refuses `replaceState` should still
 * pair.
 */
export function claimTokenFromUrl(): string | null {
  const token = new URLSearchParams(window.location.search).get('t')
  if (token === null || !isToken(token)) {
    return null
  }

  storeToken(token)
  try {
    const url = new URL(window.location.href)
    url.searchParams.delete('t')
    window.history.replaceState(null, '', url.pathname + url.search + url.hash)
  } catch {
    /* The token is stored; a URL that still shows it is worse than this catch
     * but better than not pairing. */
  }
  return token
}
