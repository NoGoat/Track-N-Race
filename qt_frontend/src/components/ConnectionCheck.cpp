#include "ConnectionCheck.h"

#include "../MainWindow.h"
#include "PcAddressList.h"
#include "SetupUi.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

using namespace tnrsetup;

namespace {

constexpr int kNoTelemetryHintMs = 30'000;

enum class Tone { Ok, Warn, Bad, Wait, Info };

QColor toneColor(Tone tone, const QPalette& palette) {
    switch (tone) {
        case Tone::Ok:   return QColor("#4ade80");
        case Tone::Warn: return QColor("#f59e0b");
        case Tone::Bad:  return QColor("#f87171");
        case Tone::Wait:
        case Tone::Info: break;
    }
    return palette.color(QPalette::PlaceholderText);
}

QWidget* checkRow(Tone tone, const QString& title, const QString& detail,
                  const QString& status = {}, QWidget* action = nullptr,
                  QWidget* extra = nullptr) {
    auto* row = new QWidget;
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 12, 0, 12);
    layout->setSpacing(14);

    QStyle::StandardPixmap fallback = QStyle::SP_MessageBoxInformation;
    const char* iconName = "dialog-information";
    switch (tone) {
        case Tone::Ok:   iconName = "dialog-ok-apply"; fallback = QStyle::SP_DialogApplyButton; break;
        case Tone::Warn: iconName = "dialog-warning"; fallback = QStyle::SP_MessageBoxWarning; break;
        case Tone::Bad:  iconName = "dialog-error"; fallback = QStyle::SP_MessageBoxCritical; break;
        case Tone::Wait: iconName = "view-refresh"; fallback = QStyle::SP_BrowserReload; break;
        case Tone::Info: break;
    }
    auto* icon = new QLabel;
    icon->setPixmap(tintedIcon(row, iconName, fallback, toneColor(tone, row->palette())).pixmap(18, 18));
    icon->setFixedSize(18, 18);
    layout->addWidget(icon, 0, Qt::AlignTop);

    auto* text = new QVBoxLayout;
    text->setSpacing(3);
    auto* titleLabel = new QLabel(title);
    titleLabel->setFont(scaledFont(titleLabel->font(), 1.0, true));
    text->addWidget(titleLabel);
    auto* detailLabel = muted(detail);
    detailLabel->setTextFormat(Qt::RichText);
    detailLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    text->addWidget(detailLabel);
    if (extra) text->addWidget(extra);
    layout->addLayout(text, 1);

    if (action) {
        layout->addWidget(action, 0, Qt::AlignVCenter);
    } else if (!status.isEmpty()) {
        auto* statusLabel = new QLabel(status);
        statusLabel->setFont(scaledFont(statusLabel->font(), 0.92, true));
        QPalette palette = statusLabel->palette();
        palette.setColor(QPalette::WindowText, toneColor(tone, palette));
        statusLabel->setPalette(palette);
        layout->addWidget(statusLabel, 0, Qt::AlignVCenter);
    }
    return row;
}

std::string programPath() {
    return QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdString();
}

} // namespace

ConnectionCheck::ConnectionCheck(MainWindow* mainWindow, QWidget* parent)
    : QWidget(parent), mainWindow_(mainWindow)
{
    rows_ = new QVBoxLayout(this);
    rows_->setContentsMargins(0, 0, 0, 0);
    rows_->setSpacing(0);

    lastDatagrams_ = mainWindow_->udpDatagramsReceived();
    // Datagram counter once a second; "receiving" means new datagrams in the
    // last 3 s. The timers belong to this widget and stop with it.
    auto* poll = new QTimer(this);
    poll->setInterval(1000);
    connect(poll, &QTimer::timeout, this, [this] {
        const quint64 count = mainWindow_->udpDatagramsReceived();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (count > lastDatagrams_) lastArrivalMs_ = now;
        lastDatagrams_ = count;
        const bool receiving = lastArrivalMs_ > 0 && now - lastArrivalMs_ < 3000;
        if (receiving != receiving_) {
            receiving_ = receiving;
            refresh();
        }
    });
    poll->start();
    QTimer::singleShot(kNoTelemetryHintMs, this, [this] { waitedLong_ = true; refresh(); });
    connect(mainWindow_, &MainWindow::udpListenerStatusChanged, this, [this] { refresh(); });

    // Lists the adapters itself unless the owner hands them over first.
    PcAddressList::load(this, [this](QVector<tnrp::NetworkInterfaceInfo> list) {
        if (interfacesLoaded_) return;
        if (selectedAddress_.isEmpty()) selectedAddress_ = PcAddressList::preferredAddress(list);
        interfaces_ = std::move(list);
        interfacesLoaded_ = true;
        refresh();
    });

    refresh();
#ifdef Q_OS_WIN
    checkFirewall();
#endif
}

void ConnectionCheck::setInterfaces(const QVector<tnrp::NetworkInterfaceInfo>& interfaces) {
    interfaces_ = interfaces;
    interfacesLoaded_ = true;
    refresh();
}

void ConnectionCheck::setSelectedAddress(const QString& address) {
    selectedAddress_ = address;
    refresh();
}

void ConnectionCheck::setChangePortHandler(std::function<void()> handler) {
    changePort_ = std::move(handler);
    refresh();
}

void ConnectionCheck::checkFirewall() {
    firewall_.reset();
    const uint16_t port = static_cast<uint16_t>(mainWindow_->udpPort());
    const std::string program = programPath();
    runAsync<tnrp::FirewallStatus>(this, [port, program] { return tnrp::firewallStatus(port, program); },
        [this](tnrp::FirewallStatus status) {
            firewall_ = std::move(status);
            refresh();
        });
}

void ConnectionCheck::refresh() {
    clearLayout(rows_);
    const int port = mainWindow_->udpPort();

    // Firewall
    QWidget* firewallRow = nullptr;
#if defined(Q_OS_WIN)
    if (!firewall_) {
        firewallRow = checkRow(Tone::Wait, QStringLiteral("Windows Firewall"), QStringLiteral("Checking…"));
    } else if (!firewall_->supported || firewall_->allowed == "unknown") {
        firewallRow = checkRow(Tone::Warn, QStringLiteral("Windows Firewall"),
                               firewall_->error.empty() ? QStringLiteral("Couldn’t check the firewall.")
                                                        : QString::fromStdString(firewall_->error).toHtmlEscaped(),
                               QStringLiteral("Unknown"));
    } else if (firewall_->allowed == "yes") {
        firewallRow = checkRow(Tone::Ok, QStringLiteral("Windows Firewall"),
                               firewall_->enabled ? QStringLiteral("UDP %1 is allowed.").arg(port)
                                                  : QStringLiteral("The firewall is off."),
                               QStringLiteral("Allowed"));
    } else {
        QString detail = firewall_->blockRule ? QStringLiteral("A firewall rule blocks Track N Race.")
            : firewall_->publicNetwork ? QStringLiteral("Track N Race is blocked on Public networks.")
            : QStringLiteral("Track N Race isn’t allowed through the firewall.");
        if (!firewallError_.isEmpty())
            detail += QStringLiteral("<br><span style='color:#f87171'>%1</span>").arg(firewallError_.toHtmlEscaped());
        auto* allow = new QPushButton(firewallBusy_ ? QStringLiteral("Allowing…")
                                                    : QStringLiteral("Allow through firewall"));
        allow->setIcon(themedIcon(allow, "security-high", QStyle::SP_VistaShield));
        allow->setEnabled(!firewallBusy_);
        connect(allow, &QPushButton::clicked, this, [this] {
            firewallBusy_ = true;
            firewallError_.clear();
            refresh();
            const std::string program = programPath();
            runAsync<std::string>(this, [program] { return tnrp::firewallAllowProgram(program); },
                [this](std::string result) {
                    firewallBusy_ = false;
                    if (!result.empty() && result != "cancelled") firewallError_ = QString::fromStdString(result);
                    if (result.empty()) checkFirewall();
                    else refresh();
                });
        });
        firewallRow = checkRow(Tone::Bad, QStringLiteral("Windows Firewall"), detail, {}, allow);
    }
#elif defined(Q_OS_LINUX)
    const QString ufw = QStringLiteral("sudo ufw allow %1/udp").arg(port);
    auto* copy = new QPushButton(QStringLiteral("Copy command"));
    copy->setIcon(themedIcon(copy, "edit-copy", QStyle::SP_FileDialogDetailedView));
    connect(copy, &QPushButton::clicked, this, [ufw] { QApplication::clipboard()->setText(ufw); });
    firewallRow = checkRow(Tone::Info, QStringLiteral("Firewall"),
                           QStringLiteral("Not checked on Linux. If nothing arrives with ufw on, run <code>%1</code>").arg(ufw),
                           {}, copy);
#else
    firewallRow = checkRow(Tone::Info, QStringLiteral("Firewall"),
                           QStringLiteral("macOS asks to allow incoming connections the first time Track N Race listens."));
#endif
    addSeparated(rows_, firewallRow);

    // Network
    const tnrp::NetworkInterfaceInfo* real = nullptr;
    for (const auto& item : interfaces_)
        if (QString::fromStdString(item.address) == selectedAddress_ && item.kind != "loopback") { real = &item; break; }
    if (!real)
        for (const auto& item : interfaces_)
            if (item.kind != "virtual" && item.kind != "loopback") { real = &item; break; }
    QWidget* networkRow = nullptr;
    if (firewall_ && firewall_->supported && firewall_->publicNetwork) {
        networkRow = checkRow(Tone::Warn, QStringLiteral("Network"),
                              QStringLiteral("%1 set as a Public network.")
                                  .arg(real ? QString::fromStdString(real->name).toHtmlEscaped() + QStringLiteral(" is")
                                            : QStringLiteral("This network is")),
                              QStringLiteral("Public"));
    } else if (real) {
        networkRow = checkRow(Tone::Ok, QStringLiteral("Network"),
                              QStringLiteral("%1 · %2").arg(QString::fromStdString(real->name).toHtmlEscaped(),
                                                                QString::fromStdString(real->address)),
                              QStringLiteral("Connected"));
    } else if (!interfacesLoaded_) {
        networkRow = checkRow(Tone::Wait, QStringLiteral("Network"), QStringLiteral("Checking…"));
    } else {
        networkRow = checkRow(Tone::Warn, QStringLiteral("Network"),
                              QStringLiteral("No network found. Only a game on this PC can send telemetry."),
                              QStringLiteral("Offline"));
    }
    addSeparated(rows_, networkRow);

    // Port
    const QString listenerError = mainWindow_->udpListenerError();
    QWidget* portRow = nullptr;
    if (listenerError.isEmpty()) {
        portRow = checkRow(Tone::Ok, QStringLiteral("Port %1").arg(port), QStringLiteral("Listening for telemetry."),
                           QStringLiteral("Listening"));
    } else {
        static const QString kEnginePrefix = QStringLiteral("engine-startup:");
        const QString shown = listenerError.startsWith(kEnginePrefix) ? listenerError.mid(kEnginePrefix.size()) : listenerError;
        QPushButton* change = nullptr;
        if (changePort_) {
            change = new QPushButton(QStringLiteral("Change port"));
            connect(change, &QPushButton::clicked, this, [this] { changePort_(); });
        }
        portRow = checkRow(Tone::Bad, QStringLiteral("Port %1").arg(port),
                           shown.toHtmlEscaped().replace('\n', QStringLiteral("<br>")), {}, change);
    }
    addSeparated(rows_, portRow);

    // Telemetry
    QWidget* telemetryRow = nullptr;
    if (receiving_) {
        telemetryRow = checkRow(Tone::Ok, QStringLiteral("Telemetry"), QStringLiteral("Packets are arriving from the game."),
                                QStringLiteral("Receiving"));
    } else if (!waitedLong_) {
        telemetryRow = checkRow(Tone::Wait, QStringLiteral("Telemetry"),
                                QStringLiteral("Start a session in the game. This updates on its own."),
                                QStringLiteral("Waiting…"));
    } else {
        auto* tips = new QLabel(QStringLiteral(
            "– <b>UDP Telemetry</b> is On in the game.<br>"
            "– The game’s IP is <b>%1</b> and its port is <b>%2</b>.<br>"
            "– You’re on track or in the garage, not in a menu.<br>"
            "– The console and this PC are on the same network.")
            .arg(selectedAddress_.isEmpty() ? QStringLiteral("this PC") : selectedAddress_.toHtmlEscaped())
            .arg(port));
        tips->setTextFormat(Qt::RichText);
        tips->setWordWrap(true);
        tips->setContentsMargins(0, 8, 0, 0);
        tips->setFont(scaledFont(tips->font(), 0.92));
        telemetryRow = checkRow(Tone::Warn, QStringLiteral("Telemetry"), QStringLiteral("Nothing received yet."),
                                QStringLiteral("No packets"), nullptr, tips);
    }
    addSeparated(rows_, telemetryRow);
}
