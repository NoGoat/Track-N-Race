#include "TnrdPlayer.h"

#include <QMetaObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>
#include <type_traits>

#include <tnrp/AnyRow.h>
#include <tnrp/BinaryRows.h>
#include <tnrp/Engine.h>

namespace {

constexpr qsizetype kMaxRows = 750000;

std::string_view typeOf(std::string_view json) {
    constexpr std::string_view key = "\"type\":\"";
    const size_t begin = json.find(key);
    if (begin == std::string_view::npos) return {};
    const size_t value = begin + key.size();
    const size_t end = json.find('"', value);
    return end == std::string_view::npos ? std::string_view{} : json.substr(value, end - value);
}

void appendHistoryRow(PlaybackHistoryBatch& batch, const tnrp::AnyRow& row) {
    auto& data = batch.data;
    if (const auto* t = std::get_if<TelemetryRow>(&row)) {
        data.onTelemetry(t->session_time, static_cast<float>(t->speed_kph), t->rpm,
                         t->gear, t->throttle, t->brake,
                         static_cast<float>(t->steering));
        data.onTyre(t->session_time,
            static_cast<float>(t->tyre_temp_surface_fl), static_cast<float>(t->tyre_temp_surface_fr),
            static_cast<float>(t->tyre_temp_surface_rl), static_cast<float>(t->tyre_temp_surface_rr),
            static_cast<float>(t->tyre_temp_inner_fl), static_cast<float>(t->tyre_temp_inner_fr),
            static_cast<float>(t->tyre_temp_inner_rl), static_cast<float>(t->tyre_temp_inner_rr),
            static_cast<float>(t->brake_temp_fl), static_cast<float>(t->brake_temp_fr),
            static_cast<float>(t->brake_temp_rl), static_cast<float>(t->brake_temp_rr),
            0.0f, 0.0f, 0.0f, 0.0f);
    } else if (const auto* s = std::get_if<StatusRow>(&row)) {
        data.onStatus(s->session_time, static_cast<float>(s->ers_pct),
                      static_cast<float>(s->fuel_kg),
                      static_cast<float>(s->engine_power_ice_kw),
                      static_cast<float>(s->engine_power_mguk_kw),
                      static_cast<float>(s->ers_harvested_mguk_j),
                      static_cast<float>(s->ers_harvested_mguh_j),
                      s->tyre_compound, s->visual_compound, s->tyre_age_laps);
    } else if (const auto* d = std::get_if<DamageRow>(&row)) {
        data.onDamage(d->session_time, static_cast<float>(d->tyre_wear_fl),
                      static_cast<float>(d->tyre_wear_fr),
                      static_cast<float>(d->tyre_wear_rl),
                      static_cast<float>(d->tyre_wear_rr));
    } else if (const auto* l = std::get_if<LapRow>(&row)) {
        batch.progress.push_back({l->session_time, l->current_lap_ms,
                                  static_cast<float>(l->lap_distance_m), l->sector});
        data.latestTime = std::max(data.latestTime, l->session_time);
    } else if (const auto* m = std::get_if<MotionRow>(&row)) {
        data.onMotion(m->session_time, static_cast<float>(m->g_lat),
                      static_cast<float>(m->g_long));
    } else if (const auto* m = std::get_if<MotionExRow>(&row)) {
        data.onMotionEx(m->session_time,
                        static_cast<float>(m->front_aero_height_mm),
                        static_cast<float>(m->rear_aero_height_mm));
    }
}

template <typename T>
void sortAndCap(QVector<T>& rows) {
    std::stable_sort(rows.begin(), rows.end(),
                     [](const T& a, const T& b) { return a.t < b.t; });
    if (rows.size() > kMaxRows) rows.remove(0, rows.size() - kMaxRows);
}

template <typename T>
void copyTimedRange(const QVector<T>& source, float start, float end, QVector<T>& target) {
    const auto first = std::lower_bound(source.begin(), source.end(), start,
        [](const T& row, float value) { return row.t < value; });
    const auto last = std::upper_bound(first, source.end(), end,
        [](float value, const T& row) { return value < row.t; });
    target.reserve(std::distance(first, last));
    for (auto it = first; it != last; ++it) target.push_back(*it);
}

void buildLapDetails(PlaybackHistoryBatch& batch,
                     const QVector<PlaybackLapRange>& ranges) {
    batch.lapDetails.reserve(ranges.size());
    for (const PlaybackLapRange& range : ranges) {
        LapBlock lap;
        lap.lapNum = range.lapNum;
        lap.startSessionTime = range.start;
        lap.endSessionTime = range.end;
        copyTimedRange(batch.data.telBuf, range.start, range.end, lap.tel);
        copyTimedRange(batch.data.tyreBuf, range.start, range.end, lap.tyre);
        copyTimedRange(batch.data.stsBuf, range.start, range.end, lap.sts);
        copyTimedRange(batch.data.damageBuf, range.start, range.end, lap.damage);
        copyTimedRange(batch.data.motionBuf, range.start, range.end, lap.motion);
        copyTimedRange(batch.data.motionExBuf, range.start, range.end, lap.motionEx);
        copyTimedRange(batch.progress, range.start, range.end, lap.progress);
        if (!lap.tel.isEmpty() || !lap.tyre.isEmpty() || !lap.sts.isEmpty() ||
            !lap.damage.isEmpty() || !lap.motion.isEmpty() ||
            !lap.motionEx.isEmpty() || !lap.progress.isEmpty())
            batch.lapDetails.push_back(std::move(lap));
    }
}

} // namespace

TnrdPlayer::TnrdPlayer(tnrp::Engine* engine, QObject* parent)
    : QObject(parent), engine_(engine), worker_(&TnrdPlayer::workerLoop, this) {}

TnrdPlayer::~TnrdPlayer() { shutdown(); }

void TnrdPlayer::setEngine(tnrp::Engine* engine) {
    quiesce();
    latestSeekRequest_.fetch_add(1, std::memory_order_acq_rel);
    latestRequirementsRequest_.fetch_add(1, std::memory_order_acq_rel);
    engine_.store(engine, std::memory_order_release);
    requirementsApplied_ = false;
    catalogReady_ = false;
    if (!engine && loaded_.exchange(false, std::memory_order_acq_rel)) {
        loading_ = false;
        playing_ = false;
        emit closed();
    }
}

void TnrdPlayer::quiesce() {
    std::unique_lock lock(workMutex_);
    work_.clear();
    idleCv_.wait(lock, [this] { return !workerActive_; });
}

void TnrdPlayer::shutdown() {
    {
        std::lock_guard lock(workMutex_);
        if (stopping_) return;
        stopping_ = true;
        work_.clear();
    }
    workCv_.notify_all();
    if (worker_.joinable()) worker_.join();
    engine_.store(nullptr, std::memory_order_release);
}

void TnrdPlayer::post(WorkKind kind, std::function<void()> work, bool replacePending) {
    {
        std::lock_guard lock(workMutex_);
        if (stopping_) return;
        if (replacePending) {
            if (kind == WorkKind::Close || kind == WorkKind::Load) {
                work_.clear();
            } else {
                std::erase_if(work_, [kind](const WorkItem& item) {
                    return item.kind == kind ||
                        (kind == WorkKind::Seek && item.kind == WorkKind::SeekDecode);
                });
            }
        }
        work_.push_back({kind, std::move(work)});
    }
    workCv_.notify_one();
}

void TnrdPlayer::workerLoop() {
    for (;;) {
        WorkItem item;
        {
            std::unique_lock lock(workMutex_);
            workCv_.wait(lock, [this] { return stopping_ || !work_.empty(); });
            if (stopping_) break;
            item = std::move(work_.front());
            work_.pop_front();
            workerActive_ = true;
        }
        item.run();
        {
            std::lock_guard lock(workMutex_);
            workerActive_ = false;
        }
        idleCv_.notify_all();
    }
    {
        std::lock_guard lock(workMutex_);
        workerActive_ = false;
    }
    idleCv_.notify_all();
}

void TnrdPlayer::load(const QString& path) {
    if (loading_) return;
    loading_ = true;
    emit loadingStarted();
    const std::string source = path.toStdString();
    post(WorkKind::Load, [this, source] {
        tnrp::Engine* engine = engine_.load(std::memory_order_acquire);
        std::string error;
        const bool ok = engine && engine->playerLoad(source, &error);
        if (!ok) {
            const QString reason = engine
                ? QString::fromStdString(error)
                : QStringLiteral("The telemetry engine is not available.");
            QMetaObject::invokeMethod(this, [this, reason] {
                loading_ = false;
                emit loadFailed(reason.isEmpty()
                    ? QStringLiteral("The file could not be read.") : reason);
            }, Qt::QueuedConnection);
        }
    }, true);
}

void TnrdPlayer::play() {
    if (auto* engine = engine_.load(std::memory_order_acquire); engine && loaded_)
        engine->playerPlay();
}

void TnrdPlayer::pause() {
    if (auto* engine = engine_.load(std::memory_order_acquire); engine && loaded_)
        engine->playerPause();
}

void TnrdPlayer::close() {
    latestSeekRequest_.fetch_add(1, std::memory_order_acq_rel);
    latestRequirementsRequest_.fetch_add(1, std::memory_order_acq_rel);
    playing_ = false;
    post(WorkKind::Close, [this] {
        if (auto* engine = engine_.load(std::memory_order_acquire)) engine->playerClose();
    }, true);
}

void TnrdPlayer::seek(float pct) {
    tnrp::Engine* engine = engine_.load(std::memory_order_acquire);
    if (!engine || !loaded_) return;
    resumeAfterSeek_ = resumeAfterSeek_ || playing_;
    if (playing_) engine->playerPause();
    const uint64_t requirementsId =
        latestRequirementsRequest_.fetch_add(1, std::memory_order_acq_rel) + 1;
    engine->requestDataRequirements(requirementsId);
    const uint64_t requestId = latestSeekRequest_.fetch_add(1, std::memory_order_acq_rel) + 1;
    engine->playerRequestSeek(requestId);
    emit seekStarted(requestId);
    emit seeked();
    const uint32_t mask = historyMask_;
    const float window = historyWindowSeconds_ < 0.0f ? 0.0f : historyWindowSeconds_;
    const bool allHistory = historyWindowSeconds_ < 0.0f;
    const uint32_t streamMask = streamMask_;
    const uint32_t historyMask = historyMask_;
    const float requirementWindow = historyWindowSeconds_;
    post(WorkKind::Seek, [this, pct, requestId, mask, window, allHistory,
                          requirementsId, streamMask, historyMask, requirementWindow] {
        if (auto* current = engine_.load(std::memory_order_acquire)) {
            current->setDataRequirements(streamMask, historyMask,
                                         requirementWindow, requirementsId);
            current->playerSeek(pct, allHistory, requestId, mask, window);
        }
    }, true);
}

void TnrdPlayer::seekToTime(float absoluteTime) {
    const float duration = std::max(0.0f, totalTime_ - startTime_);
    seek(duration > 0.0f ? (absoluteTime - startTime_) / duration : 0.0f);
}

void TnrdPlayer::setSpeed(float mult) {
    if (auto* engine = engine_.load(std::memory_order_acquire); engine && loaded_)
        engine->playerSetSpeed(mult);
}

void TnrdPlayer::setDataRequirements(uint32_t streamMask, uint32_t historyMask,
                                     float windowSeconds) {
    if (requirementsApplied_ && streamMask_ == streamMask && historyMask_ == historyMask &&
        historyWindowSeconds_ == windowSeconds) return;
    streamMask_ = streamMask;
    historyMask_ = historyMask;
    historyWindowSeconds_ = windowSeconds;
    requirementsApplied_ = true;
    tnrp::Engine* engine = engine_.load(std::memory_order_acquire);
    if (!engine) return;
    const uint64_t requestId =
        latestRequirementsRequest_.fetch_add(1, std::memory_order_acq_rel) + 1;
    engine->requestDataRequirements(requestId);
    post(WorkKind::Requirements,
         [this, streamMask, historyMask, windowSeconds, requestId] {
        tnrp::Engine* current = engine_.load(std::memory_order_acquire);
        if (!current) return;
        current->setDataRequirements(streamMask, historyMask, windowSeconds, requestId);
        if (!loaded_ || historyMask == 0 ||
            !catalogReady_.load(std::memory_order_acquire) ||
            requestId != latestRequirementsRequest_.load(std::memory_order_acquire)) return;
        if (windowSeconds < 0.0f)
            current->playerGetAllLapsData(requestId, historyMask);
        else
            current->playerGetWindowData(windowSeconds, requestId, historyMask);
    }, true);
}

void TnrdPlayer::requestLapData(int lapNum, uint32_t rowTypeMask) {
    if (lapNum <= 0 || !loaded_) return;
    post(WorkKind::LapData, [this, lapNum, rowTypeMask] {
        if (auto* current = engine_.load(std::memory_order_acquire))
            current->playerGetLapData(lapNum, rowTypeMask);
    }, false);
}

bool TnrdPlayer::handleControlRow(const QByteArray& json) {
    const std::string_view source(json.constData(), static_cast<size_t>(json.size()));
    const std::string_view type = typeOf(source);
    if (type == "playback_loaded") {
        tnrp::PlaybackLoadedRow row;
        if (glz::read_json(row, source)) return true;
        if (row.ok && row.header) {
            loading_ = false;
            loaded_ = true;
            catalogReady_ = false;
            playing_ = false;
            requirementsApplied_ = false;
            emit loaded(*row.header);
        }
        return true;
    }
    if (type == "playback_lap_blocks") {
        tnrp::PlaybackLapBlocksRow row;
        if (!glz::read_json(row, source)) {
            lapRanges_.clear();
            lapRanges_.reserve(static_cast<qsizetype>(row.blocks.size()));
            for (const auto& block : row.blocks)
                lapRanges_.push_back({block.lapNum, block.startSessionTime,
                                      block.endSessionTime});
            catalogReady_.store(true, std::memory_order_release);
            emit lapBlocksReady(row);
            requirementsApplied_ = false;
            setDataRequirements(streamMask_, historyMask_, historyWindowSeconds_);
        }
        return true;
    }
    if (type == "playback_state") {
        tnrp::PlaybackStateRow row;
        if (glz::read_json(row, source)) return true;
        startTime_ = row.start_time;
        totalTime_ = row.start_time + row.total_time;
        currentTime_ = row.start_time + row.current_time;
        speed_ = row.speed;
        playing_ = row.playing;
        emit stateChanged(playing_, row.current_time, row.total_time, speed_);
        return true;
    }
    if (type == "playback_finished") {
        playing_ = false;
        emit finished();
        return true;
    }
    if (type == "playback_close") {
        loaded_ = false;
        catalogReady_ = false;
        loading_ = false;
        playing_ = false;
        startTime_ = totalTime_ = currentTime_ = 0.0f;
        lapRanges_.clear();
        emit closed();
        return true;
    }
    if (type == "playback_lap_data") {
        const QByteArray copy = json;
        post(WorkKind::LapDataDecode, [this, copy] {
            auto decoded = decodeLapData(copy);
            if (!decoded) return;
            QMetaObject::invokeMethod(this, [this, decoded = std::move(decoded)] {
                if (loaded_) emit historyDecoded(decoded);
            }, Qt::QueuedConnection);
        }, false);
        return true;
    }
    if (type == "playback_seek_flush") return true;
    return false;
}

void TnrdPlayer::handleSeekFlush(const std::shared_ptr<EngineSeekFlush>& flush) {
    if (!flush) return;
    if (flush->authoritativeSeek) {
        if (flush->requestId != latestSeekRequest_.load(std::memory_order_acquire)) return;
    } else if (flush->requestId != 0 &&
               flush->requestId != latestRequirementsRequest_.load(std::memory_order_acquire)) {
        return;
    }
    const WorkKind decodeKind = flush->authoritativeSeek
        ? WorkKind::SeekDecode : WorkKind::RequirementsDecode;
    const QVector<PlaybackLapRange> lapRanges = lapRanges_;
    post(decodeKind, [this, flush, lapRanges] {
        auto decoded = decodeHistory(flush, lapRanges);
        if (!decoded) return;
        QMetaObject::invokeMethod(this, [this, decoded = std::move(decoded)] {
            if (decoded->authoritativeSeek) {
                if (decoded->requestId != latestSeekRequest_.load(std::memory_order_acquire)) return;
            } else if (decoded->requestId != 0 &&
                       decoded->requestId != latestRequirementsRequest_.load(std::memory_order_acquire)) {
                return;
            }
            emit historyDecoded(decoded);
        }, Qt::QueuedConnection);
    }, true);
}

void TnrdPlayer::completeSeek(uint64_t requestId) {
    if (requestId != latestSeekRequest_.load(std::memory_order_acquire)) return;
    if (resumeAfterSeek_) {
        resumeAfterSeek_ = false;
        play();
    }
}

std::shared_ptr<PlaybackHistoryBatch>
TnrdPlayer::decodeHistory(const std::shared_ptr<EngineSeekFlush>& flush,
                          const QVector<PlaybackLapRange>& lapRanges) {
    auto result = std::make_shared<PlaybackHistoryBatch>();
    result->data.trimBuffers = false;
    result->currentLapStart = flush->currentLapStart;
    result->lapNum = flush->lapNum;
    result->additive = flush->allHistory || !flush->authoritativeSeek;
    result->authoritativeSeek = flush->authoritativeSeek;
    result->requestId = flush->requestId;
    result->rowTypeMask = flush->rowTypeMask;
    result->historyStart = flush->historyStart;

    if (flush->binaryStore && flush->binaryBegin <= flush->binaryEnd &&
        flush->binaryEnd <= flush->binaryStore->size()) {
        const uint8_t* begin = flush->binaryStore->data() + flush->binaryBegin;
        const size_t length = flush->binaryEnd - flush->binaryBegin;
        tnrp::bin::decodeBatch(begin, length, [&result](auto&& row) {
            appendHistoryRow(*result, tnrp::AnyRow(std::move(row)));
        });
    }

    qsizetype offset = 0;
    while (offset < flush->coldJson.size()) {
        qsizetype end = flush->coldJson.indexOf('\n', offset);
        if (end < 0) end = flush->coldJson.size();
        if (end > offset) {
            const std::string_view line(flush->coldJson.constData() + offset,
                                        static_cast<size_t>(end - offset));
            if (auto row = tnrp::parseRow(line)) appendHistoryRow(*result, *row);
        }
        offset = end + 1;
    }

    sortAndCap(result->data.telBuf);
    sortAndCap(result->data.stsBuf);
    sortAndCap(result->data.motionBuf);
    sortAndCap(result->data.motionExBuf);
    sortAndCap(result->data.tyreBuf);
    sortAndCap(result->data.damageBuf);
    sortAndCap(result->progress);
    buildLapDetails(*result, lapRanges);
    return result;
}

std::shared_ptr<PlaybackHistoryBatch> TnrdPlayer::decodeLapData(const QByteArray& json) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return {};
    const QJsonObject object = document.object();
    auto result = std::make_shared<PlaybackHistoryBatch>();
    result->data.trimBuffers = false;
    result->lapNum = object.value("lapNum").toInt();
    result->currentLapStart = static_cast<float>(object.value("startSessionTime").toDouble());
    result->historyStart = result->currentLapStart;
    result->rowTypeMask = static_cast<uint32_t>(object.value("rowTypeMask").toDouble());
    result->authoritativeSeek = false;
    result->additive = true;
    result->isolatedLapRequest = true;

    auto appendArray = [&result, &object](const char* name) {
        const QJsonArray rows = object.value(QString::fromLatin1(name)).toArray();
        for (const QJsonValue& value : rows) {
            if (!value.isObject()) continue;
            const QByteArray encoded = QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
            if (auto row = tnrp::parseRow(std::string_view(
                    encoded.constData(), static_cast<size_t>(encoded.size()))))
                appendHistoryRow(*result, *row);
        }
    };
    appendArray("telemetry");
    appendArray("statusHistory");
    appendArray("motionHistory");
    appendArray("motionExHistory");
    appendArray("damageHistory");

    for (const QJsonValue& value : object.value("lapProgress").toArray()) {
        const QJsonObject row = value.toObject();
        result->progress.push_back({
            static_cast<float>(row.value("session_time").toDouble()),
            row.value("current_lap_ms").toInt(),
            static_cast<float>(row.value("lap_distance_m").toDouble()),
            row.value("sector").toInt()
        });
    }
    sortAndCap(result->data.telBuf);
    sortAndCap(result->data.stsBuf);
    sortAndCap(result->data.motionBuf);
    sortAndCap(result->data.motionExBuf);
    sortAndCap(result->data.tyreBuf);
    sortAndCap(result->data.damageBuf);
    sortAndCap(result->progress);
    LapBlock detail;
    detail.lapNum = result->lapNum;
    detail.startSessionTime = result->currentLapStart;
    detail.endSessionTime = static_cast<float>(object.value("endSessionTime").toDouble());
    detail.tel = result->data.telBuf;
    detail.sts = result->data.stsBuf;
    detail.tyre = result->data.tyreBuf;
    detail.damage = result->data.damageBuf;
    detail.motion = result->data.motionBuf;
    detail.motionEx = result->data.motionExBuf;
    detail.progress = result->progress;
    result->lapDetails.push_back(std::move(detail));
    return result;
}
