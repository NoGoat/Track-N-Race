#pragma once

#include <QObject>
#include <QByteArray>

#include <cstddef>
#include <cstdint>
#include <string>

#include <tnrp/Sink.h>

// Bridges the libtnrp engine to the Qt GUI thread. tnrp::Engine calls onRow() /
// onBinary() from its UDP-receive (and playback) thread; we forward the raw bytes
// as queued signals so MainWindow decodes them on the GUI thread — cold/control
// rows via tnrp::parseRow (typed structs, no dynamic JSON), the hot 60 Hz rows
// via tnrp::bin::decodeBatch (packed records; the engine runs with
// Config::hotRowsAsJson = false so hot rows never round-trip through JSON).
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

    // Hot 60 Hz rows (telemetry/motion/motion_ex/positions) as one packed batch.
    void onBinary(const uint8_t* data, size_t len) override {
        emit binaryReady(QByteArray(reinterpret_cast<const char*>(data),
                                    static_cast<qsizetype>(len)));
    }

signals:
    void rowReady(const QByteArray& json);
    void binaryReady(const QByteArray& batch);
};
