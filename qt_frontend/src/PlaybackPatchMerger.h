#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>

#include <limits>
#include <optional>
#include <vector>

#include <tnrp/AnyRow.h>

// TNRD V6 playback is a stream of independent field patches.  Electron merges
// those patches in its renderer store; this is the equivalent Qt host adapter.
// It deliberately lives outside libtnrp: the shared parser's legacy row structs
// retain their normal defaults, while this frontend marks absent projected
// values so widgets and charts can render a genuine missing-data state.
constexpr int kPlaybackMissingInt = std::numeric_limits<int>::min();

struct PlaybackDecodedRow {
    tnrp::AnyRow row;
    QByteArray normalizedJson;
    QJsonObject normalizedObject;
    bool sparse = false;
};

class PlaybackPatchMerger {
public:
    std::optional<PlaybackDecodedRow> decode(const QByteArray& json);
    // Merges one already-parsed row without re-serialising it: a V6 patch
    // (with _v6_type) returns the merged state of its row type and sets
    // *sparse; any other row is returned unchanged.
    QJsonObject mergeObject(const QJsonObject& row, bool* sparse);
    void clear() { states_.clear(); }

private:
    QHash<QString, QJsonObject> states_;
};

// The fields one TNRD V6 data type carries (Electron's V6_PATCH_FIELDS). An
// `available:false` sample of that type withdraws exactly these fields.
std::vector<const char*> playbackPatchFields(int v6Type);

bool playbackFieldAvailable(const QJsonObject* object, const char* field);
bool playbackCarFieldAvailable(const QJsonObject* object, int carIndex,
                               const char* field);
