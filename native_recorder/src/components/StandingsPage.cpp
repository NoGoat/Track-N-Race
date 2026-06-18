#include "../MainWindow.h"
#include "TyreHelpers.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
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

#include <algorithm>
#include <unordered_map>
#include <vector>

#include <cmath>

// ── Standings helpers ─────────────────────────────────────────────────────

static float relativeLuminance(const QColor& c) {
    auto toLinear = [](float v) {
        return v <= 0.03928f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    };
    float r = toLinear(c.redF());
    float g = toLinear(c.greenF());
    float b = toLinear(c.blueF());
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

static float contrastRatio(const QColor& c1, const QColor& c2) {
    float l1 = relativeLuminance(c1);
    float l2 = relativeLuminance(c2);
    if (l1 < l2) std::swap(l1, l2);
    return (l1 + 0.05f) / (l2 + 0.05f);
}

static QColor ensureContrast(const QColor& fg, const QColor& bg, const QColor& fallback, float threshold) {
    if (!fg.isValid()) return fallback;
    if (contrastRatio(fg, bg) < threshold) return fallback;
    return fg;
}

static QString formatLapTime(int ms) {
    if (ms <= 0) return "—";
    int min  = ms / 60000;
    int sec  = (ms % 60000) / 1000;
    int msec = ms % 1000;
    return QString("%1:%2.%3")
        .arg(min).arg(sec, 2, 10, QChar('0')).arg(msec, 3, 10, QChar('0'));
}

static QString formatSector(int ms) {
    if (ms <= 0) return "—";
    int sec  = ms / 1000;
    int msec = ms % 1000;
    return QString("%1.%2").arg(sec).arg(msec, 3, 10, QChar('0'));
}

static QString formatGap(int ms, bool isLeader) {
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

// ── Standings page builder ────────────────────────────────────────────────

QWidget* MainWindow::buildStandingsPage() {
    QWidget* w = new QWidget;
    QHBoxLayout* hbox = new QHBoxLayout(w);
    hbox->setContentsMargins(0, 0, 0, 0);
    hbox->setSpacing(0);

    timingTable = new QTableWidget;
    timingTable->setColumnCount(12);
    timingTable->setHorizontalHeaderLabels(
        {"POS", "#", "DRIVER", "LAP", "LAST LAP", "GAP", "S1", "S2", "S3", "TYRE", "PENALTIES", "STATUS"});
    timingTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    timingTable->setSelectionMode(QAbstractItemView::NoSelection);
    timingTable->setShowGrid(false);
    timingTable->setAlternatingRowColors(true);
    // Pixel-based scrolling — the default ScrollPerItem snaps a whole row per
    // wheel notch / scrollbar step, which feels chunky and "laggy"; per-pixel is
    // smooth.
    timingTable->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    timingTable->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    timingTable->verticalHeader()->setVisible(false);
    // Fixed/interactive widths — NOT ResizeToContents, which re-measures every
    // cell on every setItem and tanks the UI when the table rebuilds rapidly.
    timingTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    timingTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    {
        const int colW[12] = { 44, 36, 0, 46, 84, 80, 60, 60, 60, 52, 74, 72 };
        for (int c = 0; c < 12; ++c)
            if (c != 2) timingTable->setColumnWidth(c, colW[c]);
    }

    QFont hf; hf.setPointSize(7);
    timingTable->horizontalHeader()->setFont(hf);

    timingTable->setItemDelegateForColumn(2, new DriverDelegate(timingTable));

    connect(timingTable, &QTableWidget::cellClicked, this, [this](int row, int) {
        int clicked = (row >= 0 && row < (int)tableRowCarIdx.size())
                      ? tableRowCarIdx[row] : -1;
        selectedCarIdx = (clicked == selectedCarIdx) ? -1 : clicked;
        updateTimingTable();
        updateRacePanel();
    });

    hbox->addWidget(timingTable, 1);

    QFrame* vdiv = new QFrame;
    vdiv->setFrameShape(QFrame::VLine);
    vdiv->setFrameShadow(QFrame::Sunken);
    hbox->addWidget(vdiv);

    hbox->addWidget(buildRacePanel());
    return w;
}

// ── Race panel builder ────────────────────────────────────────────────────

QWidget* MainWindow::buildRacePanel() {
    QScrollArea* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFixedWidth(240);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    QWidget* w = new QWidget;
    QVBoxLayout* vbox = new QVBoxLayout(w);
    vbox->setContentsMargins(14, 14, 14, 14);
    vbox->setSpacing(6);

    // Helper: key / value row
    auto makeRow = [&](const QString& label, QLabel*& valueOut) -> QWidget* {
        QWidget* row = new QWidget;
        QHBoxLayout* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 4, 0, 4);
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
        QLabel* lbl = new QLabel(title);
        QFont f; f.setPointSize(8); f.setBold(true);
        lbl->setFont(f);
        lbl->setForegroundRole(QPalette::PlaceholderText);
        vbox->addWidget(lbl);
    };

    auto addDivider = [&]() {
        QFrame* div = new QFrame;
        div->setFrameShape(QFrame::HLine);
        div->setFrameShadow(QFrame::Sunken);
        vbox->addWidget(div);
    };

    // ── Driver header ────────────────────────────────────────────
    rp_driverName = new QLabel("—");
    QFont dnF; dnF.setPointSize(12); dnF.setBold(true);
    rp_driverName->setFont(dnF);
    rp_driverName->setAlignment(Qt::AlignCenter);
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
    sh->setContentsMargins(0, 4, 0, 0);
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
    vbox->addWidget(rp_ersPct);

    rp_ersBar = new QProgressBar;
    rp_ersBar->setRange(0, 100);
    rp_ersBar->setValue(0);
    rp_ersBar->setTextVisible(false);
    rp_ersBar->setFixedHeight(6);
    vbox->addWidget(rp_ersBar);

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

// ── Race panel updater ────────────────────────────────────────────────────

void MainWindow::updateRacePanel() {
    if (!rp_lapNum) return;

    int playerIdx    = lastTimingData.empty() ? -1 : lastTimingData.value("player_idx", -1);
    bool viewingOther = (selectedCarIdx != -1 && selectedCarIdx != playerIdx);

    if (viewingOther && !lastParticipantsData.empty() && lastParticipantsData.contains("drivers")) {
        QString name;
        for (const auto& d : lastParticipantsData["drivers"]) {
            if (d.value("idx", -1) == selectedCarIdx) {
                name = QString("#%1 %2")
                    .arg(d.value("race_number", 0))
                    .arg(QString::fromStdString(d.value("name", "")));
                break;
            }
        }
        rp_driverName->setText(name.isEmpty() ? QString("Car %1").arg(selectedCarIdx) : name);
    } else {
        rp_driverName->setText("Your Car");
    }

    auto applyTiming = [&](const nlohmann::json& lap) {
        int lapNum    = lap.value("lap_num",        0);
        int pos       = lap.value("position",       0);
        int pitSt     = lap.value("pit_status",     0);
        int currentMs = lap.value("current_lap_ms", 0);
        int lastMs    = lap.value("last_lap_ms",    0);
        int s1Ms      = lap.value("s1_ms",          0);
        int s2Ms      = lap.value("s2_ms",          0);
        bool invalid  = lap.value("lap_invalid",    false);
        int  penS     = lap.value("penalties_s",    0);

        rp_lapNum->setText(lapNum > 0 ? QString::number(lapNum) : "—");
        rp_position->setText(pos > 0 ? "P" + QString::number(pos) : "—");

        QStringList flags;
        if (pitSt == 1)      flags << "In pit lane";
        else if (pitSt == 2) flags << "In pit";
        if (invalid)         flags << "INVALID";
        if (penS > 0)        flags << ("+" + QString::number(penS) + "s");
        rp_pitStatus->setText(flags.isEmpty() ? "—" : flags.join(" · "));
        rp_pitStatus->setStyleSheet(flags.isEmpty() ? "" : "color: #C4162A; font-weight: bold;");

        rp_currentLap->setText(formatLapTime(currentMs));
        rp_lastLap->setText(formatLapTime(lastMs));
        rp_s1->setText(formatSector(s1Ms));
        rp_s2->setText(formatSector(s2Ms));
    };

    if (viewingOther && !lastTimingData.empty() && lastTimingData.contains("cars")) {
        for (const auto& car : lastTimingData["cars"]) {
            if (car.value("idx", -1) == selectedCarIdx) { applyTiming(car); break; }
        }
    } else if (!lastPlayerLapData.empty()) {
        applyTiming(lastPlayerLapData);
    }

    auto applyStatus = [&](const nlohmann::json& st) {
        float ersPct    = st.value("ers_pct",          0.0f);
        int   ersMode   = st.value("ers_mode",         0);
        float fuelKg    = st.value("fuel_kg",          0.0f);
        float fuelLaps  = st.value("fuel_laps",        0.0f);
        int   fuelMix   = st.value("fuel_mix",         0);
        int   compound  = st.value("tyre_compound",    -1);
        int   visual    = st.value("visual_compound",  -1);
        int   tyreAge   = st.value("tyre_age_laps",    0);
        float brakeBias = st.value("front_brake_bias", 0.0f);
        bool  drsOk     = st.value("drs_allowed",      false);

        rp_ersPct->setText(QString::number((int)ersPct) + "%");
        rp_ersBar->setValue((int)ersPct);
        const char* ersColor = ersPct > 60 ? "#4488ff" : ersPct > 30 ? "#ffd700" : "#C4162A";
        rp_ersBar->setStyleSheet(
            QString("QProgressBar::chunk { background-color: %1; }").arg(ersColor));

        static const char* ersModes[] = {"None", "Auto", "Hotlap", "Overtake"};
        rp_ersMode->setText(ersMode >= 0 && ersMode < 4 ? ersModes[ersMode] : "—");

        rp_drs->setText(drsOk ? "AVAILABLE" : "LOCKED");
        rp_drs->setStyleSheet(drsOk ? "color: #37872D; font-weight: bold;" : "");

        rp_fuelKg->setText(QString::number(fuelKg, 'f', 1) + " kg");
        rp_fuelLaps->setText(QString::number(fuelLaps, 'f', 1) + "L");
        const char* fuelColor = fuelLaps > 1.0f ? "#37872D" : fuelLaps >= 0.0f ? "#ffd700" : "#C4162A";
        rp_fuelKg->setStyleSheet(QString("color: %1; font-weight: bold;").arg(fuelColor));

        static const char* mixes[] = {"Lean", "Standard", "Rich", "Max Power"};
        rp_fuelMix->setText(fuelMix >= 0 && fuelMix < 4 ? mixes[fuelMix] : "—");

        rp_tyre->setText(tyreLabel(compound));
        QColor tyreFg = tyreTextColor(visual);
        rp_tyre->setStyleSheet(tyreFg.isValid()
            ? QString("color: %1; font-weight: bold;").arg(tyreFg.name())
            : "font-weight: bold;");
        rp_tyreAge->setText(tyreAge > 0 ? QString::number(tyreAge) + "L" : "—");
        rp_brakeBias->setText(brakeBias > 0 ? QString::number(brakeBias, 'f', 1) + "% front" : "—");
    };

    if (viewingOther && !lastAllStatusData.empty() && lastAllStatusData.contains("cars")) {
        for (const auto& car : lastAllStatusData["cars"]) {
            if (car.value("idx", -1) == selectedCarIdx) { applyStatus(car); break; }
        }
    } else if (!lastPlayerStatusData.empty()) {
        applyStatus(lastPlayerStatusData);
    }
}

// ── Standings table updater ───────────────────────────────────────────────

void MainWindow::updateTimingTable() {
    if (!timingTable || lastTimingData.empty()) return;

    struct DriverInfo { QString name; int raceNum; QColor color; };
    std::unordered_map<int, DriverInfo> driverMap;
    if (!lastParticipantsData.empty() && lastParticipantsData.contains("drivers")) {
        for (const auto& d : lastParticipantsData["drivers"]) {
            int idx = d.value("idx", -1);
            if (idx < 0) continue;
            driverMap[idx] = {
                QString::fromStdString(d.value("name", "")),
                d.value("race_number", 0),
                QColor(QString::fromStdString(d.value("livery_color", "#8e8e8e")))
            };
        }
    }

    struct TyreInfo { int compound; int visual; };
    std::unordered_map<int, TyreInfo> tyreMap;
    if (!lastAllStatusData.empty() && lastAllStatusData.contains("cars")) {
        for (const auto& c : lastAllStatusData["cars"]) {
            int idx = c.value("idx", -1);
            if (idx >= 0)
                tyreMap[idx] = { c.value("tyre_compound", -1), c.value("visual_compound", -1) };
        }
    }

    int playerIdx = lastTimingData.value("player_idx", -1);
    const auto& cars = lastTimingData["cars"];

    std::vector<nlohmann::json> active;
    for (const auto& car : cars) {
        int rs  = car.value("result_status", 0);
        int pos = car.value("position", 0);
        if (rs >= 2 && pos > 0) active.push_back(car);
    }
    std::sort(active.begin(), active.end(),
        [](const nlohmann::json& a, const nlohmann::json& b) {
            return a.value("position", 99) < b.value("position", 99);
        });

    bool orderOrSettingChanged = false;
    float currentThreshold = contrastThreshold();
    if (active.size() != tableRowCarIdx.size() || lastContrastThreshold != currentThreshold) {
        orderOrSettingChanged = true;
    } else {
        for (int i = 0; i < (int)active.size(); ++i) {
            if (active[i].value("idx", -1) != tableRowCarIdx[i]) {
                orderOrSettingChanged = true; break;
            }
        }
    }
    lastContrastThreshold = currentThreshold;

    if (orderOrSettingChanged) {
        rowSafeColors.resize(active.size());
        for (int row = 0; row < (int)active.size(); ++row) {
            int idx = active[row].value("idx", -1);
            auto di = driverMap.find(idx);
            QColor rawColor = (di != driverMap.end()) ? di->second.color : QColor("#8e8e8e");
            QColor bgColor = timingTable->palette().color(row % 2 == 0 ? QPalette::Base : QPalette::AlternateBase);
            QColor fallback = timingTable->palette().color(QPalette::Text);
            rowSafeColors[row] = ensureContrast(rawColor, bgColor, fallback, currentThreshold);
        }
    }

    timingTable->setRowCount((int)active.size());
    tableRowCarIdx.resize(active.size());
    for (int i = 0; i < (int)active.size(); ++i)
        tableRowCarIdx[i] = active[i].value("idx", -1);

    for (int row = 0; row < (int)active.size(); ++row) {
        const auto& car = active[row];
        int  idx        = car.value("idx",           -1);
        int  pos        = car.value("position",       0);
        int  lapNum     = car.value("lap_num",        0);
        int  lastLapMs  = car.value("last_lap_ms",    0);
        int  s1Ms       = car.value("s1_ms",          0);
        int  s2Ms       = car.value("s2_ms",          0);
        int  gapMs      = car.value("gap_ms",         0);
        int  pitStatus  = car.value("pit_status",     0);
        bool lapInvalid = car.value("lap_invalid",    false);
        int  penaltiesS = car.value("penalties_s",    0);
        int  numDt      = car.value("num_dt_pens",    0);
        int  numSg      = car.value("num_sg_pens",    0);
        int  resultSt   = car.value("result_status",  0);
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
        bool showingThisDriver = (idx == selectedCarIdx) ||
                                 (isPlayer && selectedCarIdx == -1);
        QFont cellFont;

        bool isFastest = (idx == fastestLapCarIdx_);
        QBrush bgBrush;
        bool hasCustomBg = false;

        if (showingThisDriver) {
            QColor accentColor = timingTable->palette().color(QPalette::Highlight);
            accentColor.setAlpha(38); // ~15% opacity
            bgBrush = QBrush(accentColor);
            hasCustomBg = true;
        } else if (isFastest) {
            bgBrush = QBrush(QColor(191, 95, 255, 38)); // #BF5FFF ~15% opacity
            hasCustomBg = true;
        }

        auto makeItem = [&](const QString& text, bool center = false) {
            auto* item = new QTableWidgetItem(text);
            item->setFont(cellFont);
            if (center) item->setTextAlignment(Qt::AlignCenter);
            if (hasCustomBg) item->setBackground(bgBrush);
            return item;
        };

        // Col 0: POS
        auto* posItem = makeItem(QString("P%1").arg(pos), true);
        if (posColor.isValid()) posItem->setForeground(posColor);
        timingTable->setItem(row, 0, posItem);

        QColor safeDriverColor = rowSafeColors[row];

        // Col 1: #
        auto* numItem = makeItem(raceNum > 0 ? QString::number(raceNum) : "—", true);
        numItem->setForeground(safeDriverColor);
        timingTable->setItem(row, 1, numItem);

        // Col 2: DRIVER
        auto* drvItem = makeItem(driverName);
        drvItem->setForeground(safeDriverColor);
        if (isPlayer) drvItem->setData(Qt::UserRole, true);
        timingTable->setItem(row, 2, drvItem);

        timingTable->setItem(row, 3, makeItem(lapNum > 0 ? QString::number(lapNum) : "—", true));
        timingTable->setItem(row, 4, makeItem(formatLapTime(lastLapMs), true));
        timingTable->setItem(row, 5, makeItem(formatGap(gapMs, pos == 1), true));
        timingTable->setItem(row, 6, makeItem(formatSector(s1Ms), true));
        timingTable->setItem(row, 7, makeItem(formatSector(s2Ms), true));
        timingTable->setItem(row, 8, makeItem(formatSector(s3Ms), true));

        // Col 9: TYRE
        auto* tyreItem = makeItem(tyreLabel(compound), true);
        QColor tyreFg = tyreTextColor(visual);
        if (tyreFg.isValid()) tyreItem->setForeground(tyreFg);
        timingTable->setItem(row, 9, tyreItem);

        // Col 10: PENALTIES
        auto* penItem = makeItem(penText);
        if (!penText.isEmpty()) penItem->setForeground(QColor("#C4162A"));
        timingTable->setItem(row, 10, penItem);

        // Col 11: STATUS
        timingTable->setItem(row, 11, makeItem(statusText));

        timingTable->setRowHeight(row, 22);
    }
}
