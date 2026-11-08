#pragma once

#include <QHash>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>

#include <tnrp/TrackMaps.h>

namespace tnrp { class Engine; }

// Modded maps (tnrp/TrackMaps.h): the settings, the saved maps, and which map
// the live session or loaded recording uses. libtnrp chooses, generates and
// stores maps; this hands it the bundled maps and the user folder, mirrors its
// answers, and turns UI requests into calls. GUI thread only.
class TrackMapService : public QObject {
    Q_OBJECT
public:
    struct Result {
        bool ok = false;
        QString id;
        QString error;
    };
    using Done = std::function<void(const Result&)>;

    static TrackMapService& instance();

    // MainWindow hands over each engine it creates, and nullptr before one goes.
    void setEngine(tnrp::Engine* engine);

    bool modded() const { return modded_; }
    bool generate() const { return generate_; }
    void setModded(bool on);
    void setGenerate(bool on);
    QString directory() const;

    std::vector<tnrp::TrackMapEntry> entries() const;
    QString pinned(const QString& trackKey) const;
    // The live session's or loaded recording's last "track_map" row.
    const std::optional<tnrp::TrackMapResolution>& session() const { return session_; }
    // The session's choice for a track, with Modded Maps on and a choice made for it.
    const tnrp::TrackMapResolution* resolutionFor(int trackId) const;

    // A map by ID: "builtin:<track id>" from the bundled files, else a saved map.
    std::shared_ptr<const tnrp::TrackMapData> map(const QString& id) const;
    std::shared_ptr<const tnrp::TrackMapData> builtin(int trackId) const;
    // The map to draw for a track: the session's choice with Modded Maps on,
    // otherwise (or until a choice arrives) the bundled map for its ID.
    std::shared_ptr<const tnrp::TrackMapData> mapFor(int trackId) const;
    // The map in use for a track that Edit Map changes; empty without Modded Maps.
    QString editableMapId(int trackId) const;

    void pin(const QString& trackKey, const QString& mapId);
    void edit(const QString& id, const tnrp::TrackMapEdit& edit, Done done);
    void remove(const QString& id, Done done);
    void importFiles(const QStringList& paths, Done done);
    void exportMap(const QString& id, const QString& path, Done done);
    void generateFromRecording(const QString& path, Done done);

    // Engine rows: "track_map" and "track_maps_changed". True when handled.
    bool handleRow(const QByteArray& json);
    // Playback closed: its map choice no longer applies.
    void clearPlaybackSession();

signals:
    void settingsChanged();
    void catalogChanged();
    void sessionChanged();

private:
    TrackMapService();

    void configure();
    void loadBuiltins();
    void refreshCatalog();
    void runChange(std::function<Result(tnrp::Engine&)> call, bool refresh, Done done);

    QSettings settings_{QStringLiteral("TrackNRace"), QStringLiteral("NativeRecorder")};
    tnrp::Engine* engine_ = nullptr;
    tnrp::Engine* builtinsEngine_ = nullptr;
    bool modded_ = false;
    bool generate_ = false;
    std::optional<tnrp::TrackMapResolution> session_;
    mutable QHash<int, std::shared_ptr<const tnrp::TrackMapData>> builtins_;
    mutable QHash<QString, std::shared_ptr<const tnrp::TrackMapData>> saved_;
};
