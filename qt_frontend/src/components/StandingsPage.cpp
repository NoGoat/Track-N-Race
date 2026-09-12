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

#include <algorithm>
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
        const int colW[12] = { 44, 36, 150, 46, 84, 80, 60, 60, 60, 52, 74, 72 };
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

    QFont hf; hf.setPointSize(7);
    timingTable_->horizontalHeader()->setFont(hf);

    timingTable_->setItemDelegateForColumn(2, new DriverDelegate(timingTable_));

    connect(timingTable_, &QTableWidget::cellClicked, this, [this](int row, int) {
        int clicked = (row >= 0 && row < (int)tableRowCarIdx_.size())
                      ? tableRowCarIdx_[row] : -1;
        selectedCarIdx_ = (clicked == selectedCarIdx_) ? -1 : clicked;
        emit refreshRequested();
    });

    hbox->addWidget(timingTable_, 1);

    hbox->addWidget(tnrui::vline());

    hbox->addWidget(buildRacePanel());
}

// ── Race panel builder ────────────────────────────────────────────────────

QWidget* StandingsPage::buildRacePanel() {
    QScrollArea* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFixedWidth(240);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    QWidget* w = new QWidget;
    QVBoxLayout* vbox = new QVBoxLayout(w);
    vbox->setContentsMargins(0, 14, 0, 14);
    vbox->setSpacing(6);

    // Helper: key / value row
    auto makeRow = [&](const QString& label, QLabel*& valueOut) -> QWidget* {
        QWidget* row = new QWidget;
        QHBoxLayout* h = new QHBoxLayout(row);
        h->setContentsMargins(14, 4, 14, 4);
        QLabel* lbl = new QLabel(label);
        QFont lf; lf.setPointSize(9); lbl->setFont(lf);
        lbl->setForegroundRole(QPalette::PlaceholderText);
        valueOut = new QLabel("—");
        QFont vf; vf.setPointSize(9); vf.setBold(true); valueOut->setFont(vf);
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
    QFont dnF; dnF.setPointSize(12); dnF.setBold(true);
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
    sh->setContentsMargins(14, 4, 14, 0);
    sh->setSpacing(8);
    auto makeSect = [&](const QString& lbl, QLabel*& out) {
        QWidget* sc = new QWidget;
        QVBoxLayout* sv = new QVBoxLayout(sc);
        sv->setContentsMargins(0, 0, 0, 0); sv->setSpacing(1);
        QLabel* l = new QLabel(lbl); QFont lf; lf.setPointSize(7); l->setFont(lf);
        l->setForegroundRole(QPalette::PlaceholderText); l->setAlignment(Qt::AlignCenter);
        out = new QLabel("—"); QFont vf; vf.setPointSize(8); vf.setBold(true);
        out->setFont(vf); out->setAlignment(Qt::AlignCenter);
        sv->addWidget(l); sv->addWidget(out);
        return sc;
    };
    sh->addWidget(makeSect("S1", rp_s1));
    sh->addWidget(makeSect("S2", rp_s2));
    vbox->addWidget(sectRow);

    addDivider();

    // ── ENERGY ───────────────────────────────────────────────────
    makeSection("ENERGY");

    rp_ersPct = new QLabel("—");
    QFont bigF; bigF.setPointSize(18); bigF.setBold(true);
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

    vbox->addWidget(makeRow("Mode", rp_ersMode));
    vbox->addWidget(makeRow("DRS",  rp_drs));

    addDivider();

    // ── STRATEGY ─────────────────────────────────────────────────
    makeSection("STRATEGY");
    vbox->addWidget(makeRow("Fuel",       rp_fuelKg));
    vbox->addWidget(makeRow("Fuel Laps",  rp_fuelLaps));
    vbox->addWidget(makeRow("Mix",        rp_fuelMix));
    vbox->addWidget(makeRow("Tyre",       rp_tyre));
    vbox->addWidget(makeRow("Tyre Age",   rp_tyreAge));
    vbox->addWidget(makeRow("Brake Bias", rp_brakeBias));

    vbox->addStretch();
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
}

// ── Race panel updater ────────────────────────────────────────────────────

void StandingsPage::updateRacePanel(const TimingRow* timing,
                                    const tnrp::ParticipantsRow* participants,
                                    const LapRow* playerLap,
                                    const StatusRow* playerStatus,
                                    const AllStatusRow* allStatus) {
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

    // Generic over LapRow and TimingCar — both carry the same timing field names.
    auto applyTiming = [&](const auto& lap) {
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

        QStringList flags;
        if (pitSt == 1)      flags << "In pit lane";
        else if (pitSt == 2) flags << "In pit";
        if (invalid)         flags << "INVALID";
        if (penS > 0)        flags << ("+" + QString::number(penS) + "s");
        setLabelText(rp_pitStatus, flags.isEmpty() ? "—" : flags.join(" · "));
        setLabelStyle(rp_pitStatus, flags.isEmpty() ? "" : "color: #C4162A; font-weight: bold;");

        setLabelText(rp_currentLap, formatLapTime(currentMs));
        setLabelText(rp_lastLap, formatLapTime(lastMs));
        setLabelText(rp_s1, formatSector(s1Ms));
        setLabelText(rp_s2, formatSector(s2Ms));
    };

    if (viewingOther && timing) {
        for (const TimingCar& car : timing->cars) {
            if (car.idx == selectedCarIdx_) { applyTiming(car); break; }
        }
    } else if (playerLap) {
        applyTiming(*playerLap);
    }

    // Generic over StatusRow and AllStatusCar — same status field names.
    auto applyStatus = [&](const auto& st) {
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

        setLabelText(rp_ersPct, QString::number((int)ersPct) + "%");
        if (rp_ersBar->value() != (int)ersPct) rp_ersBar->setValue((int)ersPct);
        const char* ersColor = ersPct > 60 ? "#4488ff" : ersPct > 30 ? "#ffd700" : "#C4162A";
        const QString ersStyle = QString("QProgressBar::chunk { background-color: %1; }").arg(ersColor);
        if (rp_ersBar->styleSheet() != ersStyle) rp_ersBar->setStyleSheet(ersStyle);

        // ERS deploy mode label (protocol-aware: "Overtake" → "Boost" in 2026).
        setLabelText(rp_ersMode, ersMode >= 0 && ersMode < 4 ? tnr::Ln("ers.mode", ersMode) : "—");

        setLabelText(rp_drs, drsOk ? "AVAILABLE" : "LOCKED");
        setLabelStyle(rp_drs, drsOk ? "color: #37872D; font-weight: bold;" : "");

        setLabelText(rp_fuelKg, QString::number(fuelKg, 'f', 1) + " kg");
        setLabelText(rp_fuelLaps, QString::number(fuelLaps, 'f', 1) + "L");
        const char* fuelColor = fuelLaps > 1.0f ? "#37872D" : fuelLaps >= 0.0f ? "#ffd700" : "#C4162A";
        setLabelStyle(rp_fuelKg, QString("color: %1; font-weight: bold;").arg(fuelColor));

        static const char* mixes[] = {"Lean", "Standard", "Rich", "Max Power"};
        setLabelText(rp_fuelMix, fuelMix >= 0 && fuelMix < 4 ? mixes[fuelMix] : "—");

        setLabelText(rp_tyre, tyreLabel(compound));
        QColor tyreFg = tyreTextColor(visual);
        setLabelStyle(rp_tyre, tyreFg.isValid()
            ? QString("color: %1; font-weight: bold;").arg(tyreFg.name())
            : "font-weight: bold;");
        setLabelText(rp_tyreAge, tyreAge > 0 ? QString::number(tyreAge) + "L" : "—");
        setLabelText(rp_brakeBias, brakeBias > 0 ? QString::number(brakeBias, 'f', 1) + "% front" : "—");
    };

    if (viewingOther && allStatus) {
        for (const AllStatusCar& car : allStatus->cars) {
            if (car.idx == selectedCarIdx_) { applyStatus(car); break; }
        }
    } else if (playerStatus) {
        applyStatus(*playerStatus);
    }
}

// ── Standings table updater ───────────────────────────────────────────────

void StandingsPage::updateTimingTable(const TimingRow* timing,
                                      const tnrp::ParticipantsRow* participants,
                                      const AllStatusRow* allStatus) {
    if (!timingTable_ || !timing) return;

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

        int s3Ms = (lastLapMs > 0 && s1Ms > 0 && s2Ms > 0) ? lastLapMs - s1Ms - s2Ms : 0;

        auto di = driverMap.find(idx);
        int     raceNum    = (di != driverMap.end()) ? di->second.raceNum : 0;
        QString driverName = (di != driverMap.end())
            ? di->second.name : QString("Car %1").arg(idx);
        QColor driverColor = (di != driverMap.end()) ? di->second.color : QColor("#8e8e8e");

        int compound = tyreMap.count(idx) ? tyreMap.at(idx).compound : -1;
        int visual   = tyreMap.count(idx) ? tyreMap.at(idx).visual   : -1;

        QString statusText;
        if      (resultSt == 4)  statusText = "DNF";
        else if (resultSt == 5)  statusText = "DSQ";
        else if (resultSt == 7)  statusText = "RET";
        else if (pitStatus == 1) statusText = "PITLANE";
        else if (pitStatus == 2) statusText = "IN PIT";
        else if (lapInvalid)     statusText = "INV";

        QString penText;
        if      (numDt > 0)      penText = QString("DT ×%1").arg(numDt);
        else if (numSg > 0)      penText = QString("SG ×%1").arg(numSg);
        else if (penaltiesS > 0) penText = QString("+%1s").arg(penaltiesS);

        QColor posColor;
        if      (pos == 1) posColor = QColor("#FFD700");
        else if (pos == 2) posColor = QColor("#C0C0C0");
        else if (pos == 3) posColor = QColor("#CD7F32");

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
        updateItem(row, 4, formatLapTime(lastLapMs), Qt::AlignCenter, {},
                   hasCustomBg ? bgBrush : QBrush());
        updateItem(row, 5, formatGap(gapMs, pos == 1), Qt::AlignCenter, {},
                   hasCustomBg ? bgBrush : QBrush());
        updateItem(row, 6, formatSector(s1Ms), Qt::AlignCenter, {},
                   hasCustomBg ? bgBrush : QBrush());
        updateItem(row, 7, formatSector(s2Ms), Qt::AlignCenter, {},
                   hasCustomBg ? bgBrush : QBrush());
        updateItem(row, 8, formatSector(s3Ms), Qt::AlignCenter, {},
                   hasCustomBg ? bgBrush : QBrush());

        // Col 9: TYRE
        QColor tyreFg = tyreTextColor(visual);
        updateItem(row, 9, tyreLabel(compound), Qt::AlignCenter, tyreFg,
                   hasCustomBg ? bgBrush : QBrush());

        // Col 10: PENALTIES
        updateItem(row, 10, penText, {},
                   penText.isEmpty() ? QColor() : QColor("#C4162A"),
                   hasCustomBg ? bgBrush : QBrush());

        // Col 11: STATUS
        updateItem(row, 11, statusText, {}, {},
                   hasCustomBg ? bgBrush : QBrush());

        if (timingTable_->rowHeight(row) != 22) timingTable_->setRowHeight(row, 22);
    }
    timingTable_->setUpdatesEnabled(true);
    timingTable_->viewport()->update();
}
