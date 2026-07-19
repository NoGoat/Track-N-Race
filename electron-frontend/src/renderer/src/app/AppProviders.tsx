import type { PropsWithChildren } from 'react'
import { CardColorsProvider } from '../lib/cards'
import { LabelsProvider } from '../lib/labels'
import { useTelemetryStore } from '../stores/telemetryStore'

export default function AppProviders({ children }: PropsWithChildren) {
  const labels = useTelemetryStore(state => state.protocolStatus?.labels)
  const cardColors = useTelemetryStore(state => state.protocolStatus?.cardColors)

  return (
    <LabelsProvider labels={labels}>
      <CardColorsProvider specs={cardColors}>
        {children}
      </CardColorsProvider>
    </LabelsProvider>
  )
}
