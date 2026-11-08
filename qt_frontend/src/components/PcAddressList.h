#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

#include <tnrp/NetworkSetup.h>

class QVBoxLayout;

// This PC's addresses for the game to send telemetry to, as Electron's
// PcAddressList: one row per adapter (name, what it is for, IPv4), virtual
// adapters folded behind a "N virtual adapters hidden. Show" link, and
// optionally a Copy button beside the selected address. Used by first-time
// setup and Settings ▸ Connection. The list comes from tnrp/NetworkSetup.h.
class PcAddressList : public QWidget {
    Q_OBJECT

public:
    explicit PcAddressList(bool copyable, QWidget* parent = nullptr);

    // Until this is called the list reads "Looking for network adapters…".
    void setInterfaces(const QVector<tnrp::NetworkInterfaceInfo>& interfaces);
    QString selectedAddress() const { return selected_; }
    void setSelectedAddress(const QString& address);

    // The address the game should send to: the first real adapter, else loopback.
    static QString preferredAddress(const QVector<tnrp::NetworkInterfaceInfo>& interfaces);

    // Lists the adapters off the GUI thread and hands them to `done` on it,
    // unless `context` is gone by then.
    static void load(QObject* context,
                     std::function<void(QVector<tnrp::NetworkInterfaceInfo>)> done);

signals:
    void addressSelected(const QString& address);

private:
    void rebuild();

    bool copyable_;
    bool loaded_ = false;
    bool showVirtual_ = false;
    QString selected_;
    QVector<tnrp::NetworkInterfaceInfo> interfaces_;
    QVBoxLayout* rows_ = nullptr;
};
