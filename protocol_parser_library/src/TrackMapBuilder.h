#pragma once

#include <functional>
#include <string>
#include <vector>

#include "tnrp/TrackMaps.h"

namespace tnrp::detail {

class TnrdV6Archive;

// generateTrackMapFromRecording() over an archive that is already open: a
// recording, or an image of the live session. `info` labels the map; a
// trackLengthM of 0 takes the laps' own length.
bool buildTrackMap(TnrdV6Archive& archive, const TrackMapGenerateInfo& info,
                   TrackMapData& out, std::string* errorOut,
                   const std::function<bool()>& cancelled = {});

// recordingFitSamples() over an open archive.
std::vector<WorldPoint> archiveFitSamples(TnrdV6Archive& archive, size_t perDriver);

} // namespace tnrp::detail
