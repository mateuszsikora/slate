/*
 * JSON import and export from design.md §10.
 *
 * An imported document remains a browser-side candidate until the device has
 * validated it and the user explicitly publishes it. Keeping those steps
 * separate means selecting the wrong file cannot replace a working dashboard.
 */

import { useRef, useState, type ChangeEvent } from 'react'

import { ApiError, type Config } from '../lib/api'

interface ImportedConfig {
  config: Config
  document: string
  filename: string
}

interface Failure {
  summary: string
  details: string[]
}

interface Props {
  config: Config | null
  deviceName: string
  hasUnpublishedChanges: boolean
  onValidate: (document: string) => Promise<void>
  onPublish: (document: string, config: Config) => Promise<void>
}

type Activity = 'idle' | 'validating' | 'publishing'

/** The browser enforces the same document-size boundary as design.md §3.1. */
const CONFIG_MAX_BYTES = 64 * 1024

const VALIDATION_MESSAGES: Record<string, string> = {
  schema_required: 'The schema version is required.',
  schema_invalid: 'The schema version must be a positive integer.',
  schema_too_new: 'This configuration requires newer panel firmware.',
  theme_required: 'A theme is required.',
  theme_not_found: 'The selected theme is not available on this panel.',
  pages_required: 'At least one page is required.',
  duplicate_page_id: 'Page IDs must be unique.',
  home_page_not_found: 'The home page does not match any page ID.',
  tile_id_required: 'Every tile needs an ID.',
  duplicate_tile_id: 'Tile IDs must be unique.',
  invalid_position: 'The tile position is invalid.',
  invalid_size: 'The tile size is not supported.',
  tile_out_of_bounds: 'The tile extends beyond the grid.',
  tile_overlap: 'The tile overlaps another tile.',
  binding_required: 'The tile requires a resource binding.',
  provider_required: 'The binding requires a provider.',
  resource_required: 'The binding requires a resource.',
}

export function ConfigTransfer({
  config,
  deviceName,
  hasUnpublishedChanges,
  onValidate,
  onPublish,
}: Props) {
  const input = useRef<HTMLInputElement>(null)
  const [activity, setActivity] = useState<Activity>('idle')
  const [candidate, setCandidate] = useState<ImportedConfig | null>(null)
  const [failure, setFailure] = useState<Failure | null>(null)
  const [notice, setNotice] = useState<string | null>(null)

  const exportConfig = () => {
    if (config === null) {
      return
    }

    const json = `${JSON.stringify(config, null, 2)}\n`
    const url = URL.createObjectURL(new Blob([json], { type: 'application/json' }))
    const link = document.createElement('a')
    link.href = url
    link.download = `${safeFilename(deviceName)}-config.json`
    document.body.append(link)
    link.click()
    link.remove()
    window.setTimeout(() => URL.revokeObjectURL(url), 0)
    setNotice(`Exported ${link.download}.`)
    setFailure(null)
  }

  const chooseFile = () => {
    input.current?.click()
  }

  const importConfig = async (event: ChangeEvent<HTMLInputElement>) => {
    const file = event.currentTarget.files?.[0]
    event.currentTarget.value = ''
    if (file === undefined) {
      return
    }

    setActivity('validating')
    setCandidate(null)
    setFailure(null)
    setNotice(`Validating ${file.name}…`)

    let imported: { document: string; parsed: unknown }
    try {
      imported = await readImportedFile(file)
    } catch (error) {
      setFailure({
        summary: 'Import failed. The running configuration was not changed.',
        details: [fileError(error)],
      })
      setNotice(null)
      setActivity('idle')
      return
    }

    try {
      await onValidate(imported.document)
      setCandidate({
        config: imported.parsed as Config,
        document: imported.document,
        filename: file.name,
      })
      setNotice(`${file.name} is valid and ready to publish.`)
    } catch (error) {
      setFailure({
        summary: 'Import failed. The running configuration was not changed.',
        details: requestErrors(error, 'validate'),
      })
      setNotice(null)
    } finally {
      setActivity('idle')
    }
  }

  const publish = async () => {
    if (candidate === null) {
      return
    }
    if (
      hasUnpublishedChanges &&
      !window.confirm('Replace the unpublished visual-editor draft with this imported configuration?')
    ) {
      return
    }

    setActivity('publishing')
    setFailure(null)
    setNotice(`Publishing ${candidate.filename}…`)
    try {
      await onPublish(candidate.document, candidate.config)
      setNotice(`Published ${candidate.filename}. The configuration is stored on the panel.`)
      setCandidate(null)
    } catch (error) {
      setFailure({
        summary: 'Publishing did not complete cleanly. Check the panel before retrying.',
        details: requestErrors(error, 'publish'),
      })
      setNotice(null)
    } finally {
      setActivity('idle')
    }
  }

  const busy = activity !== 'idle'

  return (
    <section className="transfer" aria-labelledby="transfer-title">
      <div className="transfer__header">
        <div>
          <h3 id="transfer-title" className="configuration__title">
            Configuration file
          </h3>
          <p className="transfer__description">
            Keep a backup across reflashes or share this layout with another panel.
          </p>
        </div>
        <div className="transfer__actions">
          <button
            type="button"
            className="button button--secondary"
            onClick={exportConfig}
            disabled={config === null || busy}
          >
            Export JSON
          </button>
          <button type="button" className="button" onClick={chooseFile} disabled={busy}>
            {activity === 'validating' ? 'Validating…' : 'Import JSON'}
          </button>
          <input
            ref={input}
            className="visually-hidden"
            type="file"
            accept="application/json,.json"
            onChange={(event) => void importConfig(event)}
            tabIndex={-1}
          />
        </div>
      </div>

      {notice !== null && (
        <p className="transfer__notice" role="status">
          {notice}
        </p>
      )}

      {failure !== null && (
        <div className="transfer__errors" role="alert">
          <p>{failure.summary}</p>
          <ul>
            {failure.details.map((error, index) => (
              <li key={`${index}-${error}`}>{error}</li>
            ))}
          </ul>
        </div>
      )}

      {candidate !== null && (
        <div className="transfer__candidate">
          <span>
            <strong>{candidate.filename}</strong> passed validation on this panel.
          </span>
          <button type="button" className="button" onClick={() => void publish()} disabled={busy}>
            {activity === 'publishing' ? 'Publishing…' : 'Publish imported configuration'}
          </button>
        </div>
      )}
    </section>
  )
}

function safeFilename(name: string): string {
  const safe = name
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9._-]+/g, '-')
    .replace(/^-+|-+$/g, '')
  return safe || 'slate'
}

class ImportedFileError extends Error {}

async function readImportedFile(file: File): Promise<{ document: string; parsed: unknown }> {
  if (file.size > CONFIG_MAX_BYTES) {
    throw new ImportedFileError('The document exceeds the 64 KB configuration limit.')
  }

  let bytes: ArrayBuffer
  try {
    bytes = await file.arrayBuffer()
  } catch {
    throw new ImportedFileError('The file could not be read.')
  }

  let document: string
  try {
    /* `fatal` prevents invalid input from being silently changed to U+FFFD.
     * Keeping the BOM instead of consuming it lets JSON.parse and the device
     * judge the same bytes rather than two normalized variants of the file. */
    document = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true }).decode(bytes)
  } catch {
    throw new ImportedFileError('The document must be valid UTF-8.')
  }

  try {
    return { document, parsed: JSON.parse(document) as unknown }
  } catch (error) {
    const detail = error instanceof SyntaxError ? `: ${error.message}` : ''
    throw new ImportedFileError(`Invalid JSON${detail}`)
  }
}

function fileError(error: unknown): string {
  return error instanceof ImportedFileError ? error.message : 'The file could not be read.'
}

function requestErrors(error: unknown, operation: 'validate' | 'publish'): string[] {
  if (!(error instanceof ApiError)) {
    return [
      operation === 'publish'
        ? 'The configuration could not be published.'
        : 'The configuration could not be checked.',
    ]
  }
  if (error.code === 'unreachable') {
    return [
      operation === 'publish'
        ? 'The panel did not answer. The request may have reached it; inspect the running configuration before retrying.'
        : 'The panel did not answer. Try again when it is reachable.',
    ]
  }
  if (error.code === 'too_large') {
    return ['The document exceeds the 64 KB configuration limit.']
  }
  if (error.code === 'store_failed') {
    return [
      'The panel activated the document in memory but could not persist it. A reboot restores the previous stored configuration.',
    ]
  }
  if (error.code === 'empty_body') {
    return ['The document is empty.']
  }
  if (error.code === 'invalid_json') {
    return ['The panel could not parse the document as UTF-8 JSON.']
  }

  const issues = detailedIssues(error.body)
  if (issues.length > 0) {
    return issues
  }
  return [`The panel rejected the document (${error.code}).`]
}

function detailedIssues(body: unknown): string[] {
  if (!isObject(body)) {
    return []
  }

  const messages = issueList(body['config_errors'])
  const tileErrors = body['tile_errors']
  if (isObject(tileErrors)) {
    for (const [tile, issues] of Object.entries(tileErrors)) {
      messages.push(...issueList(issues, `Tile ${tile}: `))
    }
  }
  return messages
}

function issueList(value: unknown, prefix = ''): string[] {
  if (!Array.isArray(value)) {
    return []
  }
  return value.flatMap((issue) => {
    if (!isObject(issue) || typeof issue['code'] !== 'string') {
      return []
    }
    const code = issue['code']
    const message = VALIDATION_MESSAGES[code] ?? 'The configuration is invalid.'
    const path = typeof issue['path'] === 'string' ? ` at ${issue['path']}` : ''
    return [`${prefix}${message} (${code})${path}`]
  })
}

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
}
