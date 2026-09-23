#include "StrategyPage.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QProgressBar>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <cmath>

namespace {
class MinimumStopsSpinBox : public QSpinBox {
public:
    using QSpinBox::QSpinBox;
protected:
    void wheelEvent(QWheelEvent* event) override {
        clearFocus();
        event->ignore();
    }
};

QString timeText(double value) {
    if (!std::isfinite(value) || value <= 0 || value > 600000) return QStringLiteral("—");
    const double seconds = value / 1000.0;
    return QStringLiteral("%1:%2").arg(int(seconds / 60))
        .arg(std::fmod(seconds, 60.0), 4, 'f', 1, QLatin1Char('0'));
}
QString deltaText(double value) {
    if (!std::isfinite(value)) return QStringLiteral("—");
    return QStringLiteral("%1%2").arg(value > 0 ? QStringLiteral("+") : value < 0 ? QStringLiteral("−") : QString())
        .arg(std::abs(value) / 1000.0, 0, 'f', 1);
}
QString words(const std::string& value) {
    QString out = QString::fromStdString(value); return out.replace(QLatin1Char('_'), QLatin1Char(' '));
}
bool isDark() { return QApplication::palette().color(QPalette::Window).lightness() < 128; }
QColor defensiveColor() { return QColor(isDark() ? "#5794f2" : "#0b57d0"); }
QColor attackingColor() { return QColor(isDark() ? "#fade2a" : "#8b5200"); }
QColor compoundColor(int visual) {
    const bool dark = isDark();
    switch (visual) {
    case 16: return QColor(dark ? "#e8002d" : "#c8001a");
    case 17: return QColor(dark ? "#ffd700" : "#765900");
    case 18: return QColor(dark ? "#c8c8c8" : "#555555");
    case 7: return QColor(dark ? "#39b54a" : "#1e7a2e");
    case 8: return QColor(dark ? "#4488ff" : "#1a55bb");
    default: return QApplication::palette().color(QPalette::Text);
    }
}
QColor wearColor(double value) {
    if (value < 20) return QColor("#73bf69");
    if (value < 40) return QColor("#a8d436");
    if (value < 60) return QColor("#fade2a");
    if (value < 80) return QColor("#ff9830");
    return QColor("#c4162a");
}
QProgressBar* makeBar(double value, const QColor& color) {
    auto* bar = new QProgressBar;
    bar->setRange(0, 1000);
    bar->setValue(std::isfinite(value) ? qRound(std::clamp(value, 0.0, 100.0) * 10) : 0);
    bar->setTextVisible(false);
    bar->setFixedHeight(6);
    bar->setStyleSheet(QStringLiteral(
        "QProgressBar { border:0; border-radius:3px; background:palette(midlight); }"
        "QProgressBar::chunk { border-radius:3px; background:%1; }").arg(color.name()));
    return bar;
}
QLabel* makeLabel(const QString& text) {
    auto* out = new QLabel(QString(text).replace(QStringLiteral("palette(mid)"),
        QApplication::palette().color(QPalette::PlaceholderText).name()));
    out->setTextInteractionFlags(Qt::TextSelectableByMouse); return out;
}
QFrame* makeRule() {
    auto* out = new QFrame; out->setFrameShape(QFrame::HLine); out->setFrameShadow(QFrame::Plain); return out;
}
QFrame* makeVerticalRule() {
    auto* out = new QFrame;
    out->setFrameShape(QFrame::VLine); out->setFrameShadow(QFrame::Plain);
    out->setFixedWidth(1); return out;
}
QWidget* makeTyreCondition(const tnrp::StrategySnapshotRow* s) {
    auto* body = new QWidget; auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(makeLabel(QStringLiteral("<b>TYRE CONDITION</b><br><span style='color:palette(mid)'>%1</span>")
        .arg(s ? QStringLiteral("%1 limits · cliff L%2").arg(QString::fromStdString(s->limiting_corner).toHtmlEscaped())
            .arg(s->cliff_lap) : QStringLiteral("—"))));
    auto* grid = new QGridLayout; grid->setHorizontalSpacing(16); grid->setVerticalSpacing(8);
    const std::array<const char*, 4> corners{{"FL", "FR", "RL", "RR"}};
    const std::array<double, 4> wear{{s ? s->wear_fl : 0, s ? s->wear_fr : 0, s ? s->wear_rl : 0, s ? s->wear_rr : 0}};
    for (size_t i = 0; i < corners.size(); ++i) {
        auto* cell = new QVBoxLayout;
        auto* values = new QHBoxLayout;
        values->addWidget(makeLabel(QString::fromLatin1(corners[i]))); values->addStretch();
        values->addWidget(makeLabel(s ? QStringLiteral("<b style='color:%1'>%2%</b>").arg(wearColor(wear[i]).name()).arg(qRound(wear[i]))
            : QStringLiteral("—")));
        cell->addLayout(values); cell->addWidget(makeBar(wear[i], wearColor(wear[i])));
        grid->addLayout(cell, int(i / 2), int(i % 2));
    }
    layout->addLayout(grid); return body;
}
QString shortTyreScope(QString value) {
    value.replace(QStringLiteral("front-left"), QStringLiteral("FL"), Qt::CaseInsensitive);
    value.replace(QStringLiteral("front-right"), QStringLiteral("FR"), Qt::CaseInsensitive);
    value.replace(QStringLiteral("rear-left"), QStringLiteral("RL"), Qt::CaseInsensitive);
    value.replace(QStringLiteral("rear-right"), QStringLiteral("RR"), Qt::CaseInsensitive);
    value.remove(QRegularExpression(QStringLiteral("\\s+tyres?$"), QRegularExpression::CaseInsensitiveOption));
    value.replace(QRegularExpression(QStringLiteral("\\s+and\\s+"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(" + "));
    return value.toUpper();
}
std::pair<QString, QString> wearWarningParts(const std::string& text) {
    const QString value = QString::fromStdString(text);
    static const std::array<QRegularExpression, 4> patterns{{
        QRegularExpression(QStringLiteral("^(.+?) is the limiting tyre$"), QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral("^(.+?) wear far above average$"), QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral("^(.+?) wearing faster than (.+)$"), QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral("^(.+?) wearing faster$"), QRegularExpression::CaseInsensitiveOption)}};
    for (size_t i = 0; i < patterns.size(); ++i) {
        const auto match = patterns[i].match(value);
        if (!match.hasMatch()) continue;
        const QString rawScope = match.captured(1);
        QString scope = shortTyreScope(rawScope);
        scope += rawScope.contains(QStringLiteral("tyres"), Qt::CaseInsensitive) || scope.contains(QLatin1Char('+'))
            ? QStringLiteral(" TYRES") : QStringLiteral(" TYRE");
        const QString detail = i == 0 ? QStringLiteral("is limiting") : i == 1 ? QStringLiteral("wear far above average")
            : i == 2 ? QStringLiteral("High wear rate vs %1").arg(shortTyreScope(match.captured(2))) : QStringLiteral("High wear rate");
        return {scope, detail};
    }
    return {QString(), value};
}
void clearLayout(QLayout* layout) {
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (item->widget()) { item->widget()->hide(); item->widget()->deleteLater(); }
        if (item->layout()) { clearLayout(item->layout()); delete item->layout(); }
        delete item;
    }
}
}

StrategyPage::StrategyPage(QWidget* parent) : QWidget(parent) {
    density_ = tnr::densityFromValue(settings_.value(QStringLiteral("ui/compact/strategySummary"), "normal"));
    root_ = new QVBoxLayout(this); root_->setContentsMargins(0, 0, 0, 0); root_->setSpacing(0); rebuild();
}
void StrategyPage::update(const tnrp::StrategySnapshotRow* value) {
    if (value) snapshot_ = *value; else snapshot_.reset(); rebuild();
}
void StrategyPage::resetForNewSession() {
    snapshot_.reset();
    if (conservativeScroll_) conservativeScroll_->verticalScrollBar()->setValue(0);
    if (aggressiveScroll_) aggressiveScroll_->verticalScrollBar()->setValue(0);
    if (sidebarScroll_) sidebarScroll_->verticalScrollBar()->setValue(0);
    rebuild();
}
void StrategyPage::setCompactMode(bool on) {
    setDensityMode(on ? tnr::DensityMode::Compact : tnr::DensityMode::Normal);
}
void StrategyPage::setDensityMode(tnr::DensityMode mode) {
    if (density_ == mode) return;
    density_ = mode;
    settings_.setValue(QStringLiteral("ui/compact/strategySummary"), tnr::densityValue(mode));
    rebuild();
}
void StrategyPage::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange && root_) rebuild();
}

void StrategyPage::rebuild() {
    // Preserve the editor (including uncommitted text) across snapshot refreshes.
    if (minimumStopsControl_) {
        restoreMinimumStopsFocus_ = minimumStopsInput_->hasFocus();
        minimumStopsControl_->setParent(this);
        minimumStopsControl_->hide();
    }
    const int cy = conservativeScroll_ ? conservativeScroll_->verticalScrollBar()->value() : 0;
    const int ay = aggressiveScroll_ ? aggressiveScroll_->verticalScrollBar()->value() : 0;
    const int sy = sidebarScroll_ ? sidebarScroll_->verticalScrollBar()->value() : 0;
    conservativeScroll_ = aggressiveScroll_ = sidebarScroll_ = nullptr; clearLayout(root_);
    const auto* s = snapshot_ ? &*snapshot_ : nullptr;

    // Full-width summary above adjoining strategy columns and a separated sidebar.
    root_->addWidget(makeHeader());
    root_->addWidget(makeRule());
    if (s && s->state == "non_race") {
        auto* empty = makeLabel(QStringLiteral("<b>Race sessions only</b><br><br>Strategy suggestions are available during Race, Race 2, and Race 3 sessions."));
        empty->setAlignment(Qt::AlignCenter); empty->setWordWrap(true);
        root_->addWidget(empty, 1); return;
    }
    auto* body = new QWidget; auto* cols = new QHBoxLayout(body);
    cols->setContentsMargins(0, 0, 0, 0); cols->setSpacing(0);
    if (s && s->state == "ready") {
        cols->addWidget(makePlan(QStringLiteral("Defensive"), s->conservative, defensiveColor(), &conservativeScroll_), 3);
        cols->addWidget(makeVerticalRule());
        cols->addWidget(makePlan(QStringLiteral("Attacking"), s->aggressive, attackingColor(), &aggressiveScroll_), 3);
        cols->addWidget(makeVerticalRule());
        cols->addWidget(withMinimumStops(makeSidebar()), 2);
    } else {
        // Strategy is still being calculated: an indeterminate bar, no text.
        auto* pending = new QWidget; auto* centre = new QVBoxLayout(pending);
        auto* bar = new QProgressBar; bar->setRange(0, 0); bar->setTextVisible(false);
        bar->setFixedSize(128, 4);
        centre->addStretch(); centre->addWidget(bar, 0, Qt::AlignCenter); centre->addStretch();
        cols->addWidget(pending, 6);
        cols->addWidget(makeVerticalRule());
        cols->addWidget(withMinimumStops(makeWaitingSidebar()), 2);
    }
    root_->addWidget(body, 1);
    // Scope restoration to this body, so a newer snapshot cannot receive an old scroll position.
    QTimer::singleShot(0, body, [this, body, cy, ay, sy] {
        if (root_->indexOf(body) < 0) return;
        if (conservativeScroll_) conservativeScroll_->verticalScrollBar()->setValue(cy);
        if (aggressiveScroll_) aggressiveScroll_->verticalScrollBar()->setValue(ay);
        if (sidebarScroll_) sidebarScroll_->verticalScrollBar()->setValue(sy);
    });
}

QWidget* StrategyPage::makeHeader() {
    const auto* s = snapshot_ ? &*snapshot_ : nullptr;
    const bool ready = s && s->state == "ready";
    const bool compact = density_ == tnr::DensityMode::Compact;
    const bool spacious = density_ == tnr::DensityMode::Spacious;
    const int pad = compact ? 6 : spacious ? 20 : 12;
    const int horizontalPad = spacious ? 32 : 24;
    const QColor secondary = palette().color(QPalette::PlaceholderText);
    const QString lap = s && s->lap_num > 0 ? QString::number(s->lap_num) : QStringLiteral("—");
    const QString total = s && s->total_laps > 0 ? QString::number(s->total_laps) : QStringLiteral("—");
    const int wear = ready ? qRound(s->average_wear) : 0;
    const QColor wearTint = ready ? wearColor(wear) : secondary;
    const QColor cliffTint = !ready ? secondary : s->laps_until_cliff <= 5 ? QColor("#c4162a")
        : s->laps_until_cliff <= 10 ? QColor("#fade2a") : palette().color(QPalette::Text);

    auto* header = new QWidget; auto* row = new QHBoxLayout(header);
    row->setContentsMargins(0, 0, 0, 0); row->setSpacing(0);
    auto addDivider = [&] {
        auto* rule = new QFrame; rule->setFrameShape(QFrame::VLine); rule->setFrameShadow(QFrame::Plain);
        row->addWidget(rule);
    };
    auto* lapCell = new QWidget; auto* lapLayout = new QVBoxLayout(lapCell);
    lapLayout->setContentsMargins(horizontalPad, pad, horizontalPad, pad); lapLayout->setSpacing(4);
    if (!compact) lapLayout->addWidget(makeLabel(QStringLiteral("<span style='color:%1'>LAP</span>").arg(secondary.name())));
    lapLayout->addWidget(makeLabel(QStringLiteral("<b style='font-size:%1px'>%2</b> <span style='color:%3'>/ %4</span>")
        .arg(compact ? 18 : spacious ? 36 : 30).arg(lap, secondary.name(), total)));
    if (spacious && s && s->lap_num > 0 && s->total_laps > 0)
        lapLayout->addWidget(makeLabel(QStringLiteral("%1% distance").arg(qRound(100.0 * s->lap_num / s->total_laps))));
    row->addWidget(lapCell); addDivider();

    auto* tyreCell = new QWidget; auto* tyreLayout = new QHBoxLayout(tyreCell);
    tyreLayout->setContentsMargins(horizontalPad, pad, horizontalPad, pad); tyreLayout->setSpacing(compact ? 12 : spacious ? 24 : 16);
    const QColor compound = compoundColor(s ? s->current_visual_compound : 0);
    auto* chip = makeLabel(ready ? QString::fromStdString(s->current_compound_name) : QStringLiteral("—"));
    chip->setTextFormat(Qt::PlainText); chip->setContentsMargins(8, 2, 8, 2);
    chip->setStyleSheet(QStringLiteral("color:%1; border:1px solid %1; border-radius:3px; font-weight:bold;").arg(compound.name()));
    tyreLayout->addWidget(chip);
    auto* wearValue = makeLabel(ready ? QStringLiteral("%1%").arg(wear) : QStringLiteral("—"));
    wearValue->setStyleSheet(QStringLiteral("color:%1; font-weight:bold; font-size:%2px;")
        .arg(wearTint.name()).arg(compact ? 14 : spacious ? 24 : 18));
    auto* detail = makeLabel(ready ? QStringLiteral("%1L%2 · %3%/L")
        .arg(s->current_tyre_age_laps).arg(spacious ? QStringLiteral(" age") : QString()).arg(s->wear_per_lap, 0, 'f', 1)
        : QStringLiteral("—"));
    detail->setStyleSheet(QStringLiteral("color:%1; font-size:%2px;").arg(secondary.name()).arg(spacious ? 12 : 10));
    auto* bar = makeBar(wear, wearColor(wear));
    if (compact) {
        tyreLayout->addWidget(wearValue); tyreLayout->addWidget(bar, 1); tyreLayout->addWidget(detail);
    } else {
        auto* wearGroup = new QWidget; auto* wearLayout = new QVBoxLayout(wearGroup);
        wearLayout->setContentsMargins(0, 0, 0, 0); wearLayout->setSpacing(spacious ? 8 : 6);
        auto* values = new QHBoxLayout;
        values->addWidget(wearValue); values->addStretch(); values->addWidget(detail);
        wearLayout->addLayout(values); wearLayout->addWidget(bar);
        if (spacious) {
            bar->setFixedHeight(10);
            if (ready && !s->limiting_corner.empty()) {
                auto* limiting = makeLabel(QStringLiteral("%1 is limiting tyre").arg(QString::fromStdString(s->limiting_corner)));
                limiting->setTextFormat(Qt::PlainText); wearLayout->addWidget(limiting);
            }
        }
        tyreLayout->addWidget(wearGroup, 1);
    }
    row->addWidget(tyreCell, 1); addDivider();

    auto* cliffCell = new QWidget; auto* cliffLayout = new QBoxLayout(compact ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom, cliffCell);
    cliffLayout->setContentsMargins(horizontalPad, pad, horizontalPad, pad); cliffLayout->setSpacing(compact ? 8 : 4);
    cliffLayout->addWidget(makeLabel(QStringLiteral("<span style='color:%1'>TYRE CLIFF</span>").arg(secondary.name())));
    cliffLayout->addWidget(makeLabel(QStringLiteral("<b style='color:%1; font-size:%2px'>%3</b>%4")
        .arg(cliffTint.name()).arg(compact ? 14 : spacious ? 24 : 18)
        .arg(ready ? QStringLiteral("Lap %1").arg(s->cliff_lap) : QStringLiteral("—"))
        .arg(ready ? QStringLiteral(" <span style='color:%1'>+%2%3</span>").arg(secondary.name()).arg(s->laps_until_cliff)
            .arg(spacious ? QStringLiteral("L") : QString()) : QString())));
    if (spacious && ready)
        cliffLayout->addWidget(makeLabel(QStringLiteral("%1 laps remaining").arg(s->laps_until_cliff)));
    row->addWidget(cliffCell);
    return header;
}

QWidget* StrategyPage::withMinimumStops(QWidget* content) {
    auto* sidebar = new QWidget;
    auto* layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(content, 1);
    layout->addWidget(makeRule());
    if (!minimumStopsControl_) {
        minimumStopsControl_ = new QWidget;
        auto* row = new QHBoxLayout(minimumStopsControl_);
        auto* label = new QLabel(QStringLiteral("Required pit stops"));
        auto* stops = new MinimumStopsSpinBox;
        minimumStopsInput_ = stops;
        stops->setRange(0, 8);
        stops->setValue(minimumStops_);
        stops->setAccessibleName(label->text());
        label->setBuddy(stops);
        row->addWidget(label);
        row->addWidget(stops);
        connect(stops, &QSpinBox::valueChanged, this, [this](int value) {
            minimumStops_ = value;
            emit minimumStopsChanged(value);
        });
    }
    layout->addWidget(minimumStopsControl_);
    minimumStopsControl_->show();
    if (restoreMinimumStopsFocus_) minimumStopsInput_->setFocus(Qt::OtherFocusReason);
    restoreMinimumStopsFocus_ = false;
    return sidebar;
}

QWidget* StrategyPage::makeWaitingSidebar() {
    auto* area = new QScrollArea;
    area->setWidgetResizable(true); area->setFrameShape(QFrame::NoFrame);
    area->setBackgroundRole(QPalette::Window);
    area->viewport()->setBackgroundRole(QPalette::Window);
    auto* body = new QWidget; auto* v = new QVBoxLayout(body);
    v->setContentsMargins(10, 10, 10, 10); v->setSpacing(8);

    auto* pitWindow = makeLabel(QStringLiteral("<b>NEXT PIT WINDOW</b><br><br>"
        "<span style='color:%1'><b>DEFEND —</b></span> · —<br>"
        "<span style='color:%2'><b>ATTACK —</b></span> · —").arg(defensiveColor().name(), attackingColor().name()));
    pitWindow->setWordWrap(true); v->addWidget(pitWindow); v->addWidget(makeRule());

    v->addWidget(makeLabel(QStringLiteral("<b>WEATHER WINDOW</b><br>—<br>"
        "<span style='color:palette(mid)'>—</span>")));
    v->addWidget(makeRule());

    v->addWidget(makeTyreCondition(nullptr));
    v->addStretch(); area->setWidget(body); sidebarScroll_ = area; return area;
}

QWidget* StrategyPage::makePlan(const QString& title, const tnrp::StrategyPlan& plan,
                                const QColor& accent, QScrollArea** scroll) {
    auto* frame = new QFrame; frame->setFrameShape(QFrame::NoFrame);
    auto* outer = new QVBoxLayout(frame); outer->setContentsMargins(0, 0, 0, 0); outer->setSpacing(0);
    const QString target = plan.target_idx >= 0
        ? QStringLiteral("%1 %2").arg(plan.mode == "attacking" ? QStringLiteral("Chasing") : QStringLiteral("Covering"),
            QString::fromStdString(plan.target_name).toHtmlEscaped())
        : QStringLiteral("Tyre-life baseline");
    const QString legality = plan.legal ? QString() : QStringLiteral(" · <span style='color:#c4162a'>No legal set path</span>");
    auto* heading = makeLabel(QStringLiteral("<b>%1</b>  ·  %2 stop%3%4<br><span style='color:palette(mid)'>%5 · %6% confidence%7</span>")
        .arg(title).arg(plan.stops).arg(plan.stops == 1 ? QString() : QStringLiteral("s"))
        .arg(legality, target).arg(qRound(plan.confidence * 100))
        .arg(plan.requires_compound_change ? QStringLiteral(" · compound change required") : QString()));
    heading->setContentsMargins(12, 10, 12, 10);
    heading->setWordWrap(true);
    heading->setStyleSheet(QStringLiteral("border-left:4px solid %1;").arg(accent.name())); outer->addWidget(heading);
    outer->addWidget(makeRule());
    auto* area = new QScrollArea; area->setWidgetResizable(true); area->setFrameShape(QFrame::NoFrame);
    auto* body = new QWidget; auto* v = new QVBoxLayout(body); v->setContentsMargins(0, 0, 0, 0); v->setSpacing(0);
    bool firstStint = true;
    for (const auto& stint : plan.stints) {
        if (!firstStint) v->addWidget(makeRule());
        firstStint = false;
        auto* card = new QFrame; card->setFrameShape(QFrame::NoFrame);
        auto* cv = new QVBoxLayout(card); cv->setContentsMargins(0, 0, 0, 0); cv->setSpacing(0);
        auto* stintLabel = makeLabel(QStringLiteral("<b><span style='color:%1'>●</span> Stint %2 · %3</b>  L%4–%5")
            .arg(compoundColor(stint.visual_compound).name()).arg(stint.stint_number)
            .arg(QString::fromStdString(stint.compound_name).toHtmlEscaped()).arg(stint.start_lap).arg(stint.end_lap));
        stintLabel->setContentsMargins(12, 8, 12, 0); cv->addWidget(stintLabel);
        auto* counts = makeLabel(QStringLiteral("Expected <b>%1</b> · Actual <b>%2</b>").arg(stint.expected_laps).arg(stint.actual_laps));
        counts->setContentsMargins(12, 4, 12, 8); cv->addWidget(counts);
        if (!stint.rows.empty()) {
            auto* table = new QTableWidget((int)stint.rows.size(), 6);
            table->setFrameShape(QFrame::NoFrame);
            table->setHorizontalHeaderLabels({QStringLiteral("Lap"), QStringLiteral("Required"),
                QStringLiteral("Actual"), QStringLiteral("Δ lap"), QStringLiteral("Δ stint"), QStringLiteral("Δ total")});
            table->verticalHeader()->hide(); table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
            table->setEditTriggers(QAbstractItemView::NoEditTriggers); table->setSelectionMode(QAbstractItemView::NoSelection);
            table->setFocusPolicy(Qt::NoFocus);
            table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            table->setAlternatingRowColors(true); table->setShowGrid(false);
            const int rowHeight = std::max(26, table->fontMetrics().height() + 8);
            table->verticalHeader()->setMinimumSectionSize(rowHeight);
            table->verticalHeader()->setDefaultSectionSize(rowHeight);
            table->horizontalHeader()->setFixedHeight(rowHeight + 4);
            // The plan owns scrolling, including long future stints, just as in Electron.
            table->setFixedHeight(rowHeight + 4 + rowHeight * int(stint.rows.size()) + 2 * table->frameWidth());
            for (int r = 0; r < (int)stint.rows.size(); ++r) {
                const auto& x = stint.rows[(size_t)r];
                const QStringList values{QString::number(x.lap_num), timeText(x.required_ms),
                    x.has_actual ? timeText(x.actual_ms) : QStringLiteral("—"),
                    x.has_actual ? deltaText(x.delta_lap_ms) : QStringLiteral("—"),
                    x.has_actual ? deltaText(x.delta_stint_ms) : QStringLiteral("—"),
                    x.has_actual ? deltaText(x.delta_total_ms) : QStringLiteral("—")};
                for (int c = 0; c < values.size(); ++c) {
                    auto* item = new QTableWidgetItem(values[c]); item->setTextAlignment(Qt::AlignCenter);
                    if (c >= 3) {
                        const double delta = c == 3 ? x.delta_lap_ms : c == 4 ? x.delta_stint_ms : x.delta_total_ms;
                        item->setForeground(x.has_actual ? (delta > 0 ? QColor("#c4162a") : QColor("#73bf69"))
                            : palette().color(QPalette::PlaceholderText));
                    }
                    if (!stint.is_last && x.lap_num == stint.end_lap) item->setBackground(QColor(115, 191, 105, 38));
                    table->setItem(r, c, item);
                }
            }
            cv->addWidget(table);
        }
        v->addWidget(card);
    }
    v->addStretch(); area->setWidget(body); outer->addWidget(area, 1); *scroll = area; return frame;
}

QWidget* StrategyPage::makeSidebar() {
    const auto& s = *snapshot_; auto* area = new QScrollArea;
    area->setWidgetResizable(true); area->setFrameShape(QFrame::NoFrame);
    area->setBackgroundRole(QPalette::Window);
    area->viewport()->setBackgroundRole(QPalette::Window);
    auto* body = new QWidget; auto* v = new QVBoxLayout(body); v->setContentsMargins(10, 10, 10, 10); v->setSpacing(8);
    if (s.neutralisation) {
        const auto& n = *s.neutralisation;
        auto* card = makeLabel(QStringLiteral("<b>%1 DECISION</b><br><span style='font-size:18px'><b>%2</b></span> · P%3 → P%4<br>"
            "<span style='color:palette(mid)'>%5</span><br><br>Box now %6 s · wait to L%7 / %8 s")
            .arg(n.kind == "safety_car" ? QStringLiteral("SAFETY CAR") : QStringLiteral("VSC"),
                n.recommendation == "box" ? QStringLiteral("BOX NOW") : QStringLiteral("STAY OUT"))
            .arg(n.current_position).arg(n.recommendation == "box" ? n.projected_box_position : n.projected_stay_position)
            .arg(words(n.reason).toHtmlEscaped(), deltaText(n.box_now_cost_ms))
            .arg(n.box_later_lap).arg(deltaText(n.box_later_cost_ms)));
        card->setWordWrap(true);
        card->setStyleSheet(QStringLiteral("border-left:4px solid %1; padding:8px;")
            .arg(n.recommendation == "box" ? QStringLiteral("#73bf69") : QStringLiteral("#fade2a")));
        v->addWidget(card); v->addWidget(makeRule());
    }
    if (s.call) {
        v->addWidget(makeLabel(QStringLiteral("<b>RACE CALL</b>")));
        const QString color = s.call->kind == "undercut" || s.call->kind == "cover" ? QStringLiteral("#fade2a") : QStringLiteral("#73bf69");
        QString text = QStringLiteral("<b style='color:%1'>%2</b> <b>%3</b> · %4s<br><span style='color:palette(mid)'>%5%6</span>")
            .arg(color, QString::fromStdString(s.call->kind).toUpper().toHtmlEscaped(), QString::fromStdString(s.call->target_name).toHtmlEscaped())
            .arg(s.call->gap_ms / 1000.0, 0, 'f', 1)
            .arg(words(s.call->reason).toHtmlEscaped())
            .arg(s.call->crossover_laps ? QStringLiteral(" · %1 lap%2").arg(*s.call->crossover_laps)
                .arg(*s.call->crossover_laps == 1 ? QString() : QStringLiteral("s")) : QString());
        auto* call = makeLabel(text); call->setWordWrap(true);
        v->addWidget(call); v->addWidget(makeRule());
    }

    auto nextStop = [&](const tnrp::StrategyPlan& plan) -> std::optional<int> {
        for (const auto& stint : plan.stints)
            if (!stint.is_last && stint.end_lap >= s.lap_num) return stint.end_lap;
        return std::nullopt;
    };
    auto stopText = [&](const std::optional<int>& lap) {
        if (!lap) return QStringLiteral("FLAG");
        return *lap <= s.lap_num ? QStringLiteral("NOW") : QStringLiteral("L%1").arg(*lap);
    };
    const auto defensiveStop = nextStop(s.conservative);
    const auto attackingStop = nextStop(s.aggressive);
    const QString defensiveTarget = s.conservative.target_idx >= 0
        ? QStringLiteral("Cover %1").arg(QString::fromStdString(s.conservative.target_name).toHtmlEscaped())
        : QStringLiteral("Tyre life");
    const QString attackingTarget = s.aggressive.target_idx >= 0
        ? QStringLiteral("Chase %1").arg(QString::fromStdString(s.aggressive.target_name).toHtmlEscaped())
        : QStringLiteral("Tyre life");
    auto* pitWindow = makeLabel(QStringLiteral("<b>NEXT PIT WINDOW</b><br><br>"
        "<span style='color:%5'><b>DEFEND %1</b></span> · %2<br>"
        "<span style='color:%6'><b>ATTACK %3</b></span> · %4")
        .arg(stopText(defensiveStop), defensiveTarget, stopText(attackingStop), attackingTarget, defensiveColor().name(), attackingColor().name()));
    pitWindow->setWordWrap(true); v->addWidget(pitWindow); v->addWidget(makeRule());

    if (s.weather_strategy &&
        (s.weather_strategy->crossover_lap > 0 || s.weather_strategy->lap_delta_ms)) {
        const auto& weather = *s.weather_strategy;
        const QString when = weather.crossover_lap > 0
            ? QStringLiteral("%1% rain · %2").arg(weather.rain_percentage)
                .arg(weather.minutes_until_change > 0
                    ? QStringLiteral("about %1 min").arg(weather.minutes_until_change)
                    : QStringLiteral("now"))
            : words(weather.reason);
        QString pace;
        if (weather.lap_delta_ms) {
            const int delta = *weather.lap_delta_ms;
            pace = QStringLiteral("%1 %2s/L %3").arg(QString::fromStdString(weather.target_compound))
                .arg(std::abs(delta) / 1000.0, 0, 'f', 1)
                .arg(delta < 0 ? QStringLiteral("faster") : QStringLiteral("slower"));
            if (weather.set_wear)
                pace += *weather.set_wear == 0 ? QStringLiteral(" · new set")
                                               : QStringLiteral(" · %1% worn").arg(*weather.set_wear);
        }
        v->addWidget(makeLabel(QStringLiteral("<b>WEATHER WINDOW</b><br>%1%2<br>"
            "<span style='color:palette(mid)'>%3%4</span>")
            .arg(words(weather.recommendation).toHtmlEscaped(),
                 weather.crossover_lap > 0 ? QStringLiteral(" · L%1").arg(weather.crossover_lap) : QString(),
                 when.toHtmlEscaped(),
                 pace.isEmpty() ? QString() : QStringLiteral("<br>") + pace.toHtmlEscaped())));
        v->addWidget(makeRule());
    }
    if (!s.rivals.empty()) {
        v->addWidget(makeLabel(QStringLiteral("<b>RACE BATTLE</b>")));
        for (const auto& r : s.rivals) {
            const QString pace = std::abs(r.pace_delta_ms) > 50.0
                ? QStringLiteral("%1 s/L %2").arg(std::abs(r.pace_delta_ms) / 1000.0, 0, 'f', 2)
                    .arg(r.pace_delta_ms < 0 ? QStringLiteral("faster") : QStringLiteral("slower"))
                : QStringLiteral("matched pace");
            const QString context = r.last_pit_lap >= s.lap_num - 1
                ? QStringLiteral("stopped L%1").arg(r.last_pit_lap) : pace;
            auto* rival = makeLabel(QStringLiteral("P%1  <b>%2</b> · %3%4 s<br>"
                "<span style='color:palette(mid)'>%5 · %6L tyres · %7</span>")
                .arg(r.position).arg(QString::fromStdString(r.name).toHtmlEscaped())
                .arg(r.direction == "ahead" ? QStringLiteral("−") : QStringLiteral("+"))
                .arg(r.gap_ms / 1000.0, 0, 'f', 1)
                .arg(r.direction == "ahead" ? QStringLiteral("To catch") : QStringLiteral("To cover"))
                .arg(r.tyre_age_laps).arg(context));
            rival->setWordWrap(true); v->addWidget(rival);
        }
        v->addWidget(makeRule());
    }

    v->addWidget(makeTyreCondition(&s));
    if (!s.wear_warnings.empty()) {
        v->addWidget(makeRule());
        v->addWidget(makeLabel(QStringLiteral("<b>TYRE ALERTS</b>")));
    }
    for (const auto& warning : s.wear_warnings) {
        const auto [scope, detail] = wearWarningParts(warning.text);
        const QString color = warning.severity == "danger" ? "#c4162a" : warning.severity == "warning" ? "#ff9830" : "#fade2a";
        auto* w = makeLabel(QStringLiteral("<span style='color:%1'>⚠ <b>%2</b></span> <span style='color:palette(mid)'>%3</span>")
            .arg(color, scope.toHtmlEscaped(), detail.toHtmlEscaped()));
        w->setWordWrap(true); v->addWidget(w);
    }
    v->addStretch(); area->setWidget(body); sidebarScroll_ = area; return area;
}
