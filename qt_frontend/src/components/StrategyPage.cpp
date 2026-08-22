#include "StrategyPage.h"

#include <QAbstractItemView>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace {
QString timeText(double value) {
    if (!std::isfinite(value) || value <= 0) return QStringLiteral("—");
    const int n = qRound(value);
    return QStringLiteral("%1:%2.%3").arg(n / 60000)
        .arg((n / 1000) % 60, 2, 10, QLatin1Char('0')).arg(n % 1000, 3, 10, QLatin1Char('0'));
}
QString deltaText(double value) {
    if (!std::isfinite(value)) return QStringLiteral("—");
    return QStringLiteral("%1%2").arg(value >= 0 ? "+" : "").arg(value / 1000.0, 0, 'f', 3);
}
QString words(const std::string& value) {
    QString out = QString::fromStdString(value); return out.replace(QLatin1Char('_'), QLatin1Char(' '));
}
QColor compoundColor(int visual) {
    switch (visual) {
    case 16: return QColor("#e44b4b"); case 17: return QColor("#e5c94d");
    case 18: return QColor("#e7e7e7"); case 7: return QColor("#58b8ff");
    case 8: return QColor("#50c878"); default: return QColor("#9aa4b2");
    }
}
QLabel* makeLabel(const QString& text) {
    auto* out = new QLabel(text); out->setTextInteractionFlags(Qt::TextSelectableByMouse); return out;
}
QFrame* makeRule() {
    auto* out = new QFrame; out->setFrameShape(QFrame::HLine); out->setFrameShadow(QFrame::Plain); return out;
}
void clearLayout(QLayout* layout) {
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (item->widget()) item->widget()->deleteLater();
        if (item->layout()) { clearLayout(item->layout()); delete item->layout(); }
        delete item;
    }
}
}

StrategyPage::StrategyPage(QWidget* parent) : QWidget(parent) {
    compact_ = settings_.value(QStringLiteral("ui/compact/strategySummary"), false).toBool();
    root_ = new QVBoxLayout(this); root_->setContentsMargins(0, 0, 0, 0); root_->setSpacing(0); rebuild();
}
void StrategyPage::update(const tnrp::StrategySnapshotRow* value) {
    if (value) snapshot_ = *value; else snapshot_.reset(); rebuild();
}
void StrategyPage::resetForNewSession() { snapshot_.reset(); rebuild(); }
void StrategyPage::setCompactMode(bool on) {
    if (compact_ == on) return; compact_ = on;
    settings_.setValue(QStringLiteral("ui/compact/strategySummary"), on); rebuild();
}

void StrategyPage::rebuild() {
    const int cy = conservativeScroll_ ? conservativeScroll_->verticalScrollBar()->value() : 0;
    const int ay = aggressiveScroll_ ? aggressiveScroll_->verticalScrollBar()->value() : 0;
    const int sy = sidebarScroll_ ? sidebarScroll_->verticalScrollBar()->value() : 0;
    conservativeScroll_ = aggressiveScroll_ = sidebarScroll_ = nullptr; clearLayout(root_);
    const auto* s = snapshot_ ? &*snapshot_ : nullptr;

    auto* header = new QWidget; auto* hg = new QGridLayout(header);
    const int pad = compact_ ? 7 : 12; hg->setContentsMargins(18, pad, 18, pad); hg->setHorizontalSpacing(30);
    const QString lap = s ? QString::number(s->lap_num) : QStringLiteral("—");
    const QString total = s && s->total_laps > 0 ? QString::number(s->total_laps) : QStringLiteral("—");
    const QString tyre = s && !s->current_compound_name.empty() ? QString::fromStdString(s->current_compound_name) : QStringLiteral("—");
    const QString wear = s && s->state == "ready" ? QStringLiteral("%1 · %2% · %3%/L")
        .arg(QString::fromStdString(s->limiting_corner)).arg(s->limiting_wear, 0, 'f', 1)
        .arg(s->limiting_wear_per_lap, 0, 'f', 1) : QStringLiteral("—");
    const QString cliff = s && s->state == "ready" ? QStringLiteral("Lap %1 · +%2")
        .arg(s->cliff_lap).arg(s->laps_until_cliff) : QStringLiteral("—");
    hg->addWidget(makeLabel(QStringLiteral("<b>Lap</b><br><span style='font-size:18px'>%1 / %2</span>").arg(lap, total)), 0, 0);
    hg->addWidget(makeLabel(QStringLiteral("<b>Current tyre</b><br><span style='font-size:18px'>%1</span> · %2").arg(tyre, wear)), 0, 1);
    hg->addWidget(makeLabel(QStringLiteral("<b>Tyre cliff</b><br><span style='font-size:18px'>%1</span>").arg(cliff)), 0, 2);
    hg->setColumnStretch(0, 1); hg->setColumnStretch(1, 1); hg->setColumnStretch(2, 1); root_->addWidget(header);

    if (!s || s->state != "ready") {
        auto* empty = makeLabel(s && s->state == "non_race"
            ? QStringLiteral("Strategy is available in Race, Race 2 and Race 3 sessions.")
            : QStringLiteral("Waiting for race strategy data…"));
        empty->setAlignment(Qt::AlignCenter); empty->setStyleSheet(QStringLiteral("color: palette(mid); font-size:15px;"));
        root_->addWidget(empty, 1); return;
    }
    auto* body = new QWidget; auto* cols = new QHBoxLayout(body);
    cols->setContentsMargins(10, 10, 10, 10); cols->setSpacing(10);
    cols->addWidget(makePlan(QStringLiteral("Defensive"), s->conservative, QColor("#61c78b"), &conservativeScroll_), 3);
    cols->addWidget(makePlan(QStringLiteral("Attacking"), s->aggressive, QColor("#ef8c62"), &aggressiveScroll_), 3);
    cols->addWidget(makeSidebar(), 2); root_->addWidget(body, 1);
    QTimer::singleShot(0, this, [this, cy, ay, sy] {
        if (conservativeScroll_) conservativeScroll_->verticalScrollBar()->setValue(cy);
        if (aggressiveScroll_) aggressiveScroll_->verticalScrollBar()->setValue(ay);
        if (sidebarScroll_) sidebarScroll_->verticalScrollBar()->setValue(sy);
    });
}

QWidget* StrategyPage::makePlan(const QString& title, const tnrp::StrategyPlan& plan,
                                const QColor& accent, QScrollArea** scroll) {
    auto* frame = new QFrame; frame->setFrameShape(QFrame::StyledPanel);
    auto* outer = new QVBoxLayout(frame); outer->setContentsMargins(0, 0, 0, 0); outer->setSpacing(0);
    const QString target = plan.target_idx >= 0
        ? QStringLiteral("%1 %2").arg(plan.mode == "attacking" ? QStringLiteral("Chasing") : QStringLiteral("Covering"),
            QString::fromStdString(plan.target_name))
        : QStringLiteral("Tyre-life baseline");
    const QString legality = plan.legal ? QString() : QStringLiteral(" · <span style='color:#ef6262'>no legal set path</span>");
    auto* heading = makeLabel(QStringLiteral("<b>%1</b>  ·  %2 stop%3%4<br><span style='color:palette(mid)'>%5 · %6% confidence%7</span>")
        .arg(title).arg(plan.stops).arg(plan.stops == 1 ? QString() : QStringLiteral("s"))
        .arg(legality, target).arg(qRound(plan.confidence * 100))
        .arg(plan.requires_compound_change ? QStringLiteral(" · compound change required") : QString()));
    heading->setContentsMargins(12, 10, 12, 10);
    heading->setStyleSheet(QStringLiteral("border-left:4px solid %1;").arg(accent.name())); outer->addWidget(heading);
    auto* area = new QScrollArea; area->setWidgetResizable(true); area->setFrameShape(QFrame::NoFrame);
    auto* body = new QWidget; auto* v = new QVBoxLayout(body); v->setContentsMargins(8, 8, 8, 8); v->setSpacing(8);
    for (const auto& stint : plan.stints) {
        auto* card = new QFrame; card->setFrameShape(QFrame::StyledPanel);
        auto* cv = new QVBoxLayout(card); cv->setContentsMargins(8, 8, 8, 8);
        cv->addWidget(makeLabel(QStringLiteral("<b><span style='color:%1'>●</span> Stint %2 · %3</b>  L%4–%5")
            .arg(compoundColor(stint.visual_compound).name()).arg(stint.stint_number)
            .arg(QString::fromStdString(stint.compound_name)).arg(stint.start_lap).arg(stint.end_lap)));
        cv->addWidget(makeLabel(QStringLiteral("%1 / %2 laps completed").arg(stint.actual_laps).arg(stint.expected_laps)));
        if (!stint.rows.empty()) {
            auto* table = new QTableWidget((int)stint.rows.size(), 5);
            table->setHorizontalHeaderLabels({QStringLiteral("Lap"), QStringLiteral("Required"),
                QStringLiteral("Actual"), QStringLiteral("Δ lap"), QStringLiteral("Δ total")});
            table->verticalHeader()->hide(); table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
            table->setEditTriggers(QAbstractItemView::NoEditTriggers); table->setSelectionMode(QAbstractItemView::NoSelection);
            table->setFocusPolicy(Qt::NoFocus); table->setFixedHeight(std::min(280, 30 + (compact_ ? 22 : 26) * (int)stint.rows.size()));
            for (int r = 0; r < (int)stint.rows.size(); ++r) {
                const auto& x = stint.rows[(size_t)r];
                const QStringList values{QString::number(x.lap_num), timeText(x.required_ms),
                    x.has_actual ? timeText(x.actual_ms) : QStringLiteral("—"),
                    x.has_actual ? deltaText(x.delta_lap_ms) : QStringLiteral("—"),
                    x.has_actual ? deltaText(x.delta_total_ms) : QStringLiteral("—")};
                for (int c = 0; c < values.size(); ++c) table->setItem(r, c, new QTableWidgetItem(values[c]));
            }
            cv->addWidget(table);
        }
        v->addWidget(card);
    }
    v->addStretch(); area->setWidget(body); outer->addWidget(area, 1); *scroll = area; return frame;
}

QWidget* StrategyPage::makeSidebar() {
    const auto& s = *snapshot_; auto* area = new QScrollArea;
    area->setWidgetResizable(true); area->setFrameShape(QFrame::StyledPanel);
    auto* body = new QWidget; auto* v = new QVBoxLayout(body); v->setContentsMargins(10, 10, 10, 10); v->setSpacing(8);
    if (s.neutralisation) {
        const auto& n = *s.neutralisation;
        auto* card = makeLabel(QStringLiteral("<b>%1 DECISION</b><br><span style='font-size:18px'><b>%2</b></span> · P%3 → P%4<br>"
            "<span style='color:palette(mid)'>%5</span><br><br>Box now %6 s · wait to L%7 / %8 s")
            .arg(n.kind == "safety_car" ? QStringLiteral("SAFETY CAR") : QStringLiteral("VSC"),
                n.recommendation == "box" ? QStringLiteral("BOX NOW") : QStringLiteral("STAY OUT"))
            .arg(n.current_position).arg(n.recommendation == "box" ? n.projected_box_position : n.projected_stay_position)
            .arg(words(n.reason)).arg(n.box_now_cost_ms / 1000.0, 0, 'f', 1)
            .arg(n.box_later_lap).arg(n.box_later_cost_ms / 1000.0, 0, 'f', 1));
        card->setWordWrap(true);
        card->setStyleSheet(QStringLiteral("border-left:4px solid %1; padding:8px;")
            .arg(n.recommendation == "box" ? QStringLiteral("#61c78b") : QStringLiteral("#d2bd63")));
        v->addWidget(card); v->addWidget(makeRule());
    }
    if (s.call) {
        v->addWidget(makeLabel(QStringLiteral("<b>RACE CALL</b>")));
        QString text = QStringLiteral("<b>%1 %2</b><br>Gap %3 s")
            .arg(QString::fromStdString(s.call->kind).toUpper(), QString::fromStdString(s.call->target_name))
            .arg(s.call->gap_ms / 1000.0, 0, 'f', 1);
        if (s.call->crossover_laps) text += QStringLiteral(" · crossover %1 laps").arg(*s.call->crossover_laps);
        text += QStringLiteral("<br><span style='color:palette(mid)'>%1</span>").arg(words(s.call->reason));
        v->addWidget(makeLabel(text)); v->addWidget(makeRule());
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
        ? QStringLiteral("Cover %1").arg(QString::fromStdString(s.conservative.target_name))
        : QStringLiteral("Tyre life");
    const QString attackingTarget = s.aggressive.target_idx >= 0
        ? QStringLiteral("Chase %1").arg(QString::fromStdString(s.aggressive.target_name))
        : QStringLiteral("Tyre life");
    auto* pitWindow = makeLabel(QStringLiteral("<b>NEXT PIT WINDOW</b><br><br>"
        "<span style='color:#5794f2'><b>DEFEND %1</b></span> · %2<br>"
        "<span style='color:#d2bd63'><b>ATTACK %3</b></span> · %4")
        .arg(stopText(defensiveStop), defensiveTarget, stopText(attackingStop), attackingTarget));
    pitWindow->setWordWrap(true); v->addWidget(pitWindow); v->addWidget(makeRule());

    if (s.weather_strategy && s.weather_strategy->crossover_lap > 0) {
        const auto& weather = *s.weather_strategy;
        v->addWidget(makeLabel(QStringLiteral("<b>WEATHER WINDOW</b><br>%1 · L%2<br>"
            "<span style='color:palette(mid)'>%3% rain · about %4 min</span>")
            .arg(words(weather.recommendation)).arg(weather.crossover_lap)
            .arg(weather.rain_percentage).arg(weather.minutes_until_change)));
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
            v->addWidget(makeLabel(QStringLiteral("P%1  <b>%2</b> · %3%4 s<br>"
                "<span style='color:palette(mid)'>%5 · %6L tyres · %7</span>")
                .arg(r.position).arg(QString::fromStdString(r.name))
                .arg(r.direction == "ahead" ? QStringLiteral("−") : QStringLiteral("+"))
                .arg(r.gap_ms / 1000.0, 0, 'f', 1)
                .arg(r.direction == "ahead" ? QStringLiteral("To catch") : QStringLiteral("To cover"))
                .arg(r.tyre_age_laps).arg(context)));
        }
        v->addWidget(makeRule());
    }

    v->addWidget(makeLabel(QStringLiteral("<b>TYRE CONDITION</b><br>"
        "<span style='color:palette(mid)'>%1 limits · cliff L%2</span><br><br>"
        "FL %3%   FR %4%<br>RL %5%   RR %6%")
        .arg(QString::fromStdString(s.limiting_corner)).arg(s.cliff_lap)
        .arg(s.wear_fl, 0, 'f', 1).arg(s.wear_fr, 0, 'f', 1).arg(s.wear_rl, 0, 'f', 1).arg(s.wear_rr, 0, 'f', 1)));
    for (const auto& warning : s.wear_warnings) {
        auto* w = makeLabel(QStringLiteral("<b>%1</b>").arg(QString::fromStdString(warning.text)));
        const QString color = warning.severity == "danger" ? "#ef6262" : warning.severity == "warning" ? "#e3a84d" : "#d2bd63";
        w->setStyleSheet(QStringLiteral("border-left:3px solid %1; padding-left:7px;").arg(color)); w->setWordWrap(true); v->addWidget(w);
    }
    v->addStretch(); area->setWidget(body); sidebarScroll_ = area; return area;
}
