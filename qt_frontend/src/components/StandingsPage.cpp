#include "StandingsPage.h"
#include "../Labels.h"
#include "PageUiHelpers.h"
#include "TyreHelpers.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QBrush>
#include <QLabel>
#include <QFont>
#include <QPalette>
#include <QSizePolicy>
#include <QTableWidget>
#include <QHeaderView>
#include <QTableWidgetItem>
#include <QGroupBox>
#include <QScrollArea>
#include <QProgressBar>
#include <QStringList>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QEvent>
#include <QVariant>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QSplitter>
#include <QResizeEvent>
#include <QTimer>
#include <QApplication>
#include <QDateTime>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

#include <cmath>

// ── Standings helpers ─────────────────────────────────────────────────────

namespace {

float relativeLuminance(const QColor& c) {
    auto toLinear = [](float v) {
        return v <= 0.03928f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    };
    float r = toLinear(c.redF());
    float g = toLinear(c.greenF());
    float b = toLinear(c.blueF());
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

float contrastRatio(const QColor& c1, const QColor& c2) {
    float l1 = relativeLuminance(c1);
    float l2 = relativeLuminance(c2);
    if (l1 < l2) std::swap(l1, l2);
    return (l1 + 0.05f) / (l2 + 0.05f);
}

QColor ensureContrast(const QColor& fg, const QColor& bg, const QColor& fallback, float threshold) {
    if (!fg.isValid()) return fallback;
    if (contrastRatio(fg, bg) < threshold) return fallback;
    return fg;
}

void setLabelText(QLabel* label, const QString& text) {
    if (label && label->text() != text) label->setText(text);
}

void setLabelStyle(QLabel* label, const QString& style) {
    if (label && label->styleSheet() != style) label->setStyleSheet(style);
}

QString formatLapTime(int ms) {
    if (ms <= 0) return "—";
    int min  = ms / 60000;
    int sec  = (ms % 60000) / 1000;
    int msec = ms % 1000;
    return QString("%1:%2.%3")
        .arg(min).arg(sec, 2, 10, QChar('0')).arg(msec, 3, 10, QChar('0'));
}

QString formatSector(int ms) {
    if (ms <= 0) return "—";
    int sec  = ms / 1000;
    int msec = ms % 1000;
    return QString("%1.%2").arg(sec).arg(msec, 3, 10, QChar('0'));
}

// The tower's empty last lap reads "--:--.---" (Electron); the race panel keeps "—".
QString formatTowerLapTime(int ms) {
    return ms <= 0 ? QStringLiteral("--:--.---") : formatLapTime(ms);
}

// Electron's tower badges: bordered chips with a 10% tint of their colour.
// The cell carries a QVariantList of {text, color} maps in BadgeRole.
constexpr int BadgeRole = Qt::UserRole + 1;

QVariantMap badge(const QString& text, const QColor& color) {
    return QVariantMap{ { QStringLiteral("text"), text }, { QStringLiteral("color"), color } };
}

class BadgeDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        painter->save();
        if (opt.backgroundBrush.style() != Qt::NoBrush) painter->fillRect(opt.rect, opt.backgroundBrush);
        QFont font = opt.font;
        font.setPointSize(std::max(1, font.pointSize() - 2));
        font.setBold(true);
        painter->setFont(font);
        const QFontMetrics fm(font);
        const int h = fm.height() + 2;
        int x = opt.rect.left() + 4;
        const int y = opt.rect.top() + (opt.rect.height() - h) / 2;
        painter->setRenderHint(QPainter::Antialiasing);
        for (const QVariant& v : index.data(BadgeRole).toList()) {
            const QVariantMap m = v.toMap();
            const QString text = m.value(QStringLiteral("text")).toString();
            const QColor color = m.value(QStringLiteral("color")).value<QColor>();
            const int w = fm.horizontalAdvance(text) + 10;
            if (x + w > opt.rect.right()) break;
            const QRectF r(x + .5, y + .5, w - 1, h - 1);
            QColor fill = color; fill.setAlphaF(.1);
            painter->setPen(QPen(color, 1));
            painter->setBrush(fill);
            painter->drawRoundedRect(r, 3, 3);
            painter->setPen(color);
            painter->drawText(r, Qt::AlignCenter, text);
            x += w + 4;
        }
        painter->restore();
    }
};

QString formatGap(int ms, bool isLeader) {
    if (isLeader) return "LEADER";
    if (ms <= 0)  return "—";
    if (ms < 60000)
        return QString("+%1.%2").arg(ms / 1000).arg(ms % 1000, 3, 10, QChar('0'));
    int min  = ms / 60000;
    int sec  = (ms % 60000) / 1000;
    int msec = ms % 1000;
    return QString("+%1:%2.%3")
        .arg(min).arg(sec, 2, 10, QChar('0')).arg(msec, 3, 10, QChar('0'));
}

class DriverDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        painter->save();

        if (opt.backgroundBrush.style() != Qt::NoBrush) {
            painter->fillRect(opt.rect, opt.backgroundBrush);
        }

        bool isPlayer = index.data(Qt::UserRole).toBool();
        QString text = index.data(Qt::DisplayRole).toString();

        QRect textRect = opt.rect.adjusted(4, 0, -4, 0);

        painter->setFont(opt.font);
        painter->setPen(opt.palette.color(QPalette::Text));

        if (!isPlayer) {
            painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, text);
        } else {
            QFontMetrics fm(opt.font);
            int nameWidth = fm.horizontalAdvance(text);

            painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, text);

            QString chipText = "YOU";
            QFont chipFont = opt.font;
            chipFont.setPointSize(std::max(1, chipFont.pointSize() - 2));
            chipFont.setBold(true);
            QFontMetrics chipFm(chipFont);

            int chipWidth = chipFm.horizontalAdvance(chipText) + 8;
            int chipHeight = chipFm.height() + 2;

            int chipX = textRect.left() + nameWidth + 8;
            int chipY = textRect.top() + (textRect.height() - chipHeight) / 2;

            QRect chipRect(chipX, chipY, chipWidth, chipHeight);

            painter->setPen(Qt::NoPen);
            QColor chipBg = opt.palette.color(QPalette::Highlight);
            chipBg.setAlpha(80);
            painter->setBrush(chipBg);
            painter->drawRoundedRect(chipRect, 4, 4);

            painter->setFont(chipFont);
            QColor chipFg = opt.palette.color(QPalette::Highlight).lighter(150);
            painter->setPen(chipFg);
            painter->drawText(chipRect, Qt::AlignCenter, chipText);
        }

        painter->restore();
    }
};

// Keeps the Driver column (col 2) stretching to fill spare width, but never below
// a readable floor. QHeaderView::Stretch shrinks a section without limit on narrow
// windows; instead we size the column on every viewport resize to
// max(MIN, available) so it grows with the table yet stays >= MIN, letting the
// table scroll horizontally rather than crushing the names.
class DriverColumnSizer : public QObject {
public:
    static constexpr int kMinWidth = 150;
    static constexpr int kCol      = 2;
    explicit DriverColumnSizer(QTableWidget* t) : QObject(t), t_(t) {}
    void apply() {
        int others = 0;
        for (int c = 0; c < t_->columnCount(); ++c)
            if (c != kCol) others += t_->columnWidth(c);
        t_->setColumnWidth(kCol, std::max(kMinWidth, t_->viewport()->width() - others));
    }
protected:
    bool eventFilter(QObject* o, QEvent* e) override {
        if (e->type() == QEvent::Resize) apply();
        return QObject::eventFilter(o, e);
    }
private:
    QTableWidget* t_;
};

} // namespace

// ── Standings page builder ────────────────────────────────────────────────

StandingsPage::StandingsPage(QWidget* parent)
    : QWidget(parent)
{
    tableDensity_ = tnr::densityFromValue(settings_.value(
        tnr::compactKey(tnr::CompactSection::StandingsTable), "normal"));
    const tnr::CompactSection cardSections[] = {
        tnr::CompactSection::StandingsTiming,
        tnr::CompactSection::StandingsErs,
        tnr::CompactSection::StandingsStrategy
    };
    for (int i = 0; i < 3; ++i)
        cardDensity_[i] = tnr::densityFromValue(settings_.value(
            tnr::compactKey(cardSections[i]), "normal"));

    QHBoxLayout* hbox = new QHBoxLayout(this);
    hbox->setContentsMargins(0, 0, 0, 0);
    hbox->setSpacing(0);

    timingTable_ = new QTableWidget;
    timingTable_->setColumnCount(12);
    timingTable_->setHorizontalHeaderLabels(
        {"POS", "#", "DRIVER", "LAP", "LAST LAP", "GAP", "S1", "S2", "S3", "TYRE", "PENALTIES", "STATUS"});
    timingTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    timingTable_->setSelectionMode(QAbstractItemView::NoSelection);
    timingTable_->setShowGrid(false);
    timingTable_->setAlternatingRowColors(true);
    // Pixel-based scrolling — the default ScrollPerItem snaps a whole row per
    // wheel notch / scrollbar step, which feels chunky and "laggy"; per-pixel is
    // smooth.
    timingTable_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    timingTable_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    timingTable_->verticalHeader()->setVisible(false);
    // Fixed/interactive widths — NOT ResizeToContents, which re-measures every
    // cell on every setItem and tanks the UI when the table rebuilds rapidly.
    timingTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    {
        const int colW[12] = { 44, 36, 150, 46, 84, 80, 60, 60, 60, 52, 110, 100 };
        for (int c = 0; c < 12; ++c)
            timingTable_->setColumnWidth(c, colW[c]);
    }
    // Driver column fills spare width but stays >= 150px (see DriverColumnSizer);
    // on narrow windows the table scrolls instead of crushing the names.
    {
        auto* sizer = new DriverColumnSizer(timingTable_);
        timingTable_->viewport()->installEventFilter(sizer);
        sizer->apply();
    }

    QFont hf; hf.setPointSize(tableDensity_ == tnr::DensityMode::Compact ? 7
                                : tableDensity_ == tnr::DensityMode::Spacious ? 9 : 8);
    timingTable_->horizontalHeader()->setFont(hf);
    timingTable_->horizontalHeader()->setFixedHeight(
        tableDensity_ == tnr::DensityMode::Compact ? 22
        : tableDensity_ == tnr::DensityMode::Spacious ? 34 : 28);

    timingTable_->setItemDelegateForColumn(2, new DriverDelegate(timingTable_));
    timingTable_->setItemDelegateForColumn(10, new BadgeDelegate(timingTable_));
    timingTable_->setItemDelegateForColumn(11, new BadgeDelegate(timingTable_));
    showPlaceholderRows();

    connect(timingTable_, &QTableWidget::cellClicked, this, [this](int row, int) {
        // A double-click's own clicks must not toggle the selection back off.
        if (lastDoubleClick_.isValid() &&
            lastDoubleClick_.elapsed() < QApplication::doubleClickInterval()) return;
        int clicked = (row >= 0 && row < (int)tableRowCarIdx_.size())
                      ? tableRowCarIdx_[row] : -1;
        selectedCarIdx_ = (clicked == selectedCarIdx_) ? -1 : clicked;
        emit refreshRequested();
    });
    connect(timingTable_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (row < 0 || row >= (int)tableRowCarIdx_.size() || tableRowCarIdx_[row] < 0) return;
        lastDoubleClick_.start();
        // The double-click leaves the row selected, whatever its first click did.
        const int car = tableRowCarIdx_[row];
        if (selectedCarIdx_ != car) {
            selectedCarIdx_ = car;
            emit refreshRequested();
        }
        emit driverLapsRequested(car);
    });

    hbox->addWidget(timingTable_, 1);

    hbox->addWidget(sidebarDivider_ = tnrui::vline());
    hbox->addWidget(sidebar_ = buildRacePanel());
    showTimingTower_ = settings_.value("standingsLayout/showTimingTower", true).toBool();
    sidebarPct_ = std::clamp(settings_.value("standingsLayout/sidebarPct", 28).toInt(), 15, 60);
    const char* keys[] = {"timing", "energyRecovery", "strategy"};
    for (int i = 0; i < 3; ++i)
        showCards_[i] = settings_.value(QStringLiteral("standingsLayout/cards/") + keys[i], true).toBool();
    applyLayout();
}

// ── Race panel builder ────────────────────────────────────────────────────

void StandingsPage::updateSidebarWidth() {
    if (!sidebar_) return;
    const bool anyCard = showCards_[0] || showCards_[1] || showCards_[2];
    if (showTimingTower_ && anyCard) {
        sidebar_->setFixedWidth(qRound(contentsRect().width() * sidebarPct_ / 100.0));
    } else {
        sidebar_->setMinimumWidth(0);
        sidebar_->setMaximumWidth(QWIDGETSIZE_MAX);
    }
}

void StandingsPage::applyLayout() {
    const bool anyCard = showCards_[0] || showCards_[1] || showCards_[2];
    timingTable_->setVisible(showTimingTower_);
    sidebar_->setVisible(anyCard);
    sidebarDivider_->setVisible(showTimingTower_ && anyCard);
    for (int i = 0; i < 3; ++i) cards_[i]->setVisible(showCards_[i]);
    cardDividers_[0]->setVisible(showCards_[0] && showCards_[1]);
    cardDividers_[1]->setVisible(showCards_[2] && (showCards_[0] || showCards_[1]));
    updateSidebarWidth();
}

void StandingsPage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateSidebarWidth();
}

void StandingsPage::showLayoutEditor() {
    auto* dialog = new QDialog(this);
    dialog->setWindowTitle(QStringLiteral("Edit Standings Layout"));
    dialog->setWindowModality(Qt::ApplicationModal);
    auto* root = new QVBoxLayout(dialog);
    root->addWidget(new QLabel(QStringLiteral("Drag the divider to resize the sidebar (15–60%).")));
    auto* preview = new QSplitter(Qt::Horizontal);
    preview->setChildrenCollapsible(false);
    preview->setMinimumHeight(256);
    preview->setHandleWidth(8);
    auto* tower = new QPushButton(QStringLiteral("Timing Tower"));
    tower->setCheckable(true);
    tower->setChecked(showTimingTower_);
    tower->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    preview->addWidget(tower);
    auto* cardColumn = new QWidget;
    auto* cardLayout = new QVBoxLayout(cardColumn);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    const QStringList labels{QStringLiteral("Timing"), QStringLiteral("Energy Recovery"),
                             QStringLiteral("Strategy")};
    const char* keys[] = {"timing", "energyRecovery", "strategy"};
    for (int i = 0; i < 3; ++i) {
        auto* card = new QPushButton(labels[i]);
        card->setCheckable(true);
        card->setChecked(showCards_[i]);
        card->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
        cardLayout->addWidget(card, 1);
        const QString key = QStringLiteral("standingsLayout/cards/") + keys[i];
        connect(card, &QPushButton::toggled, this, [this, i, key](bool on) {
            showCards_[i] = on;
            settings_.setValue(key, on);
            applyLayout();
        });
    }
    preview->addWidget(cardColumn);
    root->addWidget(preview, 1);
    auto* widthLabel = new QLabel(QStringLiteral("Sidebar: %1%").arg(sidebarPct_));
    root->addWidget(widthLabel);
    connect(tower, &QPushButton::toggled, this, [this](bool on) {
        showTimingTower_ = on;
        settings_.setValue("standingsLayout/showTimingTower", on);
        applyLayout();
    });
    connect(preview, &QSplitter::splitterMoved, dialog, [this, preview, widthLabel](int, int) {
        const QList<int> sizes = preview->sizes();
        const int total = sizes[0] + sizes[1];
        if (total <= 0) return;
        sidebarPct_ = std::clamp(qRound(sizes[1] * 100.0 / total), 15, 60);
        const int right = qRound(total * sidebarPct_ / 100.0);
        preview->setSizes({total - right, right});
        settings_.setValue("standingsLayout/sidebarPct", sidebarPct_);
        widthLabel->setText(QStringLiteral("Sidebar: %1%").arg(sidebarPct_));
        updateSidebarWidth();
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    root->addWidget(buttons);
    connect(dialog, &QDialog::finished, dialog, &QObject::deleteLater);
    dialog->resize(680, 350);
    dialog->show();
    QTimer::singleShot(0, dialog, [this, preview] {
        const int total = preview->width() - preview->handleWidth();
        const int right = qRound(total * sidebarPct_ / 100.0);
        preview->setSizes({total - right, right});
    });
}

QWidget* StandingsPage::buildRacePanel() {
    // A rebuild (density change) deletes the previous panel's labels. Forget
    // them all first, so a row the new layout omits — Harvested exists only at
    // Spacious — reads as absent (setLabelText skips null) instead of pointing
    // at a deleted label that the next telemetry update would write to.
    rp_driverName = rp_lapNum = rp_position = rp_pitStatus = nullptr;
    rp_currentLap = rp_lastLap = rp_s1 = rp_s2 = rp_s3 = nullptr;
    rp_ersBar = nullptr;
    rp_ersPct = rp_ersStore = rp_ersMode = rp_ersDeployed = rp_ersHarvested = nullptr;
    rp_drsLabel = rp_drs = nullptr;
    rp_fuelKg = rp_fuelLaps = rp_fuelMix = rp_tyre = rp_tyreAge = rp_brakeBias = nullptr;

    QScrollArea* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    QWidget* w = new QWidget;
    QVBoxLayout* vbox = new QVBoxLayout(w);
    vbox->setContentsMargins(0, 14, 0, 14);
    vbox->setSpacing(6);
    QVBoxLayout* sections = vbox;
    int currentCard = 0;
    auto beginCard = [&](int index) {
        currentCard = index;
        cards_[index] = new QWidget;
        sections->addWidget(cards_[index]);
        vbox = new QVBoxLayout(cards_[index]);
        const auto density = cardDensity_[index];
        vbox->setContentsMargins(0,
            density == tnr::DensityMode::Compact ? 0
            : density == tnr::DensityMode::Spacious ? 8 : 0,
            0,
            density == tnr::DensityMode::Compact ? 0
            : density == tnr::DensityMode::Spacious ? 8 : 0);
        vbox->setSpacing(density == tnr::DensityMode::Compact ? 2
                         : density == tnr::DensityMode::Spacious ? 10 : 6);
    };
    beginCard(0);

    // Helper: key / value row
    auto makeRow = [&](const QString& label, QLabel*& valueOut) -> QWidget* {
        const auto density = cardDensity_[currentCard];
        const bool compact = density == tnr::DensityMode::Compact;
        const bool spacious = density == tnr::DensityMode::Spacious;
        QWidget* row = new QWidget;
        QHBoxLayout* h = new QHBoxLayout(row);
        h->setContentsMargins(spacious ? 18 : 14, compact ? 1 : spacious ? 7 : 4,
                             spacious ? 18 : 14, compact ? 1 : spacious ? 7 : 4);
        QLabel* lbl = new QLabel(label);
        QFont lf; lf.setPointSize(compact ? 8 : spacious ? 11 : 9); lbl->setFont(lf);
        lbl->setForegroundRole(QPalette::PlaceholderText);
        valueOut = new QLabel("—");
        QFont vf; vf.setPointSize(compact ? 8 : spacious ? 11 : 9); vf.setBold(true); valueOut->setFont(vf);
        h->addWidget(lbl);
        h->addStretch();
        h->addWidget(valueOut);
        return row;
    };

    // Helper: flat section title (matches WheelCard corner label style)
    auto makeSection = [&](const QString& title) {
        vbox->addWidget(tnrui::makeSectionLabel(title));
    };

    auto addDivider = [&]() {
        vbox->addWidget(tnrui::hline());
    };

    // ── Driver header ────────────────────────────────────────────
    rp_driverName = new QLabel("—");
    QFont dnF; dnF.setPointSize(cardDensity_[0] == tnr::DensityMode::Compact ? 10
                                : cardDensity_[0] == tnr::DensityMode::Spacious ? 15 : 12); dnF.setBold(true);
    rp_driverName->setFont(dnF);
    rp_driverName->setAlignment(Qt::AlignCenter);
    rp_driverName->setContentsMargins(14, 0, 14, 0);
    vbox->addWidget(rp_driverName);

    addDivider();

    // ── TIMING ───────────────────────────────────────────────────
    makeSection("TIMING");
    vbox->addWidget(makeRow("Lap",      rp_lapNum));
    vbox->addWidget(makeRow("Position", rp_position));
    vbox->addWidget(makeRow("Pit",      rp_pitStatus));
    vbox->addWidget(makeRow("Current",  rp_currentLap));
    vbox->addWidget(makeRow("Last Lap", rp_lastLap));

    // S1 / S2 side by side
    QWidget* sectRow = new QWidget;
    QHBoxLayout* sh = new QHBoxLayout(sectRow);
    const bool timingCompact = cardDensity_[0] == tnr::DensityMode::Compact;
    const bool timingSpacious = cardDensity_[0] == tnr::DensityMode::Spacious;
    sh->setContentsMargins(timingSpacious ? 18 : 14, timingCompact ? 1 : 4,
                           timingSpacious ? 18 : 14, 0);
    sh->setSpacing(timingSpacious ? 12 : 8);
    auto makeSect = [&](const QString& lbl, QLabel*& out) {
        QWidget* sc = new QWidget;
        QVBoxLayout* sv = new QVBoxLayout(sc);
        sv->setContentsMargins(0, 0, 0, 0); sv->setSpacing(1);
        QLabel* l = new QLabel(lbl); QFont lf; lf.setPointSize(timingCompact ? 7 : timingSpacious ? 9 : 7); l->setFont(lf);
        l->setForegroundRole(QPalette::PlaceholderText); l->setAlignment(Qt::AlignCenter);
        out = new QLabel("—"); QFont vf; vf.setPointSize(timingCompact ? 8 : timingSpacious ? 11 : 8); vf.setBold(true);
        out->setFont(vf); out->setAlignment(Qt::AlignCenter);
        sv->addWidget(l); sv->addWidget(out);
        return sc;
    };
    sh->addWidget(makeSect("S1", rp_s1));
    sh->addWidget(makeSect("S2", rp_s2));
    sh->addWidget(makeSect("S3", rp_s3));
    vbox->addWidget(sectRow);
    rp_pitStatus->setTextFormat(Qt::RichText);   // yellow pit + red flags, as Electron

    sections->addWidget(cardDividers_[0] = tnrui::hline());

    // ── ENERGY ───────────────────────────────────────────────────
    beginCard(1);
    makeSection("ENERGY");

    rp_ersPct = new QLabel("—");
    QFont bigF; bigF.setPointSize(cardDensity_[1] == tnr::DensityMode::Compact ? 14
                                 : cardDensity_[1] == tnr::DensityMode::Spacious ? 24 : 18); bigF.setBold(true);
    rp_ersPct->setFont(bigF);
    rp_ersPct->setAlignment(Qt::AlignCenter);
    rp_ersPct->setContentsMargins(14, 0, 14, 0);
    vbox->addWidget(rp_ersPct);

    rp_ersBar = new QProgressBar;
    rp_ersBar->setRange(0, 100);
    rp_ersBar->setValue(0);
    rp_ersBar->setTextVisible(false);
    rp_ersBar->setFixedHeight(6);
    {
        QWidget* barWrap = new QWidget;
        QHBoxLayout* bh = new QHBoxLayout(barWrap);
        bh->setContentsMargins(14, 0, 14, 0);
        bh->addWidget(rp_ersBar);
        vbox->addWidget(barWrap);
    }

    {
        rp_ersStore = new QLabel("— MJ / 4.00 MJ");
        QFont sf; sf.setPointSize(7); rp_ersStore->setFont(sf);
        rp_ersStore->setForegroundRole(QPalette::PlaceholderText);
        rp_ersStore->setContentsMargins(14, 0, 14, 0);
        vbox->addWidget(rp_ersStore);
    }
    vbox->addWidget(makeRow("Mode", rp_ersMode));
    vbox->addWidget(makeRow("Deployed", rp_ersDeployed));
    if (cardDensity_[1] == tnr::DensityMode::Spacious)
        vbox->addWidget(makeRow("Harvested", rp_ersHarvested));
    {
        QWidget* drsRow = makeRow(tnr::L("drs.label"), rp_drs);
        rp_drsLabel = drsRow->findChild<QLabel*>();   // the caption is the row's first label
        vbox->addWidget(drsRow);
    }

    sections->addWidget(cardDividers_[1] = tnrui::hline());

    // ── STRATEGY ─────────────────────────────────────────────────
    beginCard(2);
    makeSection("STRATEGY");
    vbox->addWidget(makeRow("Fuel",       rp_fuelKg));
    vbox->addWidget(makeRow("Fuel Laps",  rp_fuelLaps));
    vbox->addWidget(makeRow("Mix",        rp_fuelMix));
    vbox->addWidget(makeRow("Tyre",       rp_tyre));
    vbox->addWidget(makeRow("Tyre Age",   rp_tyreAge));
    vbox->addWidget(makeRow("Brake Bias", rp_brakeBias));

    sections->addStretch();
    scroll->setWidget(w);
    return scroll;
}

// ── Fastest-lap tracking ──────────────────────────────────────────────────

void StandingsPage::noteFastestLap(int carIdx) {
    fastestLapCarIdx_ = carIdx;
    fastestLapSet_ = true;
}

bool StandingsPage::noteSessionHistoryFastest(int carIdx, int bestMs) {
    if (fastestLapSet_) return false;
    sessionHistoryBest_[carIdx] = bestMs;
    int minMs = std::numeric_limits<int>::max();
    int minIdx = -1;
    for (const auto& kv : sessionHistoryBest_) {
        if (kv.second < minMs) { minMs = kv.second; minIdx = kv.first; }
    }
    if (fastestLapCarIdx_ == minIdx) return false;
    fastestLapCarIdx_ = minIdx;
    return true;
}

void StandingsPage::resetForNewSession() {
    fastestLapCarIdx_ = -1;
    fastestLapSet_ = false;
    sessionHistoryBest_.clear();
    tableSectors_.clear();
    playerSectors_ = SectorTrack{};
}

void StandingsPage::trackSectors(SectorTrack& t, int lapNum, int sector, int s1, int s2,
                                 int lastLapMs, qint64 now) {
    // Capture S1+S2 when the car enters sector 3 so S3 can be derived at the line.
    if (sector == 2 && s1 > 0 && s2 > 0 && t.snapLap != lapNum) {
        t.snapLap = lapNum; t.snapS1 = s1; t.snapS2 = s2;
    }
    // Lap just completed: derive S3 and hold the previous lap's sectors for 7 s.
    if (t.seen && lapNum > t.lapNum) {
        const int s3 = (t.snapLap == t.lapNum && lastLapMs > 0)
            ? std::max(0, lastLapMs - t.snapS1 - t.snapS2) : 0;
        t.frozenS1 = t.s1; t.frozenS2 = t.s2; t.frozenS3 = s3;
        t.frozenUntilMs = now + 7000;
    }
    t.seen = true; t.lapNum = lapNum; t.s1 = s1; t.s2 = s2;
}

void StandingsPage::showPlaceholderRows() {
    tableRowCarIdx_.clear();
    rowSafeColors_.clear();
    timingTable_->setUpdatesEnabled(false);
    timingTable_->setRowCount(20);
    const QColor muted = timingTable_->palette().color(QPalette::PlaceholderText);
    for (int row = 0; row < 20; ++row) {
        for (int column = 0; column < timingTable_->columnCount(); ++column) {
            QString text = QStringLiteral("—");
            if (column == 0) text = QString("P%1").arg(row + 1);
            else if (column == 4) text = QStringLiteral("--:--.---");
            else if (column >= 10) text.clear();
            auto* item = new QTableWidgetItem(text);
            item->setTextAlignment(column == 2 ? Qt::AlignLeft | Qt::AlignVCenter : Qt::AlignCenter);
            item->setForeground(muted);
            timingTable_->setItem(row, column, item);
        }
    }
    timingTable_->setUpdatesEnabled(true);
}

void StandingsPage::selectDriver(int driverIndex) {
    if (selectedCarIdx_ == driverIndex) return;
    selectedCarIdx_ = driverIndex;
    emit refreshRequested();
}

// ── Race panel updater ────────────────────────────────────────────────────

void StandingsPage::updateRacePanel(const TimingRow* timing,
                                    const tnrp::ParticipantsRow* participants,
                                    const LapRow* playerLap,
                                    const StatusRow* playerStatus,
                                    const AllStatusRow* allStatus,
                                    bool playerDrsAvailable,
                                    const QSet<int>* allStatusDrsAvailable) {
    if (!rp_lapNum) return;

    int playerIdx    = timing ? timing->player_idx : -1;
    bool viewingOther = (selectedCarIdx_ != -1 && selectedCarIdx_ != playerIdx);

    {
        const int targetIdx = viewingOther ? selectedCarIdx_ : playerIdx;
        QString name;
        QColor liveryColor;
        if (targetIdx >= 0 && participants) {
            for (const tnrp::Driver& d : participants->drivers) {
                if (d.idx == targetIdx) {
                    name = QString("#%1 %2")
                        .arg(d.race_number)
                        .arg(QString::fromStdString(d.name));
                    liveryColor = QColor(QString::fromStdString(d.livery_color));
                    break;
                }
            }
        }
        setLabelText(rp_driverName,
                     name.isEmpty() ? (targetIdx >= 0 ? QString("Car %1").arg(targetIdx) : "—") : name);

        const QColor bg = rp_driverName->palette().color(QPalette::Window);
        if (liveryColor.isValid() && contrastRatio(liveryColor, bg) >= contrastThreshold())
            setLabelStyle(rp_driverName, QString("color: %1;").arg(liveryColor.name()));
        else
            setLabelStyle(rp_driverName, QString());
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (playerLap)
        trackSectors(playerSectors_, playerLap->lap_num, playerLap->sector, playerLap->s1_ms,
                     playerLap->s2_ms, playerLap->last_lap_ms, now);

    // Generic over LapRow and TimingCar — both carry the same timing field names.
    // `player` enables the 7 s finished-lap sector hold (Electron applies it to
    // the player's own lap row only; another car shows S1/S2 once reached).
    auto applyTiming = [&](const auto& lap, bool player) {
        int lapNum    = lap.lap_num;
        int pos       = lap.position;
        int pitSt     = lap.pit_status;
        int currentMs = lap.current_lap_ms;
        int lastMs    = lap.last_lap_ms;
        int s1Ms      = lap.s1_ms;
        int s2Ms      = lap.s2_ms;
        bool invalid  = lap.lap_invalid;
        int  penS     = lap.penalties_s;

        setLabelText(rp_lapNum, lapNum > 0 ? QString::number(lapNum) : "—");
        setLabelText(rp_position, pos > 0 ? "P" + QString::number(pos) : "—");

        // Pit state in the compound-medium yellow, invalid/penalty flags in red.
        QStringList flags;
        const auto flag = [](const QString& text, const QColor& color) {
            return QString("<span style='color:%1; font-weight:bold'>%2</span>").arg(color.name(), text);
        };
        if (pitSt == 1)      flags << flag("Pitting", tnr::compoundMediumColor());
        else if (pitSt == 2) flags << flag("In pit lane", tnr::compoundMediumColor());
        if (invalid)         flags << flag("INVALID", QColor("#C4162A"));
        if (penS > 0)        flags << flag("+" + QString::number(penS) + "s", QColor("#C4162A"));
        setLabelText(rp_pitStatus, flags.isEmpty() ? "—" : flags.join(" · "));

        setLabelText(rp_currentLap, formatLapTime(currentMs));
        setLabelText(rp_lastLap, formatLapTime(lastMs));
        int showS1 = 0, showS2 = 0, showS3 = 0;
        if (player && playerSectors_.frozen(now)) {
            showS1 = playerSectors_.frozenS1; showS2 = playerSectors_.frozenS2;
            showS3 = playerSectors_.frozenS3;
        } else {
            showS1 = lap.sector >= 1 ? s1Ms : 0;
            showS2 = lap.sector >= 2 ? s2Ms : 0;
        }
        setLabelText(rp_s1, formatSector(showS1));
        setLabelText(rp_s2, formatSector(showS2));
        setLabelText(rp_s3, formatSector(showS3));
    };

    if (viewingOther && timing) {
        for (const TimingCar& car : timing->cars) {
            if (car.idx == selectedCarIdx_) { applyTiming(car, false); break; }
        }
    } else if (playerLap) {
        applyTiming(*playerLap, true);
    }

    // Generic over StatusRow and AllStatusCar — same status field names.
    auto applyStatus = [&](const auto& st, bool drsAvailable) {
        float ersPct    = (float)st.ers_pct;
        int   ersMode   = st.ers_mode;
        float fuelKg    = (float)st.fuel_kg;
        float fuelLaps  = (float)st.fuel_laps;
        int   fuelMix   = st.fuel_mix;
        int   compound  = st.tyre_compound;
        int   visual    = st.visual_compound;
        int   tyreAge   = st.tyre_age_laps;
        float brakeBias = (float)st.front_brake_bias;
        bool  drsOk     = st.drs_allowed;

        const bool haveErs = std::isfinite(ersPct);
        const QColor ersColor = ersPct > 60 ? tnr::themed("#5794F2", "#0B57D0")
                              : ersPct > 30 ? tnr::themed("#d4ad04", "#765900")
                              : QColor("#C4162A");
        setLabelText(rp_ersPct, haveErs ? QString::number(ersPct, 'f', 1) + "%" : "—%");
        setLabelStyle(rp_ersPct, haveErs ? QString("color: %1;").arg(ersColor.name()) : QString());
        const int ersBarValue = haveErs ? qBound(0, qRound(ersPct), 100) : 0;
        if (rp_ersBar->value() != ersBarValue) rp_ersBar->setValue(ersBarValue);
        const QString ersStyle = haveErs
            ? QString("QProgressBar::chunk { background-color: %1; }").arg(ersColor.name())
            : QString();
        if (rp_ersBar->styleSheet() != ersStyle) rp_ersBar->setStyleSheet(ersStyle);
        const auto mj = [](double joules) { return QString::number(joules / 1'000'000.0, 'f', 2); };
        setLabelText(rp_ersStore, mj(st.ers_j) + " MJ / 4.00 MJ");
        setLabelText(rp_ersDeployed, mj(st.ers_deployed_j) + " MJ");
        setLabelText(rp_ersHarvested,
                     mj(double(st.ers_harvested_mguk_j) + double(st.ers_harvested_mguh_j)) + " MJ");

        // ERS deploy mode label (protocol-aware: "Overtake" → "Boost" in 2026).
        setLabelText(rp_ersMode, ersMode >= 0 && ersMode < 4 ? tnr::Ln("ers.mode", ersMode) : "—");
        // Mode colour: none / blue / yellow / red for modes 0–3, as Electron.
        const QColor modeColor = ersMode == 1 ? tnr::themed("#5794F2", "#0B57D0")
                               : ersMode == 2 ? tnr::compoundMediumColor()
                               : ersMode == 3 ? QColor("#C4162A") : QColor();
        setLabelStyle(rp_ersMode, modeColor.isValid()
            ? QString("color: %1; font-weight: bold;").arg(modeColor.name()) : QString());
        if (rp_drsLabel) setLabelText(rp_drsLabel, tnr::L("drs.label"));

        setLabelText(rp_drs, drsAvailable ? (drsOk ? "AVAILABLE" : "LOCKED") : "—");
        setLabelStyle(rp_drs, drsAvailable && drsOk
            ? "color: #37872D; font-weight: bold;" : "");
        const auto density = cardDensity_[2];

        const bool haveFuelKg = std::isfinite(fuelKg);
        const bool haveFuelLaps = std::isfinite(fuelLaps);
        setLabelText(rp_fuelKg, haveFuelKg ? QString::number(fuelKg, 'f', 1) + " kg" : "—");
        // Signed laps of fuel margin; Spacious spells out "vs finish" (Electron).
        setLabelText(rp_fuelLaps, haveFuelLaps
            ? QString("%1%2 laps%3").arg(fuelLaps >= 0 ? "+" : "").arg(fuelLaps, 0, 'f', 1)
                  .arg(density == tnr::DensityMode::Spacious ? QStringLiteral(" vs finish") : QString())
            : "—");
        const QColor fuelColor = fuelLaps > 1.0f ? tnr::themed("#37872D", "#137333")
                               : fuelLaps >= 0.0f ? tnr::themed("#d4ad04", "#8B5200")
                               : QColor("#C4162A");
        setLabelStyle(rp_fuelKg, haveFuelKg && haveFuelLaps
            ? QString("color: %1; font-weight: bold;").arg(fuelColor.name()) : QString());

        static const char* mixes[] = {"Lean", "Standard", "Rich", "Max power"};
        setLabelText(rp_fuelMix, fuelMix >= 0 && fuelMix < 4 ? mixes[fuelMix] : "—");

        setLabelText(rp_tyre, tyreLabel(compound));
        QColor tyreFg = tyreTextColor(compound, visual);
        setLabelStyle(rp_tyre, tyreFg.isValid()
            ? QString("color: %1; font-weight: bold;").arg(tyreFg.name())
            : "font-weight: bold;");
        setLabelText(rp_tyreAge, tyreAge >= 0 ? QString::number(tyreAge) + "L" : "—");
        // Brake bias as Electron: "56% F" (Compact), "56% front", and
        // "56% front · 44% rear" (Spacious).
        const int front = qRound(brakeBias);
        setLabelText(rp_brakeBias, !(std::isfinite(brakeBias) && brakeBias >= 0) ? QStringLiteral("—")
            : density == tnr::DensityMode::Compact ? QString("%1% F").arg(front)
            : density == tnr::DensityMode::Spacious
                ? QString("%1% front · %2% rear").arg(front).arg(100 - front)
            : QString("%1% front").arg(front));
    };

    if (viewingOther && allStatus) {
        for (const AllStatusCar& car : allStatus->cars) {
            if (car.idx == selectedCarIdx_) {
                applyStatus(car, !allStatusDrsAvailable ||
                                  allStatusDrsAvailable->contains(car.idx));
                break;
            }
        }
    } else if (playerStatus) {
        applyStatus(*playerStatus, playerDrsAvailable);
    }
}

// ── Standings table updater ───────────────────────────────────────────────

void StandingsPage::updateTimingTable(const TimingRow* timing,
                                      const tnrp::ParticipantsRow* participants,
                                      const AllStatusRow* allStatus) {
    if (!timingTable_) return;
    if (!timing) { showPlaceholderRows(); return; }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const TimingCar& car : timing->cars)
        trackSectors(tableSectors_[car.idx], car.lap_num, car.sector, car.s1_ms, car.s2_ms,
                     car.last_lap_ms, now);

    struct DriverInfo { QString name; int raceNum; QColor color; };
    std::unordered_map<int, DriverInfo> driverMap;
    if (participants) {
        for (const tnrp::Driver& d : participants->drivers) {
            if (d.idx < 0) continue;
            driverMap[d.idx] = {
                QString::fromStdString(d.name),
                d.race_number,
                QColor(d.livery_color.empty() ? QStringLiteral("#8e8e8e")
                                              : QString::fromStdString(d.livery_color))
            };
        }
    }

    struct TyreInfo { int compound; int visual; };
    std::unordered_map<int, TyreInfo> tyreMap;
    if (allStatus) {
        for (const AllStatusCar& c : allStatus->cars) {
            if (c.idx >= 0)
                tyreMap[c.idx] = { c.tyre_compound, c.visual_compound };
        }
    }

    int playerIdx = timing->player_idx;

    std::vector<const TimingCar*> active;
    for (const TimingCar& car : timing->cars) {
        if (car.result_status >= 2 && car.position > 0) active.push_back(&car);
    }
    std::sort(active.begin(), active.end(),
        [](const TimingCar* a, const TimingCar* b) {
            return a->position < b->position;
        });

    bool orderOrSettingChanged = false;
    float currentThreshold = contrastThreshold();
    if (active.size() != tableRowCarIdx_.size() || lastContrastThreshold_ != currentThreshold) {
        orderOrSettingChanged = true;
    } else {
        for (int i = 0; i < (int)active.size(); ++i) {
            if (active[i]->idx != tableRowCarIdx_[i]) {
                orderOrSettingChanged = true; break;
            }
        }
    }
    lastContrastThreshold_ = currentThreshold;

    if (orderOrSettingChanged) {
        rowSafeColors_.resize(active.size());
        auto blend = [](const QColor& fg, const QColor& bg) {
            float alpha = fg.alphaF();
            return QColor::fromRgbF(
                fg.redF() * alpha + bg.redF() * (1.0f - alpha),
                fg.greenF() * alpha + bg.greenF() * (1.0f - alpha),
                fg.blueF() * alpha + bg.blueF() * (1.0f - alpha)
            );
        };
        for (int row = 0; row < (int)active.size(); ++row) {
            int idx = active[row]->idx;
            auto di = driverMap.find(idx);
            QColor rawColor = (di != driverMap.end()) ? di->second.color : QColor("#8e8e8e");
            QColor bgNormal = timingTable_->palette().color(row % 2 == 0 ? QPalette::Base : QPalette::AlternateBase);
            QColor fallback = timingTable_->palette().color(QPalette::Text);

            QColor accentColor = timingTable_->palette().color(QPalette::Highlight);
            accentColor.setAlpha(38);
            QColor bgHighlight = blend(accentColor, bgNormal);

            QColor fastestColor(191, 95, 255, 38);
            QColor bgFastest = blend(fastestColor, bgNormal);

            rowSafeColors_[row].normal = ensureContrast(rawColor, bgNormal, fallback, currentThreshold);
            rowSafeColors_[row].highlighted = ensureContrast(rawColor, bgHighlight, fallback, currentThreshold);
            rowSafeColors_[row].fastestLap = ensureContrast(rawColor, bgFastest, fallback, currentThreshold);
        }
    }

    // Keep the table's item objects stable. Replacing ~240 QTableWidgetItems on
    // every timing packet caused allocator churn, delegate invalidation and a
    // repaint per cell. Batch the mutations and touch only changed roles.
    timingTable_->setUpdatesEnabled(false);
    if (timingTable_->rowCount() != (int)active.size())
        timingTable_->setRowCount((int)active.size());
    tableRowCarIdx_.resize(active.size());
    for (int i = 0; i < (int)active.size(); ++i)
        tableRowCarIdx_[i] = active[i]->idx;

    auto updateItem = [this](int row, int column, const QString& text,
                             Qt::Alignment alignment = {}, const QColor& foreground = {},
                             const QBrush& background = {}, const QVariant& userData = {}) {
        QTableWidgetItem* item = timingTable_->item(row, column);
        if (!item) {
            item = new QTableWidgetItem;
            timingTable_->setItem(row, column, item);
        }
        if (item->text() != text) item->setText(text);
        const int desiredAlignment = alignment == Qt::Alignment{}
            ? 0 : static_cast<int>(alignment);
        if (item->textAlignment() != desiredAlignment)
            item->setTextAlignment(alignment);

        const QVariant fg = foreground.isValid() ? QVariant(foreground) : QVariant();
        if (item->data(Qt::ForegroundRole) != fg) item->setData(Qt::ForegroundRole, fg);
        const QVariant bg = background.style() != Qt::NoBrush ? QVariant(background) : QVariant();
        if (item->data(Qt::BackgroundRole) != bg) item->setData(Qt::BackgroundRole, bg);
        if (item->data(Qt::UserRole) != userData) item->setData(Qt::UserRole, userData);
    };

    for (int row = 0; row < (int)active.size(); ++row) {
        const TimingCar& car = *active[row];
        int  idx        = car.idx;
        int  pos        = car.position;
        int  lapNum     = car.lap_num;
        int  lastLapMs  = car.last_lap_ms;
        int  s1Ms       = car.s1_ms;
        int  s2Ms       = car.s2_ms;
        int  gapMs      = car.gap_ms;
        int  pitStatus  = car.pit_status;
        bool lapInvalid = car.lap_invalid;
        int  penaltiesS = car.penalties_s;
        int  numDt      = car.num_dt_pens;
        int  numSg      = car.num_sg_pens;
        int  resultSt   = car.result_status;
        bool isPlayer   = (idx == playerIdx);

        // Sectors: the finished lap's S1/S2/S3 for 7 s after the line, else the
        // live S1/S2 with S3 blank.
        int s3Ms = 0;
        if (const auto it = tableSectors_.find(idx);
            it != tableSectors_.end() && it->second.frozen(now)) {
            s1Ms = it->second.frozenS1; s2Ms = it->second.frozenS2; s3Ms = it->second.frozenS3;
        }

        auto di = driverMap.find(idx);
        int     raceNum    = (di != driverMap.end()) ? di->second.raceNum : 0;
        QString driverName = (di != driverMap.end())
            ? di->second.name : QString("Car %1").arg(idx);
        QColor driverColor = (di != driverMap.end()) ? di->second.color : QColor("#8e8e8e");

        int compound = tyreMap.count(idx) ? tyreMap.at(idx).compound : -1;
        int visual   = tyreMap.count(idx) ? tyreMap.at(idx).visual   : -1;

        // Retirement replaces the gap (red), as Electron.
        QString retired;
        if      (resultSt == 4) retired = "DNF";
        else if (resultSt == 5) retired = "DSQ";
        else if (resultSt == 7) retired = "RET";

        // Electron's stacked badges: pit/INV in STATUS, every penalty in PENALTIES.
        QVariantList statusBadges, penaltyBadges;
        if (pitStatus > 0)
            statusBadges << badge(pitStatus == 1 ? QStringLiteral("PIT") : QStringLiteral("PIT LANE"),
                                  tnr::compoundMediumColor());
        if (lapInvalid) statusBadges << badge(QStringLiteral("INV"), QColor("#C4162A"));
        if (penaltiesS > 0)
            penaltyBadges << badge(QString("+%1s").arg(penaltiesS), tnr::themed("#c47d0e", "#8B5200"));
        if (numDt > 0)
            penaltyBadges << badge(numDt > 1 ? QString("%1× DT").arg(numDt) : QStringLiteral("DT"),
                                   QColor("#e10600"));
        if (numSg > 0)
            penaltyBadges << badge(numSg > 1 ? QString("%1× SG").arg(numSg) : QStringLiteral("SG"),
                                   QColor("#e10600"));

        QColor posColor;
        if      (pos == 1) posColor = tnr::themed("#FFD700", "#765900");
        else if (pos == 2) posColor = tnr::themed("#C0C0C0", "#5E6475");
        else if (pos == 3) posColor = tnr::themed("#CD7F32", "#9C5B23");

        // Highlight when this driver's data is shown in the race panel:
        // — explicit selection, or player when nothing is selected
        bool showingThisDriver = (idx == selectedCarIdx_) ||
                                 (isPlayer && selectedCarIdx_ == -1);
        bool isFastest = (idx == fastestLapCarIdx_);
        QBrush bgBrush;
        bool hasCustomBg = false;

        if (showingThisDriver) {
            QColor accentColor = timingTable_->palette().color(QPalette::Highlight);
            accentColor.setAlpha(38); // ~15% opacity
            bgBrush = QBrush(accentColor);
            hasCustomBg = true;
        } else if (isFastest) {
            bgBrush = QBrush(QColor(191, 95, 255, 38)); // #BF5FFF ~15% opacity
            hasCustomBg = true;
        }

        // Col 0: POS
        updateItem(row, 0, QString("P%1").arg(pos), Qt::AlignCenter,
                   posColor, hasCustomBg ? bgBrush : QBrush());

        QColor safeDriverColor;
        if (showingThisDriver) safeDriverColor = rowSafeColors_[row].highlighted;
        else if (isFastest)    safeDriverColor = rowSafeColors_[row].fastestLap;
        else                   safeDriverColor = rowSafeColors_[row].normal;

        // Col 1: #
        updateItem(row, 1, raceNum > 0 ? QString::number(raceNum) : "—",
                   Qt::AlignCenter, safeDriverColor,
                   hasCustomBg ? bgBrush : QBrush());

        // Col 2: DRIVER
        updateItem(row, 2, driverName, {}, safeDriverColor,
                   hasCustomBg ? bgBrush : QBrush(), isPlayer);

        updateItem(row, 3, lapNum > 0 ? QString::number(lapNum) : "—",
                   Qt::AlignCenter, {}, hasCustomBg ? bgBrush : QBrush());
        updateItem(row, 4, formatTowerLapTime(lastLapMs), Qt::AlignCenter, {},
                   hasCustomBg ? bgBrush : QBrush());
        updateItem(row, 5, retired.isEmpty() ? formatGap(gapMs, pos == 1) : retired, Qt::AlignCenter,
                   retired.isEmpty() ? QColor() : QColor("#C4162A"),
                   hasCustomBg ? bgBrush : QBrush());
        updateItem(row, 6, formatSector(s1Ms), Qt::AlignCenter, {},
                   hasCustomBg ? bgBrush : QBrush());
        updateItem(row, 7, formatSector(s2Ms), Qt::AlignCenter, {},
                   hasCustomBg ? bgBrush : QBrush());
        updateItem(row, 8, formatSector(s3Ms), Qt::AlignCenter, {},
                   hasCustomBg ? bgBrush : QBrush());

        // Col 9: TYRE
        QColor tyreFg = tyreTextColor(compound, visual);
        updateItem(row, 9, tyreLabel(compound), Qt::AlignCenter, tyreFg,
                   hasCustomBg ? bgBrush : QBrush());

        // Col 10: PENALTIES (badges painted by BadgeDelegate)
        updateItem(row, 10, QString(), {}, {}, hasCustomBg ? bgBrush : QBrush());
        if (QTableWidgetItem* item = timingTable_->item(row, 10);
            item && item->data(BadgeRole).toList() != penaltyBadges)
            item->setData(BadgeRole, penaltyBadges);

        // Col 11: STATUS (badges painted by BadgeDelegate)
        updateItem(row, 11, QString(), {}, {}, hasCustomBg ? bgBrush : QBrush());
        if (QTableWidgetItem* item = timingTable_->item(row, 11);
            item && item->data(BadgeRole).toList() != statusBadges)
            item->setData(BadgeRole, statusBadges);

        const int rowHeight = tableDensity_ == tnr::DensityMode::Compact ? 22
            : tableDensity_ == tnr::DensityMode::Spacious ? 34 : 28;
        if (timingTable_->rowHeight(row) != rowHeight)
            timingTable_->setRowHeight(row, rowHeight);
    }
    timingTable_->setUpdatesEnabled(true);
    timingTable_->viewport()->update();
}

void StandingsPage::setTableDensity(tnr::DensityMode mode) {
    if (tableDensity_ == mode) return;
    tableDensity_ = mode;
    QFont font = timingTable_->horizontalHeader()->font();
    font.setPointSize(mode == tnr::DensityMode::Compact ? 7
                      : mode == tnr::DensityMode::Spacious ? 9 : 8);
    timingTable_->horizontalHeader()->setFont(font);
    timingTable_->horizontalHeader()->setFixedHeight(
        mode == tnr::DensityMode::Compact ? 22
        : mode == tnr::DensityMode::Spacious ? 34 : 28);
    const int rowHeight = mode == tnr::DensityMode::Compact ? 22
        : mode == tnr::DensityMode::Spacious ? 34 : 28;
    for (int row = 0; row < timingTable_->rowCount(); ++row)
        timingTable_->setRowHeight(row, rowHeight);
}

void StandingsPage::setCardDensity(int card, tnr::DensityMode mode) {
    if (card < 0 || card >= 3 || cardDensity_[card] == mode) return;
    cardDensity_[card] = mode;
    rebuildRacePanel();
}

void StandingsPage::rebuildRacePanel() {
    if (!sidebar_) return;
    auto* layout = qobject_cast<QHBoxLayout*>(this->layout());
    if (!layout) return;
    layout->removeWidget(sidebar_);
    sidebar_->deleteLater();
    sidebar_ = buildRacePanel();
    layout->addWidget(sidebar_);
    applyLayout();
}
