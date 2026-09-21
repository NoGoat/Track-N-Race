#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>

#include <limits>
#include <optional>

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
    void clear() { states_.clear(); }

private:
    QHash<QString, QJsonObject> states_;
};

bool playbackFieldAvailable(const QJsonObject* object, const char* field);
bool playbackCarFieldAvailable(const QJsonObject* object, int carIndex,
                               const char* field);
