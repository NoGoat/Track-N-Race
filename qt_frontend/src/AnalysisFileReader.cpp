#include "AnalysisFileReader.h"

#include "TnrdPlayer.h"

#include <QFileInfo>
#include <QMetaObject>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include <tnrp/TnrdReader.h>

struct AnalysisFileReader::Impl {
    explicit Impl(AnalysisFileReader* owner) : owner(owner), worker([this] { run(); }) {}

    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            work.clear();
        }
        ready.notify_all();
        if (worker.joinable()) worker.join();
    }

    void post(std::function<void()> task, bool replace) {
        {
            std::lock_guard lock(mutex);
            if (stopping) return;
            if (replace) work.clear();
            work.push_back(std::move(task));
        }
        ready.notify_one();
    }

    void run() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [this] { return stopping || !work.empty(); });
                if (stopping) return;
                task = std::move(work.front());
                work.pop_front();
            }
            task();
        }
    }

    AnalysisFileReader* owner;
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::function<void()>> work;
    std::thread worker;
    bool stopping = false;
    std::atomic<uint64_t> nextGeneration{0};
    std::atomic<uint64_t> latestLoadGeneration{0};
    std::atomic<uint64_t> activeGeneration{0};
    uint64_t pendingGeneration = 0;
    std::unique_ptr<tnrp::TnrdReader> active;
    std::unique_ptr<tnrp::TnrdReader> pending;
};

AnalysisFileReader::AnalysisFileReader(QObject* parent)
    : QObject(parent), d_(std::make_unique<Impl>(this)) {}

AnalysisFileReader::~AnalysisFileReader() = default;

void AnalysisFileReader::load(const QString& path) {
    const uint64_t generation = d_->nextGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    d_->latestLoadGeneration.store(generation, std::memory_order_release);
    d_->post([this, path, generation] {
        auto reader = std::make_unique<tnrp::TnrdReader>();
        reader->setLapStatusSummaries(true);
        tnrp::HeaderRow header;
        if (!reader->load(path.toStdString(), header)) {
            const QString reason = QString::fromStdString(reader->lastError());
            QMetaObject::invokeMethod(this, [this, generation, reason] {
                if (d_->latestLoadGeneration.load(std::memory_order_acquire) == generation)
                    emit loadFailed(reason.isEmpty()
                        ? QStringLiteral("The recording could not be opened.") : reason);
            }, Qt::QueuedConnection);
            return;
        }

        tnrp::PlaybackLapBlocksRow laps;
        const std::string encoded = reader->lapBlocksMessage();
        if (glz::read_json(laps, encoded)) {
            QMetaObject::invokeMethod(this, [this, generation] {
                if (d_->latestLoadGeneration.load(std::memory_order_acquire) == generation)
                    emit loadFailed(QStringLiteral("The recording's lap catalog could not be read."));
            }, Qt::QueuedConnection);
            return;
        }

        auto catalog = std::make_shared<AnalysisFileCatalog>();
        catalog->generation = generation;
        catalog->path = path;
        catalog->filename = QFileInfo(path).fileName();
        catalog->trackId = header.track_id;
        catalog->trackName = QString::fromStdString(header.track_name);
        catalog->laps = std::move(laps);
        d_->pending = std::move(reader);
        d_->pendingGeneration = generation;
        QMetaObject::invokeMethod(this, [this, catalog] {
            if (d_->latestLoadGeneration.load(std::memory_order_acquire) == catalog->generation)
                emit catalogLoaded(catalog);
        }, Qt::QueuedConnection);
    }, true);
}

void AnalysisFileReader::acceptLoaded(uint64_t generation) {
    d_->activeGeneration.store(generation, std::memory_order_release);
    d_->post([this, generation] {
        if (d_->pendingGeneration != generation || !d_->pending) return;
        d_->active = std::move(d_->pending);
        d_->pendingGeneration = 0;
    }, false);
}

void AnalysisFileReader::rejectLoaded(uint64_t generation) {
    d_->post([this, generation] {
        if (d_->pendingGeneration != generation) return;
        d_->pending.reset();
        d_->pendingGeneration = 0;
    }, false);
}

void AnalysisFileReader::requestLapData(int driverIndex, int lapNum, uint32_t rowTypeMask) {
    const uint64_t generation = d_->activeGeneration.load(std::memory_order_acquire);
    d_->post([this, generation, driverIndex, lapNum, rowTypeMask] {
        if (!d_->active || d_->activeGeneration.load(std::memory_order_acquire) != generation || lapNum <= 0) return;
        const std::string encoded = d_->active->getLapDataMessage(
            lapNum, rowTypeMask, driverIndex);
        auto batch = TnrdPlayer::decodeLapData(
            QByteArray(encoded.data(), static_cast<qsizetype>(encoded.size())));
        QMetaObject::invokeMethod(this, [this, generation, driverIndex, lapNum, rowTypeMask, batch] {
            if (d_->activeGeneration.load(std::memory_order_acquire) == generation)
                emit lapDataReady(generation, driverIndex, lapNum, rowTypeMask, batch);
        }, Qt::QueuedConnection);
    }, false);
}

void AnalysisFileReader::close() {
    const uint64_t generation = d_->nextGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    d_->latestLoadGeneration.store(generation, std::memory_order_release);
    d_->activeGeneration.store(0, std::memory_order_release);
    d_->post([this] {
        d_->pending.reset();
        d_->active.reset();
        d_->pendingGeneration = 0;
        d_->activeGeneration.store(0, std::memory_order_release);
    }, true);
}
