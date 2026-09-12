#include "SettingsDialog.h"
#include "../ChartGraphicsBackend.h"
#include "../MainWindow.h"
#include "../CompactSettings.h"
#include "../GraphViewSettings.h"
#include "../IconUtils.h"
#include "../PresentationScheduler.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QCheckBox>
#include <QRadioButton>
#include <QComboBox>
#include <QSlider>
#include <QSpinBox>
#include <QLineEdit>
#include <QStyleFactory>
#include <QPushButton>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QFileDialog>
#include <QSizePolicy>
#include <QFont>
#include <QButtonGroup>
#include <QStackedWidget>
#include <QListWidget>
#include <QPalette>
#include <QApplication>
#include <QAbstractItemView>
#include <QTableWidget>
#include <QHeaderView>
#include <QToolButton>
#include <QStyle>
#include <QTextBrowser>
#include <QClipboard>
#include <QFile>
#include <QTextStream>
#include <QFontDatabase>
#include <QRegularExpression>
#include <QTimer>
#include <iterator>

static QFrame* horizontalSeparator() {
    QFrame* f = new QFrame;
    f->setFrameShape(QFrame::HLine);
    f->setFrameShadow(QFrame::Sunken);
    return f;
}

static QFrame* verticalSeparator() {
    QFrame* f = new QFrame;
    f->setFrameShape(QFrame::VLine);
    f->setFrameShadow(QFrame::Sunken);
    return f;
}

// Segmented-control button, identical to the toolbar's window-size picker (see
// SegmentButton in AppToolbar.cpp). When checked it paints itself as the active
// style's *default button* — the same blue outline the on-toggles wear — which a
// QToolButton can't get for free: the DefaultButton look lives on
// QStyleOptionButton, which only QPushButton feeds the style, so for the checked
// state we draw a default QPushButton bevel + label ourselves. Unchecked
// segments fall through to the normal flat auto-raised look.
namespace {
class SegmentButton : public QToolButton {
public:
    using QToolButton::QToolButton;

protected:
    void paintEvent(QPaintEvent* e) override {
        if (!isChecked()) { QToolButton::paintEvent(e); return; }
        QStylePainter p(this);
        QStyleOptionButton opt;
        opt.initFrom(this);
        opt.rect = rect();
        opt.text = text();
        opt.features = QStyleOptionButton::DefaultButton;
        opt.state |= QStyle::State_Raised;
        opt.state &= ~(QStyle::State_On | QStyle::State_Sunken);
        p.drawControl(QStyle::CE_PushButton, opt);
    }
};
} // namespace

// Bold label spanning both form columns, for a sub-section header inside a page.
static QLabel* subHeading(const QString& text) {
    QLabel* l = new QLabel(text);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

static bool isIpv4Address(const QString& value) {
    static const QRegularExpression octet(QStringLiteral("^[0-9]{1,3}$"));
    const QStringList parts = value.trimmed().split('.', Qt::KeepEmptyParts);
    if (parts.size() != 4) return false;
    for (const QString& part : parts) {
        if (!octet.match(part).hasMatch() || part.toInt() > 255) return false;
    }
    return true;
}

SettingsDialog::SettingsDialog(MainWindow* mainWindow, QWidget* parent)
    : QDialog(parent), mainWindow_(mainWindow)
{
    setWindowTitle("Settings");
    // Qt::Dialog gives the plain dialog frame (no minimize/maximize buttons)
    // without the bitwise flag-stripping the old code needed for that. Modal +
    // parented to MainWindow (see the call site) is what actually keeps this
    // above MainWindow and blocks it — the window-type flag alone doesn't.
    setWindowFlags(Qt::Dialog);
    setWindowModality(Qt::ApplicationModal);

    QVBoxLayout* main = new QVBoxLayout(this);
    // A fixed-size top-level layout makes Qt drop the resize handles and the
    // maximize button (same trick as EditOverviewLayoutDialog) — this should
    // behave like a plain modal child dialog, not a maximizable window.
    main->setSizeConstraint(QLayout::SetFixedSize);
    main->setContentsMargins(0, 0, 0, 0);
    main->setSpacing(0);

    // ── Category tabs over a shared content pane ──────────────────
    // Underline tabs matching the main toolbar's page switcher (see the page
    // tabs in MainWindow.cpp): a row of checkable QToolButtons in an exclusive
    // group, each reserving the same border-bottom width (transparent unless
    // checked) so the accent underline only changes colour, never shifts the
    // text. A QStackedWidget holds the page bodies so the shared Close button
    // can live inside the same bordered pane below.
    QStackedWidget* stack = new QStackedWidget;

    struct Page { const char* title; QWidget* widget; };
    const Page pages[] = {
        { "Appearance",    buildAppearancePage()    },
        { "Compact",       buildCompactPage()       },
        { "Graphs",        buildGraphsPage()        },
        { "Y Axis Behavior", buildYAxisPage()       },
        { "Recording",     buildRecordingPage()     },
        { "Overview",      buildOverviewPage()      },
        { "Track Map",     buildTrackMapPage()      },
        { "Notifications", buildNotificationsPage() },
        { "Protocol",      buildProtocolPage()      },
    };

    QWidget*     tabBar = new QWidget;
    // Tint the bar a shade lighter than the window (Midlight) so it reads as a
    // distinct surface, separated from the bordered content pane below.
    tabBar->setBackgroundRole(QPalette::Button);
    tabBar->setAutoFillBackground(true);
    QHBoxLayout* tabLay = new QHBoxLayout(tabBar);
    tabLay->setContentsMargins(8, 0, 8, 0);
    tabLay->setSpacing(4);
    QButtonGroup* tabGroup = new QButtonGroup(this);
    tabGroup->setExclusive(true);

    static constexpr int kUnderlineWidth = 2;
    const QString accent = QApplication::palette().color(QPalette::Highlight).name();
    const QString tabBtnStyle = QString(
        "QToolButton { padding: 8px 14px; border: none; background: transparent;"
        " border-bottom: %1px solid transparent; }"
        "QToolButton:checked { border-bottom: %1px solid %2; }"
    ).arg(kUnderlineWidth).arg(accent);

    int tabIndex = 0;
    for (const Page& p : pages) {
        QToolButton* b = new QToolButton;
        b->setText(p.title);
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setStyleSheet(tabBtnStyle);
        tabGroup->addButton(b, tabIndex++);
        tabLay->addWidget(b);
        stack->addWidget(p.widget);
    }
    tabLay->addStretch(1);
    connect(tabGroup, &QButtonGroup::idClicked, stack, &QStackedWidget::setCurrentIndex);
    static_cast<QToolButton*>(tabGroup->button(0))->setChecked(true);
    stack->setCurrentIndex(0);

    // Pane holding the page stack + the Close button row. A single top border
    // separates it from the tab bar above; the colour is mixed toward the text
    // colour so it actually contrasts with the background (the palette's
    // Mid/Sunken etch is nearly identical to Window in this dark theme, which is
    // why earlier borders were invisible). Scoped by object name so it doesn't
    // bleed onto children.
    QFrame* pane = new QFrame;
    pane->setObjectName("settingsPane");
    const QColor  win = QApplication::palette().color(QPalette::Window);
    const QColor  txt = QApplication::palette().color(QPalette::WindowText);
    // ~30% of the way from the window colour to the text colour: a clearly
    // visible hairline in both light and dark themes.
    const QColor  borderCol((win.red()   * 7 + txt.red()   * 3) / 10,
                            (win.green() * 7 + txt.green() * 3) / 10,
                            (win.blue()  * 7 + txt.blue()  * 3) / 10);
    pane->setStyleSheet(QString(
        "QFrame#settingsPane { background-color: %1; border-top: 1px solid %2; }")
        .arg(win.name(), borderCol.name()));
    QVBoxLayout* paneLay = new QVBoxLayout(pane);
    paneLay->setContentsMargins(0, 0, 0, 0);
    paneLay->setSpacing(0);
    paneLay->addWidget(stack, 1);
    paneLay->addWidget(horizontalSeparator());

    QHBoxLayout* bottom = new QHBoxLayout;
    bottom->setContentsMargins(12, 8, 12, 12);
    QPushButton* aboutBtn = new QPushButton("About");
    aboutBtn->setIcon(adaptThemeIcon(
        QIcon::fromTheme("info-symbolic"),
        palette().color(QPalette::WindowText),
        style()->standardIcon(QStyle::SP_MessageBoxInformation)));
    connect(aboutBtn, &QPushButton::clicked, this, &SettingsDialog::showAboutDialog);
    bottom->addWidget(aboutBtn);
    bottom->addStretch(1);
    QPushButton* closeBtn = new QPushButton("Close");
    closeBtn->setDefault(true);
    closeBtn->setIcon(adaptThemeIcon(
        QIcon::fromTheme("dialog-close-symbolic"),
        palette().color(QPalette::WindowText),
        style()->standardIcon(QStyle::SP_DialogCloseButton)));
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    bottom->addWidget(closeBtn);
    paneLay->addLayout(bottom);

    main->addWidget(tabBar);
    main->addWidget(pane);
}

// Page scaffold: a right-aligned label/control form (the tab supplies the title).
QWidget* SettingsDialog::makePage(QFormLayout*& formOut) {
    QWidget* page = new QWidget;
    QVBoxLayout* v = new QVBoxLayout(page);
    v->setContentsMargins(8, 12, 8, 8);
    v->setSpacing(12);

    QFormLayout* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(10);
    v->addLayout(form);
    v->addStretch(1);

    formOut = form;
    return page;
}

QWidget* SettingsDialog::buildProtocolPage() {
    QFormLayout* form;
    QWidget* page = makePage(form);

    // Read-only: the format most recently detected from incoming UDP packets,
    // cached by MainWindow::onEngineRow() from protocol_status rows. Shows
    // "—" until the first packet after the engine starts.
    const int detected = mainWindow_->lastDetectedProtocolFormat();
    detectedProtocolLabel_ = new QLabel(detected > 0 ? QString::number(detected) : QStringLiteral("—"));
    form->addRow("Detected Protocol:", detectedProtocolLabel_);

    protocolCombo_ = new QComboBox;
    protocolCombo_->addItem("Auto", "auto");
    protocolCombo_->addItem("2024", "f1_24");
    protocolCombo_->addItem("2025", "f1_25");
    protocolCombo_->addItem("2026", "f1_26");
    const int idx = protocolCombo_->findData(mainWindow_->currentProtocolOverride());
    protocolCombo_->setCurrentIndex(idx >= 0 ? idx : 0);
    connect(protocolCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        mainWindow_->setProtocolOverride(protocolCombo_->currentData().toString());
    });
    form->addRow("Protocol Version Override:", protocolCombo_);

    protocolWarningLabel_ = new QLabel;
    protocolWarningLabel_->setWordWrap(true);
    protocolWarningLabel_->setContentsMargins(12, 10, 12, 10);
    protocolWarningLabel_->setStyleSheet(
        "background-color: rgba(225, 6, 0, 0.08); border: 1px solid rgba(225, 6, 0, 0.35);"
        " border-radius: 6px; color: #e35b57;");
    form->addRow(protocolWarningLabel_);
    updateProtocolWarning(mainWindow_->detectedProtocolWarningFormat(),
                          mainWindow_->forcedProtocolWarningFormat());
    connect(mainWindow_, &MainWindow::protocolWarningChanged,
            this, &SettingsDialog::updateProtocolWarning);

    form->addRow(horizontalSeparator());
    form->addRow(subHeading("Network"));

    // Network controls are a local draft. Nothing is persisted or restarted
    // until Apply & Restart is pressed.
    udpPortSpin_ = new QSpinBox;
    udpPortSpin_->setRange(1, 65535);
    udpPortSpin_->setGroupSeparatorShown(false);   // a port is not a thousands-grouped number
    udpPortSpin_->setValue(mainWindow_->udpPort());
    form->addRow("UDP Port:", udpPortSpin_);

    udpBindAddressEdit_ = new QLineEdit(mainWindow_->udpBindAddress());
    udpBindAddressEdit_->setPlaceholderText(QStringLiteral("0.0.0.0"));
    udpBindAddressEdit_->setToolTip(
        "Which local network interface to receive telemetry on. "
        "0.0.0.0 listens on all interfaces.");
    form->addRow("Bind Address:", udpBindAddressEdit_);

    udpForwardingCheck_ = new QCheckBox("Forward every received packet unchanged");
    udpForwardingCheck_->setChecked(mainWindow_->udpForwardingEnabled());
    form->addRow("UDP Forward Mode:", udpForwardingCheck_);

    udpForwardEditor_ = new QWidget;
    auto* forwardLayout = new QVBoxLayout(udpForwardEditor_);
    forwardLayout->setContentsMargins(0, 0, 0, 0);
    forwardLayout->setSpacing(6);
    auto* forwardHeader = new QHBoxLayout;
    auto* forwardTitle = new QLabel("Forwarding channels (up to 15)");
    QFont forwardTitleFont = forwardTitle->font();
    forwardTitleFont.setBold(true);
    forwardTitle->setFont(forwardTitleFont);
    udpAddForwardTarget_ = new QPushButton("Add channel");
    forwardHeader->addWidget(forwardTitle);
    forwardHeader->addStretch(1);
    forwardHeader->addWidget(udpAddForwardTarget_);
    forwardLayout->addLayout(forwardHeader);

    udpForwardTargets_ = new QTableWidget(0, 3);
    udpForwardTargets_->setHorizontalHeaderLabels({"IPv4 destination", "Port", QString()});
    udpForwardTargets_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    udpForwardTargets_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    udpForwardTargets_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    udpForwardTargets_->verticalHeader()->setVisible(false);
    udpForwardTargets_->setSelectionMode(QAbstractItemView::NoSelection);
    udpForwardTargets_->setFocusPolicy(Qt::NoFocus);
    udpForwardTargets_->setFixedHeight(170);
    forwardLayout->addWidget(udpForwardTargets_);

    udpForwardValidation_ = new QLabel(
        "Enter valid IPv4 destinations and ports. Forwarding back to this listener "
        "would create a packet loop.");
    udpForwardValidation_->setWordWrap(true);
    udpForwardValidation_->setStyleSheet("color: #e35b57;");
    forwardLayout->addWidget(udpForwardValidation_);
    form->addRow(udpForwardEditor_);

    for (const UdpForwardTargetSetting& target : mainWindow_->udpForwardTargets())
        addForwardTargetRow(target.address, target.port);

    QWidget* applyRow = new QWidget;
    auto* applyLayout = new QHBoxLayout(applyRow);
    applyLayout->setContentsMargins(0, 0, 0, 0);
    udpApplyStatus_ = new QLabel;
    udpApplyStatus_->setWordWrap(true);
    udpApplyButton_ = new QPushButton("Apply & Restart");
    applyLayout->addWidget(udpApplyStatus_, 1);
    applyLayout->addWidget(udpApplyButton_);
    form->addRow(applyRow);

    udpStatusResetTimer_ = new QTimer(this);
    udpStatusResetTimer_->setSingleShot(true);
    udpStatusResetTimer_->setInterval(2500);
    connect(udpStatusResetTimer_, &QTimer::timeout, this, [this] {
        udpApplyState_ = UdpApplyState::Idle;
        udpApplyError_.clear();
        refreshNetworkDraftUi();
    });
    connect(udpPortSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int) { refreshNetworkDraftUi(); });
    connect(udpBindAddressEdit_, &QLineEdit::textChanged, this,
            [this](const QString&) { refreshNetworkDraftUi(); });
    connect(udpForwardingCheck_, &QCheckBox::toggled, this,
            [this](bool) { refreshNetworkDraftUi(); });
    connect(udpAddForwardTarget_, &QPushButton::clicked, this,
            [this] { addForwardTargetRow(); });
    connect(udpApplyButton_, &QPushButton::clicked,
            this, &SettingsDialog::applyNetworkDraft);
    refreshNetworkDraftUi();

    return page;
}

void SettingsDialog::updateProtocolWarning(int detectedFormat, int forcedFormat) {
    if (!protocolWarningLabel_) return;
    const bool visible = detectedFormat > 0 && forcedFormat > 0;
    protocolWarningLabel_->setVisible(visible);
    if (visible) {
        protocolWarningLabel_->setText(
            QString("Protocol mismatch detected\nReceiving %1 packets — override is set to %2")
                .arg(detectedFormat).arg(forcedFormat));
    } else {
        protocolWarningLabel_->clear();
    }
}

void SettingsDialog::addForwardTargetRow(const QString& address, int port) {
    if (!udpForwardTargets_ || udpForwardTargets_->rowCount() >= 15) return;

    const int row = udpForwardTargets_->rowCount();
    udpForwardTargets_->insertRow(row);
    auto* addressEdit = new QLineEdit(address);
    addressEdit->setPlaceholderText("192.168.1.100");
    auto* portSpin = new QSpinBox;
    portSpin->setRange(1, 65535);
    portSpin->setGroupSeparatorShown(false);
    portSpin->setValue(port);
    auto* removeButton = new QToolButton;
    removeButton->setText(QStringLiteral("×"));
    removeButton->setToolTip(QString("Remove forwarding channel %1").arg(row + 1));
    udpForwardTargets_->setCellWidget(row, 0, addressEdit);
    udpForwardTargets_->setCellWidget(row, 1, portSpin);
    udpForwardTargets_->setCellWidget(row, 2, removeButton);

    connect(addressEdit, &QLineEdit::textChanged, this,
            [this](const QString&) { refreshNetworkDraftUi(); });
    connect(portSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int) { refreshNetworkDraftUi(); });
    connect(removeButton, &QToolButton::clicked, this,
            [this, addressEdit] { removeForwardTargetRow(addressEdit); });
    refreshNetworkDraftUi();
}

void SettingsDialog::removeForwardTargetRow(QWidget* addressEditor) {
    if (!udpForwardTargets_) return;
    for (int row = 0; row < udpForwardTargets_->rowCount(); ++row) {
        if (udpForwardTargets_->cellWidget(row, 0) == addressEditor) {
            udpForwardTargets_->removeRow(row);
            break;
        }
    }
    refreshNetworkDraftUi();
}

QVector<UdpForwardTargetSetting> SettingsDialog::forwardTargetDraft() const {
    QVector<UdpForwardTargetSetting> targets;
    if (!udpForwardTargets_) return targets;
    targets.reserve(udpForwardTargets_->rowCount());
    for (int row = 0; row < udpForwardTargets_->rowCount(); ++row) {
        const auto* address = qobject_cast<QLineEdit*>(udpForwardTargets_->cellWidget(row, 0));
        const auto* port = qobject_cast<QSpinBox*>(udpForwardTargets_->cellWidget(row, 1));
        if (address && port) targets.push_back({address->text(), port->value()});
    }
    return targets;
}

bool SettingsDialog::networkDraftValid() const {
    if (!udpForwardingCheck_ || !udpForwardingCheck_->isChecked()) return true;
    const QString listenerAddress = udpBindAddressEdit_->text().trimmed();
    const int listenerPort = udpPortSpin_->value();
    for (const UdpForwardTargetSetting& target : forwardTargetDraft()) {
        const QString address = target.address.trimmed();
        if (!isIpv4Address(address) || target.port < 1 || target.port > 65535 ||
            (target.port == listenerPort &&
             (address.startsWith("127.") || address == listenerAddress))) {
            return false;
        }
    }
    return true;
}

bool SettingsDialog::networkDraftDirty() const {
    return udpPortSpin_->value() != mainWindow_->udpPort() ||
           udpBindAddressEdit_->text() != mainWindow_->udpBindAddress() ||
           udpForwardingCheck_->isChecked() != mainWindow_->udpForwardingEnabled() ||
           forwardTargetDraft() != mainWindow_->udpForwardTargets();
}

void SettingsDialog::refreshNetworkDraftUi() {
    if (!udpApplyButton_) return;
    const bool forwarding = udpForwardingCheck_->isChecked();
    udpForwardEditor_->setVisible(forwarding);
    const bool valid = networkDraftValid();
    udpForwardValidation_->setVisible(forwarding && !valid);
    udpAddForwardTarget_->setEnabled(udpForwardTargets_->rowCount() < 15);
    udpApplyButton_->setEnabled(valid && udpApplyState_ != UdpApplyState::Applying);

    // Mark only invalid destination fields, while retaining the single shared
    // explanation below the channel table.
    const QString listenerAddress = udpBindAddressEdit_->text().trimmed();
    const int listenerPort = udpPortSpin_->value();
    for (int row = 0; row < udpForwardTargets_->rowCount(); ++row) {
        auto* address = qobject_cast<QLineEdit*>(udpForwardTargets_->cellWidget(row, 0));
        auto* port = qobject_cast<QSpinBox*>(udpForwardTargets_->cellWidget(row, 1));
        if (!address || !port) continue;
        const QString destination = address->text().trimmed();
        const bool rowValid = isIpv4Address(destination) &&
            !(port->value() == listenerPort &&
              (destination.startsWith("127.") || destination == listenerAddress));
        const QString invalidStyle = rowValid ? QString() : QStringLiteral("border: 1px solid #c62828;");
        address->setStyleSheet(invalidStyle);
        port->setStyleSheet(invalidStyle);
    }

    QString status;
    QString color;
    switch (udpApplyState_) {
        case UdpApplyState::Applying:
            status = QStringLiteral("Restarting…");
            udpApplyButton_->setText(QStringLiteral("Restarting…"));
            break;
        case UdpApplyState::Ok:
            status = QStringLiteral("Listener restarted successfully");
            color = QStringLiteral("#4caf50");
            udpApplyButton_->setText(QStringLiteral("Applied"));
            break;
        case UdpApplyState::Error:
            status = udpApplyError_;
            color = QStringLiteral("#e35b57");
            udpApplyButton_->setText(QStringLiteral("Apply & Restart"));
            break;
        case UdpApplyState::Idle: {
            const bool dirty = networkDraftDirty();
            status = dirty
                ? QStringLiteral("Unsaved changes")
                : QStringLiteral("Restart the listener to apply changes");
            if (dirty) color = QStringLiteral("#d49b2e");
            udpApplyButton_->setText(QStringLiteral("Apply & Restart"));
            break;
        }
    }
    udpApplyStatus_->setText(status);
    udpApplyStatus_->setStyleSheet(color.isEmpty() ? QString() : "color: " + color + ";");
}

void SettingsDialog::applyNetworkDraft() {
    if (!networkDraftValid() || udpApplyState_ == UdpApplyState::Applying) return;
    udpStatusResetTimer_->stop();
    udpApplyState_ = UdpApplyState::Applying;
    udpApplyError_.clear();
    refreshNetworkDraftUi();

    // Defer the synchronous stop/start by one event-loop turn so "Restarting…"
    // can paint before the listener is recreated.
    QTimer::singleShot(0, this, [this] {
        QVector<UdpForwardTargetSetting> targets = forwardTargetDraft();
        for (int row = 0; row < targets.size(); ++row) {
            targets[row].address = targets[row].address.trimmed();
            if (auto* address = qobject_cast<QLineEdit*>(udpForwardTargets_->cellWidget(row, 0)))
                address->setText(targets[row].address);
        }
        const QString error = mainWindow_->applyUdpConfiguration(
            udpPortSpin_->value(), udpBindAddressEdit_->text(),
            udpForwardingCheck_->isChecked(), targets);
        if (error.isEmpty()) {
            udpApplyState_ = UdpApplyState::Ok;
        } else {
            udpApplyState_ = UdpApplyState::Error;
            udpApplyError_ = error;
        }
        refreshNetworkDraftUi();
        udpStatusResetTimer_->start();
    });
}

QWidget* SettingsDialog::buildRecordingPage() {
    QFormLayout* form;
    QWidget* page = makePage(form);

    recordCheck_ = new QCheckBox("Auto-record when a session starts");
    recordCheck_->setChecked(mainWindow_->autoRecordEnabled());
    form->addRow(QString(), recordCheck_);

    QWidget* dirRow = new QWidget;
    QHBoxLayout* dirLay = new QHBoxLayout(dirRow);
    dirLay->setContentsMargins(0, 0, 0, 0);
    const QString dir = mainWindow_->currentOutputDirectory();
    dirLabel_ = new QLabel(dir.isEmpty() ? "No directory selected." : dir);
    dirLabel_->setWordWrap(true);
    dirLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    dirLabel_->setMaximumWidth(280);
    QPushButton* browseBtn = new QPushButton("Browse…");
    browseBtn->setFixedWidth(90);
    dirLay->addWidget(dirLabel_);
    dirLay->addWidget(browseBtn);
    form->addRow("Save to:", dirRow);

    connect(recordCheck_, &QCheckBox::toggled, this, [this](bool on) {
        mainWindow_->setAutoRecord(on);
    });
    connect(browseBtn, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(
            this, "Select Output Directory", mainWindow_->currentOutputDirectory());
        if (!dir.isEmpty()) {
            mainWindow_->setOutputDirectory(dir);
            dirLabel_->setText(dir);
        }
    });
    return page;
}

QWidget* SettingsDialog::buildAppearancePage() {
    QFormLayout* form;
    QWidget* page = makePage(form);

    QWidget* themeRow = new QWidget;
    QHBoxLayout* themeLay = new QHBoxLayout(themeRow);
    themeLay->setContentsMargins(0, 0, 0, 0);
    themeLay->setSpacing(14);
    themeSystem_ = new QRadioButton("System default");
    themeLight_  = new QRadioButton("Light");
    themeDark_   = new QRadioButton("Dark");
    themeLay->addWidget(themeSystem_);
    themeLay->addWidget(themeLight_);
    themeLay->addWidget(themeDark_);

    const QString theme = mainWindow_->currentTheme();
    if (theme == "light")     themeLight_->setChecked(true);
    else if (theme == "dark") themeDark_->setChecked(true);
    else                      themeSystem_->setChecked(true);

    form->addRow("Theme:", themeRow);

    // Style selector — lists the QStyles actually available at runtime, so a
    // bundled "Breeze" only appears once its plugin has loaded. "System default"
    // restores Qt's native platform style. Each item's userData is the lowercased
    // QStyleFactory key (what setStyleName/QSettings store); "system" is special.
    styleCombo_ = new QComboBox;
    styleCombo_->addItem("System default", "system");
    const QString curStyle = mainWindow_->currentStyleName();
    for (const QString& key : QStyleFactory::keys()) {
        styleCombo_->addItem(key, key.toLower());
        if (key.compare(curStyle, Qt::CaseInsensitive) == 0)
            styleCombo_->setCurrentIndex(styleCombo_->count() - 1);
    }
    // Connect after populating so the initial setCurrentIndex above doesn't
    // re-trigger an apply during construction.
    connect(styleCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        mainWindow_->setStyleName(styleCombo_->currentData().toString());
    });
    form->addRow("Style:", styleCombo_);

    toolbarLabelsCheck_ = new QCheckBox("Show button labels in toolbar");
    toolbarLabelsCheck_->setChecked(mainWindow_->toolbarLabelsEnabled());
    form->addRow("Toolbar:", toolbarLabelsCheck_);

    form->addRow(horizontalSeparator());
    form->addRow(subHeading("Graphs"));

    chartBackendCombo_ = new QComboBox;
    chartBackendCombo_->addItem(
        QString("Automatic (currently %1)").arg(tnr::graphics::activeBackendLabel()), "auto");
    for (const tnr::graphics::BackendInfo& backend : tnr::graphics::supportedBackends())
        chartBackendCombo_->addItem(backend.label, backend.key);
    int backendIndex = chartBackendCombo_->findData(mainWindow_->chartGraphicsBackend());
    chartBackendCombo_->setCurrentIndex(backendIndex >= 0 ? backendIndex : 0);
    chartBackendCombo_->setToolTip(
        "Graphics API used by the telemetry charts. A restart is required after changing it.");
    connect(chartBackendCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        mainWindow_->setChartGraphicsBackend(chartBackendCombo_->currentData().toString());
    });
    QWidget* backendRow = new QWidget;
    auto* backendLayout = new QHBoxLayout(backendRow);
    backendLayout->setContentsMargins(0, 0, 0, 0);
    backendLayout->setSpacing(10);
    backendLayout->addWidget(chartBackendCombo_);
    auto* restartNote = new QLabel("Restart required after changing the graphics API.");
    backendLayout->addWidget(restartNote);
    backendLayout->addStretch(1);
    form->addRow("Graphics API:", backendRow);

    // Portable QRhi multisampling: higher values smooth lines but increase fill cost.
    chartMsaaCombo_ = new QComboBox;
    chartMsaaCombo_->addItem("Off", 0);
    for (int s : { 4, 8, 16 })
        chartMsaaCombo_->addItem(QString("%1x").arg(s), s);
    const int curMsaa = mainWindow_->chartMsaaSamples();
    const int msaaIdx = chartMsaaCombo_->findData(curMsaa);
    chartMsaaCombo_->setCurrentIndex(msaaIdx >= 0 ? msaaIdx : chartMsaaCombo_->findData(4));
    // Connect after selecting so the initial index set above doesn't apply.
    connect(chartMsaaCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        mainWindow_->setChartMsaaSamples(chartMsaaCombo_->currentData().toInt());
    });
    form->addRow("Anti-aliasing:", chartMsaaCombo_);

    auto populateFrameRates = [](QComboBox* combo) {
        combo->addItem("Pause", 0);
        combo->addItem("1 FPS", 1);
        combo->addItem("10 FPS", 10);
        combo->addItem("30 FPS", 30);
        combo->addItem("60 FPS", 60);
        combo->addItem("120 FPS", 120);
        combo->addItem("Match display", PresentationScheduler::MatchDisplay);
    };

    chartFpsInFocusCombo_ = new QComboBox;
    populateFrameRates(chartFpsInFocusCombo_);
    int fpsIdx = chartFpsInFocusCombo_->findData(mainWindow_->chartFpsInFocus());
    chartFpsInFocusCombo_->setCurrentIndex(
        fpsIdx >= 0 ? fpsIdx : chartFpsInFocusCombo_->findData(PresentationScheduler::MatchDisplay));
    chartFpsInFocusCombo_->setToolTip(
        "Maximum chart frame rate while the application window is focused.");
    connect(chartFpsInFocusCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        mainWindow_->setChartFpsInFocus(chartFpsInFocusCombo_->currentData().toInt());
    });
    form->addRow("FPS in focus:", chartFpsInFocusCombo_);

    chartFpsOutOfFocusCombo_ = new QComboBox;
    populateFrameRates(chartFpsOutOfFocusCombo_);
    fpsIdx = chartFpsOutOfFocusCombo_->findData(mainWindow_->chartFpsOutOfFocus());
    chartFpsOutOfFocusCombo_->setCurrentIndex(
        fpsIdx >= 0 ? fpsIdx : chartFpsOutOfFocusCombo_->findData(30));
    chartFpsOutOfFocusCombo_->setToolTip(
        "Maximum chart frame rate while the application window is not focused.");
    connect(chartFpsOutOfFocusCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        mainWindow_->setChartFpsOutOfFocus(chartFpsOutOfFocusCombo_->currentData().toInt());
    });
    form->addRow("FPS out of focus:", chartFpsOutOfFocusCombo_);

    form->addRow(horizontalSeparator());
    form->addRow(subHeading("Accessibility"));

    QWidget* contrastRow = new QWidget;
    QHBoxLayout* ch = new QHBoxLayout(contrastRow);
    ch->setContentsMargins(0, 0, 0, 0);
    QSlider* contrastSlider = new QSlider(Qt::Horizontal);
    contrastSlider->setRange(100, 2100);
    contrastSlider->setValue((int)(mainWindow_->contrastThreshold() * 100.0f));
    QLabel* contrastVal = new QLabel(QString::number(mainWindow_->contrastThreshold(), 'f', 2));
    contrastVal->setFixedWidth(40);
    ch->addWidget(contrastSlider);
    ch->addWidget(contrastVal);
    form->addRow("Contrast Threshold:", contrastRow);

    auto applyTheme = [this](bool) {
        QString val = "system";
        if (themeLight_->isChecked())     val = "light";
        else if (themeDark_->isChecked()) val = "dark";
        mainWindow_->setTheme(val);
    };
    connect(themeSystem_, &QRadioButton::toggled, this, applyTheme);
    connect(themeLight_,  &QRadioButton::toggled, this, applyTheme);
    connect(themeDark_,   &QRadioButton::toggled, this, applyTheme);
    connect(toolbarLabelsCheck_, &QCheckBox::toggled, this, [this](bool on) {
        mainWindow_->setToolbarLabels(on);
    });
    connect(contrastSlider, &QSlider::valueChanged, this, [this, contrastVal](int val) {
        float f = val / 100.0f;
        contrastVal->setText(QString::number(f, 'f', 2));
        mainWindow_->setContrastThreshold(f);
    });
    return page;
}

QWidget* SettingsDialog::buildCompactPage() {
    // Sidebar (group list) on the left, a stack of per-group control pages on
    // the right — same layout as the Graphs page.
    QWidget* page = new QWidget;
    QHBoxLayout* h = new QHBoxLayout(page);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);

    // Registry of every section's segmented group, so the "Toggle all" button
    // below the sidebar can both flip the setting and re-check the right segment.
    struct Ctl { tnr::CompactSection s; QButtonGroup* group; };
    QList<Ctl> controls;

    // One control = a label on the left and a density segmented control on the
    // right, exactly like the toolbar's window-size row: an exclusive
    // edge-to-edge row of checkable buttons, the active one wearing the native
    // default-button outline. Most sections are a 2-way Normal/Compact; the
    // Overview tyre cards add three extra levels, so that one is 5-way. Weather
    // has Normal plus two compact layouts.
    auto makeControl = [this, &controls](const char* label, tnr::CompactSection s) -> QWidget* {
        QWidget* w = new QWidget;
        QHBoxLayout* cv = new QHBoxLayout(w);
        cv->setContentsMargins(0, 0, 0, 0);
        cv->setSpacing(12);
        QLabel* cap = new QLabel(label);

        QWidget* seg = new QWidget;
        QHBoxLayout* segLay = new QHBoxLayout(seg);
        segLay->setContentsMargins(0, 0, 0, 0);
        segLay->setSpacing(0);
        QButtonGroup* group = new QButtonGroup(w);
        group->setExclusive(true);
        int idc = 0;
        auto addSeg = [&](const char* text) {
            SegmentButton* b = new SegmentButton;
            b->setText(text);
            b->setCheckable(true);
            b->setAutoRaise(true);
            group->addButton(b, idc++);
            segLay->addWidget(b);
        };

        if (s == tnr::CompactSection::OverviewTyres) {
            addSeg("Normal"); addSeg("Compact 1");
            addSeg("Compact 2"); addSeg("Compact 3"); addSeg("Compact 4");
            group->button(mainWindow_->tyresCompactLevel())->setChecked(true);
            connect(group, &QButtonGroup::idClicked, this,
                    [this](int idx) { mainWindow_->setTyresCompactLevel(idx); });
        } else if (s == tnr::CompactSection::SessionWeather) {
            addSeg("Normal"); addSeg("Compact 1"); addSeg("Compact 2"); addSeg("Compact 3");
            group->button(mainWindow_->weatherCompactLevel())->setChecked(true);
            connect(group, &QButtonGroup::idClicked, this,
                    [this](int idx) { mainWindow_->setWeatherCompactLevel(idx); });
        } else if (s == tnr::CompactSection::SessionHeader) {
            addSeg("Normal"); addSeg("Compact 1"); addSeg("Compact 2");
            group->button(mainWindow_->headerCompactLevel())->setChecked(true);
            connect(group, &QButtonGroup::idClicked, this,
                    [this](int idx) { mainWindow_->setHeaderCompactLevel(idx); });
        } else {
            addSeg("Normal"); addSeg("Compact");
            group->button(mainWindow_->compactSection(s) ? 1 : 0)->setChecked(true);
            connect(group, &QButtonGroup::idClicked, this,
                    [this, s](int idx) { mainWindow_->setCompactSection(s, idx == 1); });
        }
        controls.push_back({ s, group });

        cv->addWidget(cap);
        cv->addStretch(1);
        cv->addWidget(seg);
        return w;
    };

    struct Row { tnr::CompactSection s; const char* group; const char* label; };
    static const Row rows[] = {
        { tnr::CompactSection::OverviewStats,   "Overview", "Stats row" },
        { tnr::CompactSection::OverviewDamage,  "Overview", "Damage cards" },
        { tnr::CompactSection::OverviewTyres,   "Overview", "Tyre cards" },
        { tnr::CompactSection::SessionCards,    "Session",  "Info cards" },
        { tnr::CompactSection::SessionProximity, "Session", "Proximity" },
        { tnr::CompactSection::SessionEvents,   "Session",  "Events" },
        { tnr::CompactSection::SessionWeather,  "Session",  "Weather strip" },
        { tnr::CompactSection::SessionHeader,   "Session",  "Header" },
        { tnr::CompactSection::PowerCards,      "Power",    "Cards" },
        { tnr::CompactSection::StrategySummary, "Strategy", "Summary" },
    };

    // Left nav column listing the groups. Tinted a shade lighter than the
    // window (Button role) so it reads as a distinct surface, matching the top
    // tab bar; the selected row uses the accent highlight.
    QListWidget* sidebar = new QListWidget;
    sidebar->setFrameShape(QFrame::NoFrame);
    sidebar->setMinimumWidth(130);
    sidebar->setMaximumWidth(160);
    sidebar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    const QString sidebarBg = QApplication::palette().color(QPalette::Button).name();
    sidebar->setStyleSheet(QString(
        "QListWidget { background: %1; border: none; outline: none; }"
        "QListWidget::item { padding: 8px 14px; }"
    ).arg(sidebarBg));

    QStackedWidget* stack = new QStackedWidget;

    // rows are grouped consecutively, so each time the group changes we start a
    // fresh sidebar entry + stack page and stack the group's controls into it,
    // one Label/segment row per section.
    QString lastGroup;
    QVBoxLayout* colLay = nullptr;
    for (const Row& r : rows) {
        if (r.group != lastGroup) {
            sidebar->addItem(r.group);

            QWidget* groupPage = new QWidget;
            QVBoxLayout* gv = new QVBoxLayout(groupPage);
            gv->setContentsMargins(16, 12, 16, 8);
            gv->setSpacing(10);
            gv->addWidget(subHeading(r.group));

            QWidget* colW = new QWidget;
            colLay = new QVBoxLayout(colW);
            colLay->setContentsMargins(0, 0, 0, 0);
            colLay->setSpacing(8);
            gv->addWidget(colW);
            gv->addStretch(1);

            stack->addWidget(groupPage);
            lastGroup = r.group;
        }
        colLay->addWidget(makeControl(r.label, r.s));
    }

    connect(sidebar, &QListWidget::currentRowChanged,
            stack, &QStackedWidget::setCurrentIndex);
    sidebar->setCurrentRow(0);

    // Pinned to the bottom of the sidebar column. If any section is already
    // compact the click resets everything to Normal; otherwise it makes
    // everything compact (the tyre-cards level goes to Compact 1). The label
    // shows the action the next click will perform. Programmatic setChecked()
    // doesn't emit idClicked, so re-checking the segments here doesn't re-fire
    // the per-control handlers — we push each setting directly.
    QToolButton* toggleAllBtn = new QToolButton;
    toggleAllBtn->setAutoRaise(true);
    toggleAllBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    toggleAllBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto anyCompact = [this, controls]() {
        for (const Ctl& c : controls) {
            const bool compact = c.s == tnr::CompactSection::OverviewTyres
                ? mainWindow_->tyresCompactLevel() != 0
                : c.s == tnr::CompactSection::SessionWeather
                    ? mainWindow_->weatherCompactLevel() != 0
                    : c.s == tnr::CompactSection::SessionHeader
                        ? mainWindow_->headerCompactLevel() != 0
                        : mainWindow_->compactSection(c.s);
            if (compact) return true;
        }
        return false;
    };
    auto refreshLabel = [anyCompact, toggleAllBtn]() {
        toggleAllBtn->setText(anyCompact() ? "Set Normal" : "Set Compact");
    };
    refreshLabel();

    connect(toggleAllBtn, &QToolButton::clicked, this,
            [this, controls, anyCompact, refreshLabel]() {
        const bool makeCompact = !anyCompact();   // all Normal → compact; else → Normal
        for (const Ctl& c : controls) {
            if (c.s == tnr::CompactSection::OverviewTyres) {
                const int lvl = makeCompact ? 1 : 0;   // 1 == "Compact 1"
                mainWindow_->setTyresCompactLevel(lvl);
                c.group->button(lvl)->setChecked(true);
            } else if (c.s == tnr::CompactSection::SessionWeather) {
                const int lvl = makeCompact ? 1 : 0;
                mainWindow_->setWeatherCompactLevel(lvl);
                c.group->button(lvl)->setChecked(true);
            } else if (c.s == tnr::CompactSection::SessionHeader) {
                const int lvl = makeCompact ? 1 : 0;
                mainWindow_->setHeaderCompactLevel(lvl);
                c.group->button(lvl)->setChecked(true);
            } else {
                mainWindow_->setCompactSection(c.s, makeCompact);
                c.group->button(makeCompact ? 1 : 0)->setChecked(true);
            }
        }
        refreshLabel();
    });
    // Keep the label current when individual sections are changed directly.
    for (const Ctl& c : controls)
        connect(c.group, &QButtonGroup::idClicked, this, [refreshLabel](int) { refreshLabel(); });

    QWidget* sideCol = new QWidget;
    sideCol->setAutoFillBackground(true);
    sideCol->setBackgroundRole(QPalette::Button);
    QVBoxLayout* sideColLay = new QVBoxLayout(sideCol);
    sideColLay->setContentsMargins(0, 0, 0, 0);
    sideColLay->setSpacing(0);
    sideColLay->addWidget(sidebar, 1);
    QWidget* btnWrap = new QWidget;
    QVBoxLayout* btnWrapLay = new QVBoxLayout(btnWrap);
    btnWrapLay->setContentsMargins(8, 8, 8, 8);
    btnWrapLay->addWidget(toggleAllBtn);
    sideColLay->addWidget(btnWrap);

    h->addWidget(sideCol);
    h->addWidget(verticalSeparator());
    h->addWidget(stack, 1);
    return page;
}

QWidget* SettingsDialog::buildGraphsPage() {
    // Sidebar (group list) on the left, a stack of per-group control pages on
    // the right. Each sidebar row maps 1:1 to a stack page; there are far more
    // graphs than fit comfortably in one flat list, so grouping them behind a
    // navigation column keeps each pane short.
    QWidget* page = new QWidget;
    QHBoxLayout* h = new QHBoxLayout(page);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);

    // Registry of every graph's segmented group, so the "Set Chart/Table" button
    // below the sidebar can both flip the setting and re-check the right segment.
    struct Ctl { tnr::GraphSection s; QButtonGroup* group; };
    QList<Ctl> controls;

    // One control = a label on the left and a Chart/Table segmented control on
    // the right, exactly like the toolbar's window-size row: an exclusive
    // edge-to-edge pair of checkable buttons, the active one wearing the native
    // default-button outline. Each graph can independently show as its chart or
    // as a table of the raw sample values behind it (see GraphTable / the charts
    // widgets).
    auto makeControl = [this, &controls](const char* label, tnr::GraphSection s) -> QWidget* {
        QWidget* w = new QWidget;
        QHBoxLayout* cv = new QHBoxLayout(w);
        cv->setContentsMargins(0, 0, 0, 0);
        cv->setSpacing(12);
        QLabel* cap = new QLabel(label);

        QWidget* seg = new QWidget;
        QHBoxLayout* segLay = new QHBoxLayout(seg);
        segLay->setContentsMargins(0, 0, 0, 0);
        segLay->setSpacing(0);
        QButtonGroup* group = new QButtonGroup(w);
        group->setExclusive(true);
        // Tyre cards toggle Card ⇄ Table; every other graph toggles Chart ⇄ Table.
        const bool isCard =
               s == tnr::GraphSection::TyreCardFL || s == tnr::GraphSection::TyreCardFR
            || s == tnr::GraphSection::TyreCardRL || s == tnr::GraphSection::TyreCardRR
            || s == tnr::GraphSection::OverviewTyreCardFL || s == tnr::GraphSection::OverviewTyreCardFR
            || s == tnr::GraphSection::OverviewTyreCardRL || s == tnr::GraphSection::OverviewTyreCardRR;
        const char* const opts[] = { isCard ? "Card" : "Chart", "Table" };
        for (int i = 0; i < 2; ++i) {
            SegmentButton* b = new SegmentButton;
            b->setText(opts[i]);
            b->setCheckable(true);
            b->setAutoRaise(true);
            group->addButton(b, i);
            segLay->addWidget(b);
        }
        group->button(mainWindow_->graphView(s) ? 1 : 0)->setChecked(true);
        connect(group, &QButtonGroup::idClicked, this,
                [this, s](int idx) { mainWindow_->setGraphView(s, idx == 1); });
        controls.push_back({ s, group });

        cv->addWidget(cap);
        cv->addStretch(1);
        cv->addWidget(seg);
        return w;
    };

    struct Row { tnr::GraphSection s; const char* group; const char* label; };
    static const Row rows[] = {
        { tnr::GraphSection::OverviewTelemetry,   "Overview", "Speed / RPM / ERS" },
        { tnr::GraphSection::OverviewTyreSurface, "Overview", "Tyre surface temp" },
        { tnr::GraphSection::OverviewTyreInner,   "Overview", "Tyre inner temp" },
        { tnr::GraphSection::OverviewTyreBrake,   "Overview", "Tyre brake temp" },
        { tnr::GraphSection::OverviewTyreWear,    "Overview", "Tyre wear / life" },
        { tnr::GraphSection::OverviewTyreCardFL,  "Overview", "Tyre card FL" },
        { tnr::GraphSection::OverviewTyreCardFR,  "Overview", "Tyre card FR" },
        { tnr::GraphSection::OverviewTyreCardRL,  "Overview", "Tyre card RL" },
        { tnr::GraphSection::OverviewTyreCardRR,  "Overview", "Tyre card RR" },
        { tnr::GraphSection::TyreSurface,        "Tyres",    "Surface temp" },
        { tnr::GraphSection::TyreInner,          "Tyres",    "Inner temp" },
        { tnr::GraphSection::TyreBrake,          "Tyres",    "Brake temp" },
        { tnr::GraphSection::TyreWear,           "Tyres",    "Wear / life" },
        { tnr::GraphSection::TyreCardFL,         "Tyres",    "Front-left card" },
        { tnr::GraphSection::TyreCardFR,         "Tyres",    "Front-right card" },
        { tnr::GraphSection::TyreCardRL,         "Tyres",    "Rear-left card" },
        { tnr::GraphSection::TyreCardRR,         "Tyres",    "Rear-right card" },
        { tnr::GraphSection::InputGear,          "Input",    "Gear" },
        { tnr::GraphSection::InputThrottleBrake, "Input",    "Throttle / brake" },
        { tnr::GraphSection::InputSteering,      "Input",    "Steering" },
        { tnr::GraphSection::PowerSplit,         "Power",    "Power" },
        { tnr::GraphSection::PowerHarvest,       "Power",    "ERS harvest" },
        { tnr::GraphSection::PowerStore,         "Power",    "ERS store" },
        { tnr::GraphSection::PowerFuel,          "Power",    "Fuel" },
        { tnr::GraphSection::MiscGForce,         "Misc",     "G-force" },
        { tnr::GraphSection::MiscRideHeight,     "Misc",     "Ride height" },
    };

    // Left nav column listing the groups. Tinted a shade lighter than the
    // window (Button role) so it reads as a distinct surface, matching the top
    // tab bar; the selected row uses the accent highlight.
    QListWidget* sidebar = new QListWidget;
    sidebar->setFrameShape(QFrame::NoFrame);
    sidebar->setMinimumWidth(130);
    sidebar->setMaximumWidth(160);
    sidebar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    const QString sidebarBg = QApplication::palette().color(QPalette::Button).name();
    sidebar->setStyleSheet(QString(
        "QListWidget { background: %1; border: none; outline: none; }"
        "QListWidget::item { padding: 8px 14px; }"
    ).arg(sidebarBg));

    QStackedWidget* stack = new QStackedWidget;

    // rows are grouped consecutively, so each time the group changes we start a
    // fresh sidebar entry + stack page and stack the group's controls into it,
    // one Label/toggle row per graph.
    QString lastGroup;
    QVBoxLayout* colLay = nullptr;
    for (const Row& r : rows) {
        if (r.group != lastGroup) {
            sidebar->addItem(r.group);

            QWidget* groupPage = new QWidget;
            QVBoxLayout* gv = new QVBoxLayout(groupPage);
            gv->setContentsMargins(16, 12, 16, 8);
            gv->setSpacing(10);
            gv->addWidget(subHeading(r.group));

            if (QString::fromLatin1(r.group) == "Overview") {
                auto* vertical = new QCheckBox("Secondary Vertical Crosshair");
                auto* horizontal = new QCheckBox("Secondary Horizontal Crosshair");
                vertical->setChecked(mainWindow_->chartSecondaryVerticalCrosshair());
                horizontal->setChecked(mainWindow_->chartSecondaryHorizontalCrosshair());
                gv->addWidget(vertical);
                gv->addWidget(horizontal);
                connect(vertical, &QCheckBox::toggled, this, [this, horizontal](bool on) {
                    mainWindow_->setChartSecondaryCrosshairs(on, horizontal->isChecked());
                });
                connect(horizontal, &QCheckBox::toggled, this, [this, vertical](bool on) {
                    mainWindow_->setChartSecondaryCrosshairs(vertical->isChecked(), on);
                });
            }

            QWidget* colW = new QWidget;
            colLay = new QVBoxLayout(colW);
            colLay->setContentsMargins(0, 0, 0, 0);
            colLay->setSpacing(8);
            gv->addWidget(colW);
            gv->addStretch(1);

            stack->addWidget(groupPage);
            lastGroup = r.group;
        }
        colLay->addWidget(makeControl(r.label, r.s));
    }

    connect(sidebar, &QListWidget::currentRowChanged,
            stack, &QStackedWidget::setCurrentIndex);
    sidebar->setCurrentRow(0);

    // Pinned to the bottom of the sidebar column. If any graph is already a
    // Table the click resets everything to Chart; otherwise it makes everything
    // a Table. The label shows the action the next click will perform.
    // Programmatic setChecked() doesn't emit idClicked, so re-checking the
    // segments here doesn't re-fire the per-control handlers — we push each
    // setting directly.
    QToolButton* setAllBtn = new QToolButton;
    setAllBtn->setAutoRaise(true);
    setAllBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    setAllBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto anyTable = [this, controls]() {
        for (const Ctl& c : controls)
            if (mainWindow_->graphView(c.s)) return true;
        return false;
    };
    auto refreshLabel = [anyTable, setAllBtn]() {
        setAllBtn->setText(anyTable() ? "Set Chart" : "Set Table");
    };
    refreshLabel();

    connect(setAllBtn, &QToolButton::clicked, this,
            [this, controls, anyTable, refreshLabel]() {
        const bool makeTable = !anyTable();   // all Chart → table; else → Chart
        for (const Ctl& c : controls) {
            mainWindow_->setGraphView(c.s, makeTable);
            c.group->button(makeTable ? 1 : 0)->setChecked(true);
        }
        refreshLabel();
    });
    // Keep the label current when individual graphs are changed directly.
    for (const Ctl& c : controls)
        connect(c.group, &QButtonGroup::idClicked, this, [refreshLabel](int) { refreshLabel(); });

    QWidget* sideCol = new QWidget;
    sideCol->setAutoFillBackground(true);
    sideCol->setBackgroundRole(QPalette::Button);
    QVBoxLayout* sideColLay = new QVBoxLayout(sideCol);
    sideColLay->setContentsMargins(0, 0, 0, 0);
    sideColLay->setSpacing(0);
    sideColLay->addWidget(sidebar, 1);
    QWidget* btnWrap = new QWidget;
    QVBoxLayout* btnWrapLay = new QVBoxLayout(btnWrap);
    btnWrapLay->setContentsMargins(8, 8, 8, 8);
    btnWrapLay->addWidget(setAllBtn);
    sideColLay->addWidget(btnWrap);

    h->addWidget(sideCol);
    h->addWidget(verticalSeparator());
    h->addWidget(stack, 1);
    return page;
}

QWidget* SettingsDialog::buildYAxisPage() {
    // Electron keeps Fixed/Dynamic behavior in its own category instead of
    // nesting it underneath the Chart/Table choice. Mirror that separation so
    // changing a graph's presentation never looks coupled to its axis policy.
    QWidget* page = new QWidget;
    auto* outer = new QHBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    struct YCtl { tnr::GraphSection section; QButtonGroup* group; };
    QList<YCtl> controls;

    auto makeControl = [this, &controls](const char* label,
                                         tnr::GraphSection section) -> QWidget* {
        QWidget* row = new QWidget;
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(12);
        layout->addWidget(new QLabel(label));
        layout->addStretch(1);

        auto* group = new QButtonGroup(row);
        group->setExclusive(true);
        for (int i = 0; i < 2; ++i) {
            auto* button = new SegmentButton;
            button->setText(i == 0 ? "Fixed" : "Dynamic");
            button->setCheckable(true);
            button->setAutoRaise(true);
            group->addButton(button, i);
            layout->addWidget(button);
        }
        group->button(mainWindow_->chartDynamicYAxis(section) ? 1 : 0)->setChecked(true);
        connect(group, &QButtonGroup::idClicked, this,
                [this, section](int id) {
                    mainWindow_->setChartDynamicYAxis(section, id == 1);
                });
        controls.push_back({ section, group });
        return row;
    };

    struct Row { tnr::GraphSection section; const char* group; const char* label; };
    static const Row rows[] = {
        { tnr::GraphSection::OverviewTyreSurface, "Overview", "Tyre surface temp" },
        { tnr::GraphSection::OverviewTyreInner,   "Overview", "Tyre inner temp" },
        { tnr::GraphSection::OverviewTyreBrake,   "Overview", "Tyre brake temp" },
        { tnr::GraphSection::OverviewTyreWear,    "Overview", "Tyre wear / life" },
        { tnr::GraphSection::TyreSurface,         "Tyres",    "Surface temp" },
        { tnr::GraphSection::TyreInner,           "Tyres",    "Inner temp" },
        { tnr::GraphSection::TyreBrake,           "Tyres",    "Brake temp" },
        { tnr::GraphSection::TyreWear,            "Tyres",    "Wear / life" },
        { tnr::GraphSection::PowerHarvest,        "Power",    "ERS harvest" },
    };

    auto* sidebar = new QListWidget;
    sidebar->setFrameShape(QFrame::NoFrame);
    sidebar->setMinimumWidth(130);
    sidebar->setMaximumWidth(160);
    sidebar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    const QString sidebarBg = QApplication::palette().color(QPalette::Button).name();
    sidebar->setStyleSheet(QString(
        "QListWidget { background: %1; border: none; outline: none; }"
        "QListWidget::item { padding: 8px 14px; }"
    ).arg(sidebarBg));

    auto* stack = new QStackedWidget;
    QString lastGroup;
    QVBoxLayout* groupLayout = nullptr;
    for (const Row& row : rows) {
        if (row.group != lastGroup) {
            sidebar->addItem(row.group);
            QWidget* groupPage = new QWidget;
            auto* pageLayout = new QVBoxLayout(groupPage);
            pageLayout->setContentsMargins(16, 12, 16, 8);
            pageLayout->setSpacing(10);
            pageLayout->addWidget(subHeading(row.group));
            QWidget* groupBody = new QWidget;
            groupLayout = new QVBoxLayout(groupBody);
            groupLayout->setContentsMargins(0, 0, 0, 0);
            groupLayout->setSpacing(8);
            pageLayout->addWidget(groupBody);
            pageLayout->addStretch(1);
            stack->addWidget(groupPage);
            lastGroup = row.group;
        }
        groupLayout->addWidget(makeControl(row.label, row.section));
    }

    connect(sidebar, &QListWidget::currentRowChanged,
            stack, &QStackedWidget::setCurrentIndex);
    sidebar->setCurrentRow(0);

    auto* setAllButton = new QToolButton;
    setAllButton->setAutoRaise(true);
    setAllButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    setAllButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto anyDynamic = [this, controls] {
        for (const YCtl& control : controls)
            if (mainWindow_->chartDynamicYAxis(control.section)) return true;
        return false;
    };
    auto refreshLabel = [anyDynamic, setAllButton] {
        setAllButton->setText(anyDynamic() ? "Set All Fixed" : "Set All Dynamic");
    };
    refreshLabel();
    connect(setAllButton, &QToolButton::clicked, this,
            [this, controls, anyDynamic, refreshLabel] {
                const bool makeDynamic = !anyDynamic();
                for (const YCtl& control : controls) {
                    mainWindow_->setChartDynamicYAxis(control.section, makeDynamic);
                    control.group->button(makeDynamic ? 1 : 0)->setChecked(true);
                }
                refreshLabel();
            });
    for (const YCtl& control : controls)
        connect(control.group, &QButtonGroup::idClicked, this,
                [refreshLabel](int) { refreshLabel(); });

    QWidget* sideColumn = new QWidget;
    sideColumn->setAutoFillBackground(true);
    sideColumn->setBackgroundRole(QPalette::Button);
    auto* sideLayout = new QVBoxLayout(sideColumn);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(0);
    sideLayout->addWidget(sidebar, 1);
    QWidget* buttonWrap = new QWidget;
    auto* buttonLayout = new QVBoxLayout(buttonWrap);
    buttonLayout->setContentsMargins(8, 8, 8, 8);
    buttonLayout->addWidget(setAllButton);
    sideLayout->addWidget(buttonWrap);

    outer->addWidget(sideColumn);
    outer->addWidget(verticalSeparator());
    outer->addWidget(stack, 1);
    return page;
}

QWidget* SettingsDialog::buildNotificationsPage() {
    QFormLayout* form;
    QWidget* page = makePage(form);

    toastsCheck_ = new QCheckBox("Show event toasts (penalties, flags, fastest lap…)");
    toastsCheck_->setChecked(mainWindow_->toastsEnabled());
    form->addRow(QString(), toastsCheck_);

    toastDurationCombo_ = new QComboBox;
    for (int s : { 2, 3, 5, 8, 10 })
        toastDurationCombo_->addItem(QString("%1s").arg(s), s);
    const int cur = toastDurationCombo_->findData(mainWindow_->toastDurationSecs());
    toastDurationCombo_->setCurrentIndex(cur >= 0 ? cur : 1);   // default 3s
    toastDurationCombo_->setEnabled(toastsCheck_->isChecked());
    form->addRow("Toast duration:", toastDurationCombo_);

    connect(toastsCheck_, &QCheckBox::toggled, this, [this](bool on) {
        mainWindow_->setToastsEnabled(on);
        toastDurationCombo_->setEnabled(on);
    });
    connect(toastDurationCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        mainWindow_->setToastDurationSecs(toastDurationCombo_->currentData().toInt());
    });
    return page;
}

QWidget* SettingsDialog::buildOverviewPage() {
    QFormLayout* form;
    QWidget* page = makePage(form);

    tyreViewCombo_ = new QComboBox;
    tyreViewCombo_->addItem("Cards",  (int)OverviewLayout::TyreCards);
    tyreViewCombo_->addItem("Charts", (int)OverviewLayout::TyreCharts);
    tyreViewCombo_->setCurrentIndex(
        mainWindow_->currentTyreView() == OverviewLayout::TyreCharts ? 1 : 0);
    connect(tyreViewCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        mainWindow_->setTyreView(
            tyreViewCombo_->currentData().toInt() == (int)OverviewLayout::TyreCharts
                ? OverviewLayout::TyreCharts : OverviewLayout::TyreCards);
    });
    form->addRow("Tyre view:", tyreViewCombo_);

    // Whether the tyre graph shows remaining life (100 - wear) or accumulated wear.
    tyreWearModeCombo_ = new QComboBox;
    tyreWearModeCombo_->addItem("Tyre Life", true);
    tyreWearModeCombo_->addItem("Tyre Wear", false);
    tyreWearModeCombo_->setCurrentIndex(mainWindow_->tyreGraphLifeMode() ? 0 : 1);
    connect(tyreWearModeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        mainWindow_->setTyreGraphLifeMode(tyreWearModeCombo_->currentData().toBool());
    });
    form->addRow("Tyre wear graph:", tyreWearModeCombo_);
    return page;
}

QWidget* SettingsDialog::buildTrackMapPage() {
    QFormLayout* form;
    QWidget* page = makePage(form);

    trackMapLabelsCombo_ = new QComboBox;
    trackMapLabelsCombo_->addItem("Dots & Labels", 0);
    trackMapLabelsCombo_->addItem("Dots Only", 1);
    trackMapLabelsCombo_->addItem("Labels Only", 2);
    trackMapLabelsCombo_->setCurrentIndex(mainWindow_->trackMapLabelMode());

    connect(trackMapLabelsCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        mainWindow_->setTrackMapLabelMode(trackMapLabelsCombo_->currentData().toInt());
    });
    form->addRow("Labels:", trackMapLabelsCombo_);

    // Hide drivers idle for longer than the selected duration (0 = never).
    trackMapIdleCombo_ = new QComboBox;
    trackMapIdleCombo_->addItem("Off", 0);
    for (int s : { 3, 5, 10, 15, 30 })
        trackMapIdleCombo_->addItem(QString("%1s").arg(s), s);
    const int idleIdx = trackMapIdleCombo_->findData(mainWindow_->trackMapIdleTimeout());
    trackMapIdleCombo_->setCurrentIndex(idleIdx >= 0 ? idleIdx : 0);
    connect(trackMapIdleCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        mainWindow_->setTrackMapIdleTimeout(trackMapIdleCombo_->currentData().toInt());
    });
    form->addRow("Hide static drivers:", trackMapIdleCombo_);

    // Per-sector colours vs plain white/black lines.
    trackMapSectorColorsCheck_ = new QCheckBox;
    trackMapSectorColorsCheck_->setChecked(mainWindow_->trackMapSectorColors());
    connect(trackMapSectorColorsCheck_, &QCheckBox::toggled, this, [this](bool on) {
        mainWindow_->setTrackMapSectorColors(on);
    });
    form->addRow("Sector colors:", trackMapSectorColorsCheck_);

    // Track-outline opacity (20–100%); driver dots/labels stay full strength.
    trackMapOpacitySlider_ = new QSlider(Qt::Horizontal);
    trackMapOpacitySlider_->setRange(20, 100);
    trackMapOpacitySlider_->setValue(mainWindow_->trackMapOpacity());
    connect(trackMapOpacitySlider_, &QSlider::valueChanged, this, [this](int v) {
        mainWindow_->setTrackMapOpacity(v);
    });
    form->addRow("Map opacity:", trackMapOpacitySlider_);
    return page;
}

// About: app identity + version, the project license, and attribution for every
// bundled third-party library. Each library exposes its full license text via a
// "View" button (showLicenseText), satisfying the GPL/LGPL notice requirements.
QWidget* SettingsDialog::buildAboutPage() {
    QWidget* page = new QWidget;
    QVBoxLayout* v = new QVBoxLayout(page);
    v->setContentsMargins(8, 12, 8, 8);
    v->setSpacing(8);

    // ── App identity ─────────────────────────────────────────────
    QLabel* name = new QLabel(QApplication::applicationName());
    QFont nameFont = name->font();
    nameFont.setBold(true);
    nameFont.setPointSizeF(nameFont.pointSizeF() + 3.0);
    name->setFont(nameFont);
    v->addWidget(name);

    v->addWidget(new QLabel("Version " + QApplication::applicationVersion()));

    QLabel* desc = new QLabel(
        "Background telemetry recorder and live session viewer for F1 sim racing.");
    desc->setWordWrap(true);
    v->addWidget(desc);

    QLabel* copyright = new QLabel("© 2026 Track N Race");
    v->addWidget(copyright);

    QLabel* repoLink = new QLabel(
        "<a href=\"https://github.com/nogoat/track-n-race\">github.com/nogoat/track-n-race</a>");
    repoLink->setOpenExternalLinks(true);
    v->addWidget(repoLink);

    // Project license row.
    QWidget* licRow = new QWidget;
    QHBoxLayout* licLay = new QHBoxLayout(licRow);
    licLay->setContentsMargins(0, 0, 0, 0);
    licLay->addWidget(new QLabel("Licensed under the GNU General Public License v3."));
    QPushButton* viewProjectLic = new QPushButton("View license");
    connect(viewProjectLic, &QPushButton::clicked, this, [this] {
        showLicenseText("GNU General Public License v3", ":/licenses/GPL-3.0.txt");
    });
    licLay->addWidget(viewProjectLic);
    licLay->addStretch(1);
    v->addWidget(licRow);

    v->addWidget(horizontalSeparator());
    v->addWidget(subHeading("Third-party software"));

    // name, version, license label, copyright holder, homepage URL, short link text, license resource.
    struct Lib { const char* name; QString version; const char* license; const char* copyright;
                 const char* url; const char* linkText; const char* resource; };
    const Lib libs[] = {
        { "Qt",            qVersion(), "LGPL v3", "© The Qt Company Ltd.",                       "https://www.qt.io",                "qt.io",           ":/licenses/LGPL-3.0.txt" },
        { "glaze",         "7.8.3",    "MIT",     "© 2019–present Stephen Berry",           "https://github.com/stephenberry/glaze", "github.com", ":/licenses/MIT-glaze.txt" },
        // The toast notifications are a fork of niklashenning/qt-toast (see the
        // license text, which notes the fork and reproduces the upstream notice).
        { "qt-toast (fork)", "—",      "MIT",     "© 2024 Niklas Henning",                  "https://github.com/niklashenning/qt-toast", "github.com", ":/licenses/MIT-qt-toast.txt" },
        { "zlib",          "1.3.2",    "zlib",    "© 1995–2026 Jean-loup Gailly & Mark Adler", "https://zlib.net",              "zlib.net",        ":/licenses/Zlib.txt"     },
        { "Zstandard",     "1.5.7",    "BSD 3-Clause", "© Meta Platforms, Inc. and affiliates", "https://facebook.github.io/zstd/", "facebook.github.io", ":/licenses/BSD-3-Clause-Zstandard.txt" },
        // libxlsxwriter powers the "Export to Excel" action; linked in every build.
        { "libxlsxwriter", "1.2.4",    "BSD 2-Clause", "© 2014–2026 John McNamara",         "https://libxlsxwriter.github.io", "libxlsxwriter.github.io", ":/licenses/BSD-2-Clause-libxlsxwriter.txt" },
        // Noto Sans is bundled (fonts.qrc) in every build as the Breeze UI font, so
        // it's credited here unconditionally — not under BREEZE_BUNDLED.
        { "Noto Sans",     "—",        "OFL 1.1", "© The Noto Project Authors",             "https://fonts.google.com/noto/specimen/Noto+Sans", "fonts.google.com", ":/licenses/OFL-1.1-Noto.txt" },
#ifdef BREEZE_BUNDLED
        // KDE is bundled only in --with-breeze builds (BREEZE_BUNDLED). "KDE
        // Frameworks" covers every KF6 module in the breeze6 runtime closure
        // (CoreAddons, Config, GuiAddons, ColorScheme, WindowSystem, IconThemes,
        // Archive, Codecs, ConfigWidgets, I18n, WidgetsAddons) — one row, not
        // eleven, because KDE's Frameworks Licensing Policy requires every module
        // to use the same terms (LGPL 2.1, or 3, or a later KDE e.V.-approved
        // version), so it's genuinely one license, matching KDE apps' own
        // convention (e.g. Kate's About dialog). All ship at 6.27.0.
        //
        // Breeze style (Plasma) is a separate row: it tracks Plasma's own release
        // (6.7.0, not KF6_VERSION) and — unlike the rest of this list — the
        // widget-style plugin itself (kstyle/breezestyle.cpp) is GPL-2.0-or-later,
        // not LGPL; verified against its SPDX header and the GPL-2.0-or-later.txt
        // shipped in the breeze repo's LICENSES/ dir.
        //
        // Breeze Icons is also separate: KDE's policy requires icon files
        // specifically to be LGPL-3.0 (not 2.1-or-later like the rest of
        // Frameworks), and the shipped icon set is a mix of LGPL-2.x/3.0/
        // CC-BY-SA-4.0 from legacy contributions — distinct enough from the code
        // frameworks' licensing to warrant its own line and its own license text.
        { "Breeze style (Plasma)", "6.7.0",  "GPL v2+",    "© 2014 Hugo Pereira Da Costa, The Qt Company Ltd., and KDE contributors", "https://invent.kde.org/plasma/breeze", "invent.kde.org", ":/licenses/GPL-2.0.txt" },
        { "Breeze Icons",          "6.27.0", "LGPL v3",    "© KDE contributors",                         "https://invent.kde.org/frameworks/breeze-icons", "invent.kde.org", ":/licenses/LGPL-3.0.txt" },
        { "KDE Frameworks",        "6.27.0", "LGPL v2.1+", "© KDE contributors",                         "https://develop.kde.org/products/frameworks/",   "develop.kde.org", ":/licenses/LGPL-2.1.txt" },
#endif
    };
    const int libCount = int(std::size(libs));

    // A table keeps the columns aligned and width under control; the long homepage
    // URLs (shown as short host links) were what forced the horizontal scrollbar.
    QTableWidget* table = new QTableWidget(libCount, 6);
    table->setHorizontalHeaderLabels({ "Library", "Version", "License", "Copyright", "Website", "View License" });
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setFocusPolicy(Qt::NoFocus);
    table->setShowGrid(false);
    table->setAlternatingRowColors(true);   // match the app's other tables
    table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table->setMinimumWidth(860);             // drives the fixed dialog width

    QHeaderView* hdr = table->horizontalHeader();
    hdr->setSectionResizeMode(0, QHeaderView::Stretch);            // Library absorbs slack
    for (int c = 1; c < 6; ++c)
        hdr->setSectionResizeMode(c, QHeaderView::ResizeToContents);

    for (int row = 0; row < libCount; ++row) {
        const Lib& lib = libs[row];

        QTableWidgetItem* nameItem = new QTableWidgetItem(lib.name);
        QFont nf = nameItem->font();
        nf.setBold(true);
        nameItem->setFont(nf);
        table->setItem(row, 0, nameItem);
        table->setItem(row, 1, new QTableWidgetItem(lib.version));
        table->setItem(row, 2, new QTableWidgetItem(lib.license));
        table->setItem(row, 3, new QTableWidgetItem(lib.copyright));

        QLabel* linkLbl = new QLabel(
            QString("<a href=\"%1\">%2</a>").arg(lib.url, lib.linkText));
        linkLbl->setOpenExternalLinks(true);
        linkLbl->setContentsMargins(4, 0, 24, 0);
        // Rich-text labels report a sizeHint a hair too narrow, so ResizeToContents
        // sizes the column just short of the widest link and clips its last glyph
        // Some proportional fonts clip the last glyph. Pin a plain-text-measured minimum
        // width (+ margins + a little slack) so the text always fits.
        linkLbl->setMinimumWidth(
            linkLbl->fontMetrics().horizontalAdvance(lib.linkText) + 4 + 24 + 6);
        table->setCellWidget(row, 4, linkLbl);

        QToolButton* viewBtn = new QToolButton;
        viewBtn->setAutoRaise(true);
        viewBtn->setCursor(Qt::PointingHandCursor);
        viewBtn->setToolTip("View license");
        viewBtn->setIcon(adaptThemeIcon(
            QIcon::fromTheme("quickview-symbolic"),
            table->palette().color(QPalette::WindowText),
            style()->standardIcon(QStyle::SP_FileDialogContentsView)));
        const QString title    = QString("%1 — %2").arg(lib.name, lib.license);
        const QString resource = lib.resource;
        connect(viewBtn, &QToolButton::clicked, this, [this, title, resource] {
            showLicenseText(title, resource);
        });
        // Centre the icon button in its cell so the alternating row tint shows
        // around it instead of a full-width button.
        QWidget* viewCell = new QWidget;
        QHBoxLayout* viewLay = new QHBoxLayout(viewCell);
        viewLay->setContentsMargins(0, 0, 0, 0);
        viewLay->addStretch(1);
        viewLay->addWidget(viewBtn);
        viewLay->addStretch(1);
        table->setCellWidget(row, 5, viewCell);
    }

    table->resizeRowsToContents();
    // QLabel cell-widgets and plain QTableWidgetItems report slightly different
    // sizeHints, so resizeRowsToContents() can give rows unequal heights. Clamp
    // everything to the tallest row so the grid looks uniform.
    int rowH = 0;
    for (int r = 0; r < libCount; ++r)
        rowH = qMax(rowH, table->rowHeight(r));
    for (int r = 0; r < libCount; ++r)
        table->setRowHeight(r, rowH);
    // Cap the visible height to ~9 rows; the rest (the KDE closure) scrolls.
    const int visibleRows = qMin(libCount, 9);
    int tableHeight = table->horizontalHeader()->height() + 2 * table->frameWidth()
                      + visibleRows * rowH;
    table->setFixedHeight(tableHeight);
    v->addWidget(table);

    v->addStretch(1);   // keep content top-aligned in the taller About modal
    return page;
}

// About lives in its own modal (opened from the Settings footer's About button)
// rather than a tab, so it can be wider/taller than the cramped settings tabs.
void SettingsDialog::showAboutDialog() {
    QDialog dlg(this);
    dlg.setWindowTitle("About " + QApplication::applicationName());
    // Same recipe as the Settings dialog itself: a fixed-size, plain modal frame
    // (no resize handles / maximize button) sized to its content.
    dlg.setWindowFlags(Qt::Dialog);
    dlg.setWindowModality(Qt::ApplicationModal);

    QVBoxLayout* lay = new QVBoxLayout(&dlg);
    lay->setSizeConstraint(QLayout::SetFixedSize);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(buildAboutPage(), 1);

    lay->addWidget(horizontalSeparator());
    QHBoxLayout* bottom = new QHBoxLayout;
    bottom->setContentsMargins(12, 8, 12, 12);
    bottom->addStretch(1);
    QPushButton* closeBtn = new QPushButton("Close");
    closeBtn->setDefault(true);
    closeBtn->setIcon(adaptThemeIcon(
        QIcon::fromTheme("dialog-close-symbolic"),
        dlg.palette().color(QPalette::WindowText),
        style()->standardIcon(QStyle::SP_DialogCloseButton)));
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    bottom->addWidget(closeBtn);
    lay->addLayout(bottom);

    dlg.exec();
}

void SettingsDialog::showLicenseText(const QString& title, const QString& resourcePath) {
    QDialog dlg(this);
    dlg.setWindowTitle(title);
    // Plain, fixed-size modal frame, same as the Settings/About modals (the text
    // browser's own scrollbar handles long license texts).
    dlg.setWindowFlags(Qt::Dialog);
    dlg.setWindowModality(Qt::ApplicationModal);
    dlg.setFixedSize(660, 560);

    QVBoxLayout* lay = new QVBoxLayout(&dlg);

    QTextBrowser* browser = new QTextBrowser;
    browser->setOpenExternalLinks(true);
    browser->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    QFile f(resourcePath);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&f);
        browser->setPlainText(in.readAll());
    } else {
        browser->setPlainText("License text could not be loaded (" + resourcePath + ").");
    }
    lay->addWidget(browser);

    QWidget* btnRow = new QWidget;
    QHBoxLayout* btnLay = new QHBoxLayout(btnRow);
    btnLay->setContentsMargins(0, 0, 0, 0);

    QPushButton* copyBtn = new QPushButton("Copy");
    copyBtn->setIcon(adaptThemeIcon(
        QIcon::fromTheme("edit-copy-symbolic"),
        dlg.palette().color(QPalette::WindowText),
        style()->standardIcon(QStyle::SP_FileDialogContentsView)));
    connect(copyBtn, &QPushButton::clicked, browser, [browser] {
        QApplication::clipboard()->setText(browser->toPlainText());
    });

    QPushButton* closeBtn = new QPushButton("Close");
    closeBtn->setDefault(true);
    closeBtn->setIcon(adaptThemeIcon(
        QIcon::fromTheme("dialog-close-symbolic"),
        dlg.palette().color(QPalette::WindowText),
        style()->standardIcon(QStyle::SP_DialogCloseButton)));
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    btnLay->addWidget(copyBtn);
    btnLay->addStretch(1);
    btnLay->addWidget(closeBtn);
    lay->addWidget(btnRow);

    dlg.exec();
}
