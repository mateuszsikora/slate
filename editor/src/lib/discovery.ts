/*
 * Finding the panel again after its address changed (DESIGN.md §4.3).
 *
 * The address in a bookmark does not have to be the address the panel has
 * today. The device advertises itself as `slate-<mac6>.local`, and that name is
 * what this editor falls back to when the origin it was served from stops
 * answering.
 */

import { API_BASE, type DeviceInfo } from './api'

/** `slate-71a2e4` → `http://slate-71a2e4.local`. */
export function mdnsOrigin(deviceName: string): string {
  return `http://${deviceName}.local`
}

/**
 * Whether the panel with this name answers at `origin`.
 *
 * The name is checked, not just the response: a bare 200 from something at
 * that address is not evidence that it is the same panel. Moving the editor to
 * a different device is exactly the mistake worth one extra comparison.
 */
export async function deviceAnswersAt(origin: string, name: string, timeoutMs = 4000): Promise<boolean> {
  const controller = new AbortController()
  const timer = window.setTimeout(() => controller.abort(), timeoutMs)
  try {
    const response = await fetch(`${origin}${API_BASE}/info`, {
      signal: controller.signal,
      cache: 'no-store',
    })
    if (!response.ok) {
      return false
    }
    const info = (await response.json()) as DeviceInfo
    return info.name === name
  } catch {
    return false
  } finally {
    window.clearTimeout(timer)
  }
}

/**
 * Whether this page is one the device served.
 *
 * `npm run dev` proxies the API to a panel from a workstation, and moving that
 * tab to the panel's own address would take the editor being edited away with
 * it.
 */
export function servedByDevice(): boolean {
  const host = window.location.hostname
  return host !== 'localhost' && host !== '127.0.0.1' && host !== '[::1]'
}
