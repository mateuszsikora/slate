import type { Config } from '../lib/api'
import { timezoneFor, withTimezone } from '../lib/settings'
import { TIMEZONES } from '../generated/timezones'

interface Props {
  config: Config
  onChange: (config: Config) => void
  onClose: () => void
}

const SUPPORTED_TIMEZONES = new Set<string>(TIMEZONES)

export function SettingsDialog({ config, onChange, onClose }: Props) {
  const timezone = timezoneFor(config)
  const unknownTimezone = timezone !== null && !SUPPORTED_TIMEZONES.has(timezone)

  return (
    <div className="dialog-backdrop" role="presentation" onMouseDown={onClose}>
      <section
        className="settings-dialog"
        role="dialog"
        aria-modal="true"
        aria-labelledby="settings-title"
        onMouseDown={(event) => event.stopPropagation()}
      >
        <header className="dialog-header">
          <div>
            <span className="dialog-eyebrow">Dashboard settings</span>
            <h2 id="settings-title">Panel behavior</h2>
          </div>
          <button type="button" className="dialog-close" onClick={onClose} aria-label="Close">
            ×
          </button>
        </header>

        <div className="settings-dialog__body">
          <label className="field settings-dialog__field">
            <span>Time zone</span>
            <select
              value={timezone ?? ''}
              onChange={(event) =>
                onChange(
                  withTimezone(
                    config,
                    event.currentTarget.value === '' ? null : event.currentTarget.value,
                  ),
                )
              }
            >
              <option value="">UTC — firmware default</option>
              {unknownTimezone ? (
                <option value={timezone ?? ''}>Unsupported by this firmware: {timezone}</option>
              ) : null}
              {TIMEZONES.map((zone) => (
                <option key={zone} value={zone}>
                  {zone}
                </option>
              ))}
            </select>
          </label>
          <p>
            The clock and night schedule use this IANA zone, including daylight-saving changes.
            This list is generated from the same timezone table compiled into the panel.
          </p>
          <p>Changes stay in the draft until you publish them.</p>
        </div>
      </section>
    </div>
  )
}
