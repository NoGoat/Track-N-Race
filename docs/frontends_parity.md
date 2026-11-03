# Frontend parity

Status per frontend: Done / Not done / N/A.

| Feature | Electron | Qt | Android | Updated | Notes |
| --- | --- | --- | --- | --- | --- |
| Analysis map layers (Track Dominance / Driver Inputs / Speed / ERS Usage / ERS Harvest) | Done | Not done | Not done | 2026-11-01 | Calculation is in libtnrp (tnrp/AnalysisMap.h, `buildAnalysisMap` modes incl. `speed`, `calculateSpeed`); Qt: header only. Android has no `buildAnalysisMap` JNI yet |
| Analysis map braking points and Data Comparison panel toggle | Done | Not done | Not done | 2026-11-01 | Braking points are found in libtnrp (tnrp/AnalysisMap.h) |
| Modded maps (map picker, geometry matching, generate from live session or recording, import/export, Edit Map: names and rotation) | Done | Not done | Not done | 2026-11-01 | Logic in libtnrp (tnrp/TrackMaps.h, `Engine::trackMaps` / `setTrackMapOptions` / `generateTrackMap` / `resolveTrackMapForRecording`, `TrackMapLibrary::edit`, `track_map` rows). Qt: header only. Plumbed: `nativeTrackMaps*` (Android JNI, per session engine). Live matching and generation need the retained live session (`binaryPlayback`) |
