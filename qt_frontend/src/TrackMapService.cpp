#include "TrackMapService.h"

#include <QDir>
#include <QFile>
#include <QMetaObject>
#include <QPointer>
#include <QStandardPaths>
#include <QThreadPool>

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <tnrp/Engine.h>

namespace {

constexpr auto kModdedKey = "maps/modded";
constexpr auto kGenerateKey = "maps/generate";
constexpr auto kBuiltinPrefix = "builtin:";

std::optional<tnrp::TrackMapResolution> parseResolution(std::string_view json) {
    tnrp::TrackMapResolution row;
    if (json.empty() || glz::read<glz::opts{.error_on_unknown_keys = false}>(row, json))
        return std::nullopt;
    if (row.type != "track_map") return std::nullopt;
    return row;
}

} // namespace

TrackMapService& TrackMapService::instance() {
    static TrackMapService service;
    return service;
}

TrackMapService::TrackMapService() {
    modded_ = settings_.value(kModdedKey, false).toBool();
    generate_ = settings_.value(kGenerateKey, false).toBool();
}

QString TrackMapService::directory() const {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("maps"));
}

void TrackMapService::setEngine(tnrp::Engine* engine) {
    if (engine_ == engine) return;
    engine_ = engine;
    // A new engine may reuse the old one's address; it still needs the maps.
    builtinsEngine_ = nullptr;
    if (!engine_) return;
    configure();
    refreshCatalog();
    if (modded_) {
        const auto state = parseResolution(engine_->trackMapStateJson());
        if (state && state->scope != "recording") {
            session_ = state;
            emit sessionChanged();
        }
    }
}

void TrackMapService::configure() {
    if (!engine_) return;
    QDir().mkpath(directory());
    engine_->trackMaps().setUserDirectory(QDir::toNativeSeparators(directory()).toStdString());
    engine_->refreshTrackMap();
    engine_->setTrackMapOptions(modded_, modded_ && generate_);
    if (modded_) loadBuiltins();
}

// The engine needs the bundled maps too, to match a session's car positions
// against every map. They are handed over once per engine, the first time
// Modded Maps is on, off the GUI thread.
void TrackMapService::loadBuiltins() {
    if (!engine_ || builtinsEngine_ == engine_) return;
    builtinsEngine_ = engine_;
    std::vector<std::string> maps;
    const QDir dir(QStringLiteral(":/maps"));
    for (const QString& name : dir.entryList({QStringLiteral("track_*.json")}, QDir::Files)) {
        QFile file(dir.filePath(name));
        if (file.open(QIODevice::ReadOnly)) maps.push_back(file.readAll().toStdString());
    }
    tnrp::Engine* engine = engine_;
    QThreadPool::globalInstance()->start([engine, maps = std::move(maps)] {
        bool added = false;
        for (const std::string& json : maps) added = engine->trackMaps().addBuiltin(json) || added;
        if (added) engine->refreshTrackMap();
    });
}

void TrackMapService::setModded(bool on) {
    if (modded_ == on) return;
    modded_ = on;
    settings_.setValue(kModdedKey, on);
    configure();
    if (!on && session_) {
        session_.reset();
        emit sessionChanged();
    } else if (on && engine_) {
        const auto state = parseResolution(engine_->trackMapStateJson());
        if (state && state->scope != "recording") {
            session_ = state;
            emit sessionChanged();
        }
    }
    emit settingsChanged();
}

void TrackMapService::setGenerate(bool on) {
    if (generate_ == on) return;
    generate_ = on;
    settings_.setValue(kGenerateKey, on);
    if (engine_) engine_->setTrackMapOptions(modded_, modded_ && generate_);
    emit settingsChanged();
}

std::vector<tnrp::TrackMapEntry> TrackMapService::entries() const {
    return engine_ ? engine_->trackMaps().entries() : std::vector<tnrp::TrackMapEntry>{};
}

QString TrackMapService::pinned(const QString& trackKey) const {
    return engine_ ? QString::fromStdString(engine_->trackMaps().pinned(trackKey.toStdString()))
                   : QString();
}

const tnrp::TrackMapResolution* TrackMapService::resolutionFor(int trackId) const {
    return modded_ && session_ && session_->track_id == trackId ? &*session_ : nullptr;
}

std::shared_ptr<const tnrp::TrackMapData> TrackMapService::builtin(int trackId) const {
    if (trackId < 0) return nullptr;
    const auto found = builtins_.constFind(trackId);
    if (found != builtins_.cend()) return *found;
    std::shared_ptr<const tnrp::TrackMapData> map;
    QFile file(QStringLiteral(":/maps/track_%1.json").arg(trackId));
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray json = file.readAll();
        tnrp::TrackMapData data;
        if (tnrp::readTrackMap(std::string_view(json.constData(), static_cast<size_t>(json.size())), data))
            map = std::make_shared<const tnrp::TrackMapData>(std::move(data));
    }
    builtins_.insert(trackId, map);
    return map;
}

std::shared_ptr<const tnrp::TrackMapData> TrackMapService::map(const QString& id) const {
    if (id.startsWith(QLatin1String(kBuiltinPrefix))) {
        bool ok = false;
        const int trackId = id.mid(int(std::strlen(kBuiltinPrefix))).toInt(&ok);
        return ok ? builtin(trackId) : nullptr;
    }
    if (id.isEmpty() || !engine_) return nullptr;
    const auto found = saved_.constFind(id);
    if (found != saved_.cend()) return *found;
    std::shared_ptr<const tnrp::TrackMapData> map;
    if (auto data = engine_->trackMaps().map(id.toStdString()))
        map = std::make_shared<const tnrp::TrackMapData>(std::move(*data));
    saved_.insert(id, map);
    return map;
}

std::shared_ptr<const tnrp::TrackMapData> TrackMapService::mapFor(int trackId) const {
    if (trackId < 0) return nullptr;
    const tnrp::TrackMapResolution* resolution = resolutionFor(trackId);
    if (!resolution || resolution->map_id.empty()) return builtin(trackId);
    return map(QString::fromStdString(resolution->map_id));
}

QString TrackMapService::editableMapId(int trackId) const {
    const tnrp::TrackMapResolution* resolution = resolutionFor(trackId);
    if (!resolution) return {};
    if (!resolution->map_id.empty()) return QString::fromStdString(resolution->map_id);
    return builtin(trackId) ? QStringLiteral("builtin:%1").arg(trackId) : QString();
}

void TrackMapService::refreshCatalog() {
    // A map renamed or replaced on disk is read again.
    saved_.clear();
    emit catalogChanged();
}

void TrackMapService::pin(const QString& trackKey, const QString& mapId) {
    if (!engine_) return;
    engine_->trackMaps().pin(trackKey.toStdString(), mapId.toStdString());
    engine_->refreshTrackMap();
    refreshCatalog();
}

void TrackMapService::runChange(std::function<Result(tnrp::Engine&)> call, bool refresh, Done done) {
    if (!engine_) {
        if (done) done({false, {}, QStringLiteral("The map engine is not available.")});
        return;
    }
    tnrp::Engine* engine = engine_;
    QPointer<TrackMapService> self(this);
    QThreadPool::globalInstance()->start([self, engine, call = std::move(call), refresh,
                                          done = std::move(done)] {
        Result result = call(*engine);
        if (result.ok && refresh) engine->refreshTrackMap();
        QMetaObject::invokeMethod(self, [self, result, refresh, done] {
            if (self && result.ok && refresh) self->refreshCatalog();
            if (done) done(result);
        }, Qt::QueuedConnection);
    });
}

void TrackMapService::edit(const QString& id, const tnrp::TrackMapEdit& edit, Done done) {
    runChange([id = id.toStdString(), edit](tnrp::Engine& engine) {
        std::string error;
        const std::string newId = engine.trackMaps().edit(id, edit, &error);
        return Result{!newId.empty(), QString::fromStdString(newId), QString::fromStdString(error)};
    }, true, std::move(done));
}

void TrackMapService::remove(const QString& id, Done done) {
    runChange([id = id.toStdString()](tnrp::Engine& engine) {
        std::string error;
        const bool ok = engine.trackMaps().remove(id, &error);
        return Result{ok, QString::fromStdString(id), QString::fromStdString(error)};
    }, true, std::move(done));
}

void TrackMapService::importFiles(const QStringList& paths, Done done) {
    std::vector<std::string> files;
    for (const QString& path : paths) files.push_back(QDir::toNativeSeparators(path).toStdString());
    runChange([files = std::move(files)](tnrp::Engine& engine) {
        Result result;
        QStringList errors;
        for (const std::string& file : files) {
            std::string error;
            const std::string id = engine.trackMaps().importFile(file, &error);
            if (id.empty()) {
                errors << QStringLiteral("%1: %2").arg(QDir(QString::fromStdString(file)).dirName(),
                                                       QString::fromStdString(error));
            } else {
                result.ok = true;
                result.id = QString::fromStdString(id);
            }
        }
        result.error = errors.join(QLatin1Char('\n'));
        return result;
    }, true, std::move(done));
}

void TrackMapService::exportMap(const QString& id, const QString& path, Done done) {
    runChange([id = id.toStdString(), path = QDir::toNativeSeparators(path).toStdString()](tnrp::Engine& engine) {
        std::string error;
        const bool ok = engine.trackMaps().exportFile(id, path, &error);
        return Result{ok, QString::fromStdString(id), QString::fromStdString(error)};
    }, false, std::move(done));
}

void TrackMapService::generateFromRecording(const QString& path, Done done) {
    runChange([path = QDir::toNativeSeparators(path).toStdString()](tnrp::Engine& engine) {
        std::string error;
        const std::string id = engine.generateTrackMap(path, &error);
        return Result{!id.empty(), QString::fromStdString(id), QString::fromStdString(error)};
    }, true, std::move(done));
}

bool TrackMapService::handleRow(const QByteArray& json) {
    if (json.contains("\"type\":\"track_maps_changed\"")) {
        refreshCatalog();
        return true;
    }
    if (!json.contains("\"type\":\"track_map\"")) return false;
    auto row = parseResolution(std::string_view(json.constData(), static_cast<size_t>(json.size())));
    if (row && row->scope != "recording") {
        session_ = std::move(row);
        emit sessionChanged();
    }
    return true;
}

void TrackMapService::clearPlaybackSession() {
    if (!session_ || session_->scope != "playback") return;
    session_.reset();
    emit sessionChanged();
}
