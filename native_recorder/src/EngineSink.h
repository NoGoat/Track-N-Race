#pragma once

#include <QObject>
#include <QByteArray>

#include <cstddef>
#include <cstdint>
#include <string>

#include <tnrp/Sink.h>

// Bridges the libtnrp engine to the Qt GUI thread. tnrp::Engine calls onRow()
// from its UDP-receive (and, later, playback) thread; we forward each pre-serialised
// JSON row as a queued signal so MainWindow handles it on the GUI thread. The
// engine is configured with Config::hotRowsAsJson = true, so the hot 60 Hz rows
// also arrive as JSON via onRow() and the binary onBinary() channel is unused.
class EngineSink : public QObject, public tnrp::Sink {
    Q_OBJECT
public:
    explicit EngineSink(QObject* parent = nullptr) : QObject(parent) {}

    // Called from the engine's worker thread. QByteArray is implicitly shared and a
    // registered metatype, so the auto→queued connection marshals it safely to the
    // GUI thread without a UTF-16 round-trip.
    void onRow(const std::string& json) override {
        emit rowReady(QByteArray(json.data(), static_cast<qsizetype>(json.size())));
    }

signals:
    void rowReady(const QByteArray& json);
};
