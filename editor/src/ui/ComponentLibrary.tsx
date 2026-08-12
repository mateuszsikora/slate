import { COMPONENTS } from '../lib/editor'

interface Props {
  onAdd: (type: string) => void
}

export function ComponentLibrary({ onAdd }: Props) {
  return (
    <section className="editor-panel library" aria-labelledby="library-title">
      <h3 id="library-title" className="editor-panel__title">
        Components
      </h3>
      <p className="editor-panel__note">Drag one onto the grid, or add it to the first free cell.</p>
      <div className="library__items">
        {COMPONENTS.map((component) => (
          <button
            key={component.type}
            type="button"
            className="library-item"
            draggable
            onDragStart={(event) => {
              event.dataTransfer.effectAllowed = 'copy'
              event.dataTransfer.setData('application/x-slate-component', component.type)
            }}
            onClick={() => onAdd(component.type)}
          >
            <strong>{component.title}</strong>
            <span>{component.description}</span>
          </button>
        ))}
      </div>
    </section>
  )
}
