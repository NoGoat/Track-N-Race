import type { CSSProperties } from 'react'
import { Upload, X } from 'lucide-react'
import { useTelemetryStore } from '../../stores/telemetryStore'
import { SESSION_TYPES } from '../../components/SessionPanel'
import type { SecondaryFileData } from '../../components/AnalyzeScreen'
import { fileDateLabels, recordingSessionLabel } from '../../lib/secondaryFile'

const NO_DRAG = { WebkitAppRegion: 'no-drag' } as CSSProperties

interface TitlebarFilesProps {
  filename: string | null | undefined
  sessionType: number | undefined
  /** Session-type colour for the single-file badge, or null when offline. */
  accent: string | null
  /** Hover colour class for the close buttons (the two headers differ). */
  closeHoverClass: string
  onClosePlayback: () => void
  onSelectPlaybackFile: () => void
  secondaryFile: SecondaryFileData | null
  secondaryLoading: boolean
  secondaryError: string | null
  onSelectSecondaryFile: () => void
  onCloseSecondaryFile: () => void
}

/**
 * The title bar's file area.
 *  - No recording: the upload button (unchanged).
 *  - File A open: filename, close, session badge, then the upload button again,
 *    which opens the same desktop file dialog for the second file.
 *  - Both open: one chip per file — date in its file colour, track, session,
 *    close — with the full file name in the tooltip.
 */
export default function TitlebarFiles({
  filename, sessionType, accent, closeHoverClass, onClosePlayback, onSelectPlaybackFile,
  secondaryFile, secondaryLoading, secondaryError, onSelectSecondaryFile, onCloseSecondaryFile,
}: TitlebarFilesProps) {
  const trackName = useTelemetryStore(state => state.playbackTrackName)
  const badge = accent ? (
    <span className="text-[10px] font-medium uppercase tracking-wide rounded px-2 py-0.5 select-none shrink-0" style={{ backgroundColor: accent + '22', color: accent }}>{SESSION_TYPES[sessionType!] ?? 'Unknown'}</span>
  ) : (
    <span className="text-[10px] font-medium uppercase tracking-wide rounded px-2 py-0.5 select-none shrink-0 bg-[var(--bg-panel)] text-[var(--text-secondary)]">Offline</span>
  )

  if (!filename) {
    return <>
      <button onClick={onSelectPlaybackFile} title="Load Session Data File (.tnrd)" style={NO_DRAG} className="p-1.5 rounded text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-colors shrink-0"><Upload size={14} /></button>
      {badge}
    </>
  }

  if (!secondaryFile) {
    return <>
      <div className="flex items-center gap-1.5 shrink-0">
        <span className="titlebar-filename text-[11px] font-mono text-[var(--text-secondary)] truncate">{filename}</span>
        <button onClick={onClosePlayback} title="Close session data" style={NO_DRAG} className={`p-1 text-[var(--text-secondary)] ${closeHoverClass} transition-colors`}><X size={14} /></button>
      </div>
      {badge}
      <button
        onClick={onSelectSecondaryFile}
        disabled={secondaryLoading}
        title={secondaryError ?? (secondaryLoading ? 'Loading the second file…' : 'Load a second file to compare (.tnrd)')}
        aria-label="Load a second file to compare"
        style={NO_DRAG}
        className={`p-1.5 rounded ${secondaryError ? 'text-[#d44252]' : 'text-[var(--text-secondary)]'} hover:text-[var(--text-primary)] hover:bg-[var(--bg-hover)] transition-colors shrink-0 disabled:opacity-40`}
      ><Upload size={14} /></button>
    </>
  }

  const [dateA, dateB] = fileDateLabels(filename, secondaryFile.filename)
  const sessionA = (sessionType !== undefined ? SESSION_TYPES[sessionType] : undefined) ?? recordingSessionLabel(filename)
  return (
    // Shrinks when the title bar runs out of room: the track names truncate
    // first, so the controls to the right never get covered. A squeezed name
    // tells the title bar to make room elsewhere (useTitlebarFit).
    <div data-titlebar-squeeze className="flex items-center min-w-0 overflow-hidden">
      <FileChip
        label={dateA ?? filename} colorVar="--file-a" track={trackName} session={sessionA}
        title={filename} closeTitle="Close session data" closeHoverClass={closeHoverClass} onClose={onClosePlayback}
      />
      <span className="w-px h-4 bg-[var(--border)] mx-1.5 shrink-0" />
      <FileChip
        label={dateB ?? secondaryFile.filename} colorVar="--file-b" track={secondaryFile.trackName}
        session={recordingSessionLabel(secondaryFile.filename)}
        title={secondaryFile.filename} closeTitle="Close the second file" closeHoverClass={closeHoverClass} onClose={onCloseSecondaryFile}
      />
    </div>
  )
}

function FileChip({ label, colorVar, track, session, title, closeTitle, closeHoverClass, onClose }: {
  label: string
  colorVar: '--file-a' | '--file-b'
  track: string | null
  session: string | null
  title: string
  closeTitle: string
  closeHoverClass: string
  onClose: () => void
}) {
  return (
    <span title={title} className="flex items-center gap-2 pl-1.5 min-w-0">
      <span className="text-[12px] font-mono font-bold whitespace-nowrap shrink-0" style={{ color: `var(${colorVar})` }}>{label}</span>
      {track && <span data-titlebar-squeeze className="text-[11px] font-mono text-[var(--text-primary)] truncate min-w-0 max-w-[10rem]">{track}</span>}
      {session && <span className="text-[9.5px] font-mono uppercase tracking-wide text-[var(--text-secondary)] whitespace-nowrap shrink-0">{session}</span>}
      <button onClick={onClose} title={closeTitle} aria-label={closeTitle} style={NO_DRAG} className={`p-1 text-[var(--text-muted)] ${closeHoverClass} transition-colors shrink-0`}><X size={12} /></button>
    </span>
  )
}
