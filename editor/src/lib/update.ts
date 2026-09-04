/*
 * §11.4's release channel, as prose. The device returns stable codes and no
 * presentation text (ADR-4), so this is where a code becomes a sentence — and
 * it is separate from the panel that renders it so both halves can be tested
 * without a browser.
 */

import type { UpdateStatus } from './api'

export const UPDATE_MESSAGES: Record<string, string> = {
  offline: 'The panel is not on a network, so it could not check.',
  unreachable: 'The update server did not answer.',
  no_release: 'The update channel has no release published yet.',
  release_gone:
    'That release is no longer published. The panel is looking for the current one.',
  manifest_invalid: 'The update server answered with something that is not a Slate manifest.',
  board_mismatch: 'That channel publishes firmware for a different board.',
  schema_too_new: 'That release needs a newer configuration format than this firmware supports.',
  insecure_url: 'The channel tried to serve the image over plain HTTP, and was refused.',
  download_failed: 'The download stopped before the image was complete.',
  checksum_mismatch:
    'The downloaded image did not match the checksum in the manifest, and was discarded.',
  not_an_image: 'What the channel served is not firmware for this board.',
  version_mismatch: 'The image the channel served is not the version its manifest promised.',
  invalid_image: 'The panel refused the downloaded image as damaged.',
  too_large: 'The release does not fit this panel’s firmware slot.',
  no_ota_partition: 'This panel has no second firmware slot to install into.',
  pending_verify: 'The firmware running now is still being verified. Try again in a minute.',
  ota_failed: 'The panel could not write the image to flash.',
  out_of_memory: 'The panel ran out of memory during the update.',
  no_update: 'There is nothing to install.',
  busy: 'The panel is already checking or installing.',
  no_channel: 'This firmware was built without an update channel.',
  /* The panel's request-shape refusals. This editor builds both requests
   * correctly and should never see one, but a code with no sentence reaches a
   * person as the code itself — and these are the ones a second client, or the
   * next route added here, would run into first. */
  unexpected_body: 'The panel refused that request: it carried a body the route does not take.',
  truncated: 'The request did not reach the panel in one piece.',
  invalid_json: 'The panel could not read that request as JSON.',
  invalid_version: 'That request did not name a version the panel could read.',
}

/** One §4 error code as a sentence. An unknown code is shown, never swallowed. */
export function updateError(code: string): string {
  return UPDATE_MESSAGES[code] ?? `The last update job failed: ${code}.`
}

/** What the panel is doing, in one line. */
export function updateHeadline(update: UpdateStatus): string {
  switch (update.state) {
    case 'checking':
      return 'Checking for a release…'
    case 'downloading':
      return `Downloading and verifying… ${updatePercent(update)}%`
    case 'installed':
      return 'Installed. The panel is restarting into the new firmware.'
    default:
      return update.available === null
        ? `Slate ${update.current} is the current release.`
        : `Slate ${update.available.version} is available.`
  }
}

/** Download progress, and 0 before the server has said how long the image is. */
export function updatePercent(update: UpdateStatus): number {
  if (update.progress === null || update.progress.total <= 0) {
    return 0
  }
  const percent = (update.progress.received / update.progress.total) * 100
  return Math.max(0, Math.min(100, Math.round(percent)))
}

/**
 * How long ago the panel looked. Deliberately coarse: this is a daily check,
 * and a number that ticks every second would suggest it is not.
 */
export function checkedAgo(seconds: number): string {
  if (seconds < 90) return 'just now'
  if (seconds < 5400) return `${Math.round(seconds / 60)} min ago`
  if (seconds < 172800) return `${Math.round(seconds / 3600)} h ago`
  return `${Math.round(seconds / 86400)} days ago`
}
