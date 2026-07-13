# TimeChart fork provenance

- Upstream: https://github.com/huww98/TimeChart
- Upstream version: `v1.0.0-beta.10`
- Upstream commit: `31c69965787eba1849e8d1e67846a49c438e8c15`
- Imported: 2026-07-13
- License: MIT; see `LICENSE` in this directory.

This directory contains the upstream TypeScript source vendored into Track N
Race so fixes can be maintained with the telemetry dashboard. Keep the
upstream copyright and license notice intact when modifying or redistributing
the fork.

## Track N Race changes

- Fixed `makeContentBox` to position its SVG with `paddingTop` instead of
  `paddingRight`, keeping the crosshair inside the plot rectangle when chart
  padding is asymmetric.
- Removed the engine's per-chart global window resize listener and suppress
  duplicate container/canvas sizes; Track N Race already observes each chart's
  actual container.
- Added an allocation-light model update path and skipped point-extrema scans
  when callers provide an explicit y-range.
- Coalesced hover work behind one animation-frame pointer stream, removed
  per-move layout reads and nearest-point allocations, and made tooltip DOM
  updates allocation-light.
- Cached frame-invariant WebGL line-renderer state: retained uniform views,
  resize-only projection geometry, source-aware parsed colors, allocation-free
  explicit-domain math, numeric render intervals, and consecutive shader binds.
- Replaced texture-row-rounded series synchronization with exact partial-row
  uploads backed by a retained staging buffer, including one endpoint padding
  texel where shaders require a segment neighbour.
- Reduced each RG32F series texture page from 256 x 2048 to 256 x 256 points
  (4 MiB to 512 KiB); larger buffers continue to use overlapping pages.
- Replaced per-series `DataPointsBuffer` object arrays with a paged aligned
  circular store: one Float64 X timeline, Float32 Y channels, logical-head
  eviction, shared binary search, rebuild tracking, and incremental dirty spans.
- Replaced per-series `RG32F` WebGL textures with lazy shared GPU pages: one
  `R32F` X texture plus an `R32F` texture-array containing every aligned Y
  channel. Uploads and drawing now follow physical ring/page boundaries.
- Added one visibility-aware frame scheduler for scrolling, deferred model
  redraws, and hover coalescing. Active charts share a frame timestamp, hidden
  charts are parked, and animation frames stop when no chart needs work.
- Removed the legacy point-array ingestion path, per-chart animation loops,
  the unused uPlot scrolling hook, and the obsolete domain-search helper.
