#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>
#include <optional>

#include <tnrp/NetworkSetup.h>

class MainWindow;
class QVBoxLayout;

// The connection check, as Electron's ConnectionCheck: firewall, network,
// port and incoming telemetry as a plain status list that updates on its own,
// with "Allow through firewall" when Windows Firewall blocks this app. Shown
// by first-time setup's Check connection step and Settings ▸ Connection.
class ConnectionCheck : public QWidget {
    Q_OBJECT

public:
    explicit ConnectionCheck(MainWindow* mainWindow, QWidget* parent = nullptr);

    // The adapters, when the owner already listed them (otherwise it lists
    // them itself), and the address the game sends to.
    void setInterfaces(const QVector<tnrp::NetworkInterfaceInfo>& interfaces);
    void setSelectedAddress(const QString& address);
    // Adds a Change port button to the port row when the port can't be opened.
    void setChangePortHandler(std::function<void()> handler);

private:
    void refresh();
    void checkFirewall();

    MainWindow* mainWindow_;
    QVBoxLayout* rows_ = nullptr;
    QVector<tnrp::NetworkInterfaceInfo> interfaces_;
    bool interfacesLoaded_ = false;
    QString selectedAddress_;
    std::function<void()> changePort_;
    std::optional<tnrp::FirewallStatus> firewall_;
    bool firewallBusy_ = false;
    QString firewallError_;
    bool receiving_ = false;
    bool waitedLong_ = false;
    quint64 lastDatagrams_ = 0;
    qint64 lastArrivalMs_ = 0;
};
