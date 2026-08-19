/*
 * The panel is not answering at this address.
 *
 * §4.3: the token outlives a DHCP lease, so the address this page was opened
 * at may simply have moved. The editor probes `slate-<mac>.local` in the
 * background; this view is what the person sees while that happens.
 */

interface Props {
  /** The origin the editor is moving to, once the panel has answered there. */
  movingTo: string | null
  onRetry: () => void
}

export function Unreachable({ movingTo, onRetry }: Props) {
  return (
    <main className="access-view">
      <div className="card">
        <h1 className="card__title">
          {movingTo === null ? 'The panel is not answering' : 'Found it — moving over'}
        </h1>
        {movingTo === null ? (
          <p className="card__note">
            Nothing answered at <code>{window.location.host}</code>. If its address changed, the
            panel still advertises itself by name and this page is trying that now. The address is
            also on the panel's own screen.
          </p>
        ) : (
          <p className="card__note">
            The panel answered at <code>{movingTo}</code>. Opening the editor there with the same
            token.
          </p>
        )}
        <button className="button" type="button" onClick={onRetry}>
          Try again
        </button>
      </div>
    </main>
  )
}
