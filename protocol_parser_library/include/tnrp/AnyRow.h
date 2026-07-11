#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "tnrp/rows.h"
#include "tnrp/control_rows.h"

// Typed decode seam for in-process consumers (the Qt recorder): one call turns a
// raw JSONL row into the matching typed struct from rows.h / control_rows.h, so
// the UI never touches a dynamic JSON object. The type tag is sniffed first and
// only the matching struct is glaze-parsed (unknown keys ignored, missing keys
// keep defaults — the same tolerance the old nlohmann `.value()` access had).
//
// Row types outside the variant (playback control rows, unknown/future types)
// parse to std::nullopt; callers skip them.

namespace tnrp {

using AnyRow = std::variant<
    TelemetryRow, MotionRow, MotionExRow, PositionsRow,
    LapRow, TimingRow, StatusRow, AllStatusRow, DamageRow,
    SessionRow, ParticipantsRow, TyreSetsRow, RaceEventRow,
    FastestLapRow, SessionHistoryFastestRow,
    ProtocolStatusRow, ProtocolWarningRow>;

// One JSONL row (no trailing newline required) → typed row, or nullopt for
// types the variant doesn't carry / malformed JSON.
std::optional<AnyRow> parseRow(std::string_view jsonl);

// The row's "type" string (every variant member carries one).
const std::string& rowType(const AnyRow& row);

// The row's session_time, or -1.0f for types that don't carry one
// (positions/participants/protocol_status/protocol_warning).
float rowSessionTime(const AnyRow& row);

} // namespace tnrp
