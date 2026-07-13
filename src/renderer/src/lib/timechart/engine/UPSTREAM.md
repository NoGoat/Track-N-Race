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
