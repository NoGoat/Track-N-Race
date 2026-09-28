#include "StrategyPage.h"
#include "TyreHelpers.h"

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
QColor compoundColor(int actual, int visual) {
    const QColor color = tyreTextColor(actual, visual);
    return color.isValid() ? color : QApplication::palette().color(QPalette::Text);
}
QColor wearColor(double value) {
    if (value < 20) return QColor("#73bf69");
    if (value < 40) return QColor("#a8d436");
    if (value < 60) return QColor("#fade2a");
    if (value < 80) return QColor("#ff9830");
    return QColor("#c4162a");
}
QString barStyle(const QColor& color) {
    return QStringLiteral(
        "QProgressBar { border:0; border-radius:3px; background:palette(midlight); }"
        "QProgressBar::chunk { border-radius:3px; background:%1; }").arg(color.name());
}
QProgressBar* makeBar(double value, const QColor& color) {
    auto* bar = new QProgressBar;
    bar->setRange(0, 1000);
    bar->setValue(std::isfinite(value) ? qRound(std::clamp(value, 0.0, 100.0) * 10) : 0);
    bar->setTextVisible(false);
    bar->setFixedHeight(6);
    bar->setStyleSheet(barStyle(color));
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
// In-place setters: each touches the widget only when the value changed, so
// an unchanged field costs no relayout or repaint.
void setHtml(QLabel* label, QString text) {
    text.replace(QStringLiteral("palette(mid)"), QApplication::palette().color(QPalette::PlaceholderText).name());
    if (label->text() != text) label->setText(text);
}
void applyStyleSheet(QWidget* widget, const QString& css) {
    if (widget->styleSheet() != css) widget->setStyleSheet(css);
}
void setBar(QProgressBar* bar, double value, const QColor& color) {
    bar->setValue(std::isfinite(value) ? qRound(std::clamp(value, 0.0, 100.0) * 10) : 0);
    applyStyleSheet(bar, barStyle(color));
}
void setCell(QTableWidget* table, int row, int column, const QString& text, const QBrush& foreground, const QBrush& background) {
    auto* item = table->item(row, column);
    if (!item) {
        item = new QTableWidgetItem; item->setTextAlignment(Qt::AlignCenter);
        table->setItem(row, column, item);
    }
    if (item->text() != text) item->setText(text);
    if (item->foreground() != foreground) item->setForeground(foreground);
    if (item->background() != background) item->setBackground(background);
}
// Grows or shrinks a list of word-wrapped labels to count, at the list's end.
void syncLabels(QVBoxLayout* layout, std::vector<QLabel*>& labels, size_t count) {
    while (labels.size() > count) { delete labels.back(); labels.pop_back(); }
    while (labels.size() < count) {
        auto* label = makeLabel(QString()); label->setWordWrap(true);
        layout->addWidget(label); labels.push_back(label);
    }
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
}

StrategyPage::StrategyPage(QWidget* parent) : QWidget(parent) {
    density_ = tnr::densityFromValue(settings_.value(QStringLiteral("ui/compact/strategySummary"), "normal"));
    pageLayout_ = new QVBoxLayout(this); pageLayout_->setContentsMargins(0, 0, 0, 0); pageLayout_->setSpacing(0); rebuild();
}
void StrategyPage::update(const tnrp::StrategySnapshotRow* value, bool rebuilding) {
    // Playback publishes a snapshot nearly every tick because its clock fields
    // advance; the page shows neither, so only a visible change refreshes.
    std::string key;
    if (value) {
        tnrp::StrategySnapshotRow visible = *value;
        visible.session_time = 0.0f;
        visible.data_age_s = 0.0;
        if (glz::write_json(visible, key)) key.clear();
    }
    const bool unchanged = snapshot_.has_value() == (value != nullptr) && !key.empty() && key == renderedKey_ &&
        rebuilding == rebuilding_;
    renderedKey_ = std::move(key);
    rebuilding_ = rebuilding;
    if (value) snapshot_ = *value; else snapshot_.reset();
    if (!unchanged) refresh();
}
void StrategyPage::resetForNewSession() {
    snapshot_.reset();
    renderedKey_.clear();
    for (auto* area : {defensive_.scroll, attacking_.scroll, sidebarScroll_})
        if (area) area->verticalScrollBar()->setValue(0);
    refresh();
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
    if (event->type() == QEvent::PaletteChange && pageLayout_) rebuild();
}

void StrategyPage::rebuild() {
    // Preserve the editor (including uncommitted text) across rebuilds.
    if (minimumStopsControl_) {
        restoreMinimumStopsFocus_ = minimumStopsInput_->hasFocus();
        minimumStopsControl_->setParent(this);
        minimumStopsControl_->hide();
    }
    const auto scrollOf = [](QScrollArea* area) { return area ? area->verticalScrollBar()->value() : 0; };
    const int cy = scrollOf(defensive_.scroll), ay = scrollOf(attacking_.scroll), sy = scrollOf(sidebarScroll_);
    defensive_ = PlanView{}; attacking_ = PlanView{};
    rivals_.clear(); alerts_.clear();

    // Build the new tree hidden, fill and lay it out, then swap it in within
    // this call so the screen never shows an empty or unlaid-out frame.
    QWidget* previous = content_;
    content_ = new QWidget(this);
    content_->hide();
    auto* root = new QVBoxLayout(content_); root->setContentsMargins(0, 0, 0, 0); root->setSpacing(0);
    root->addWidget(buildHeader());
    root->addWidget(makeRule());
    nonRace_ = makeLabel(QStringLiteral("<b>Race sessions only</b><br><br>Strategy suggestions are available during Race, Race 2, and Race 3 sessions."));
    nonRace_->setAlignment(Qt::AlignCenter); nonRace_->setWordWrap(true);
    root->addWidget(nonRace_, 1);
    root->addWidget(buildBody(), 1);
    refresh();

    content_->setGeometry(rect());
    content_->show();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    defensive_.scroll->verticalScrollBar()->setValue(cy);
    attacking_.scroll->verticalScrollBar()->setValue(ay);
    sidebarScroll_->verticalScrollBar()->setValue(sy);
    pageLayout_->addWidget(content_);
    if (previous) {
        pageLayout_->removeWidget(previous);
        previous->hide();
        previous->deleteLater();
    }
}

void StrategyPage::refresh() {
    const auto* s = snapshot_ ? &*snapshot_ : nullptr;
    // As in Electron, a seek keeps the previous snapshot on screen and swaps
    // only the plan columns for the progress bar until the rebuild arrives.
    const bool nonRace = s && s->state == "non_race" && !rebuilding_;
    const bool ready = s && s->state == "ready";
    const bool plans = ready && !rebuilding_;
    refreshHeader(s);
    nonRace_->setVisible(nonRace);
    body_->setVisible(!nonRace);
    // Strategy still being calculated: an indeterminate bar in place of the plans.
    pending_->setVisible(!plans);
    defensive_.frame->setVisible(plans);
    planRule_->setVisible(plans);
    attacking_.frame->setVisible(plans);
    if (plans) {
        refreshPlan(defensive_, QStringLiteral("Defensive"), s->conservative, defensiveColor());
        refreshPlan(attacking_, QStringLiteral("Attacking"), s->aggressive, attackingColor());
    }
    refreshSidebar(ready ? s : nullptr);
}

QWidget* StrategyPage::buildHeader() {
    const bool compact = density_ == tnr::DensityMode::Compact;
    const bool spacious = density_ == tnr::DensityMode::Spacious;
    const int pad = compact ? 6 : spacious ? 20 : 12;
    const int horizontalPad = spacious ? 32 : 24;
    const QColor secondary = palette().color(QPalette::PlaceholderText);

    auto* header = new QWidget; auto* row = new QHBoxLayout(header);
    row->setContentsMargins(0, 0, 0, 0); row->setSpacing(0);
    auto addDivider = [&] {
        auto* rule = new QFrame; rule->setFrameShape(QFrame::VLine); rule->setFrameShadow(QFrame::Plain);
        row->addWidget(rule);
    };
    auto* lapCell = new QWidget; auto* lapLayout = new QVBoxLayout(lapCell);
    lapLayout->setContentsMargins(horizontalPad, pad, horizontalPad, pad); lapLayout->setSpacing(4);
    if (!compact) lapLayout->addWidget(makeLabel(QStringLiteral("<span style='color:%1'>LAP</span>").arg(secondary.name())));
    lapValue_ = makeLabel(QString()); lapLayout->addWidget(lapValue_);
    lapDistance_ = nullptr;
    if (spacious) { lapDistance_ = makeLabel(QString()); lapLayout->addWidget(lapDistance_); }
    row->addWidget(lapCell); addDivider();

    auto* tyreCell = new QWidget; auto* tyreLayout = new QHBoxLayout(tyreCell);
    tyreLayout->setContentsMargins(horizontalPad, pad, horizontalPad, pad); tyreLayout->setSpacing(compact ? 12 : spacious ? 24 : 16);
    compoundChip_ = makeLabel(QString());
    compoundChip_->setTextFormat(Qt::PlainText); compoundChip_->setContentsMargins(8, 2, 8, 2);
    tyreLayout->addWidget(compoundChip_);
    wearValue_ = makeLabel(QString());
    wearDetail_ = makeLabel(QString());
    wearBar_ = makeBar(0, wearColor(0));
    limitingTyre_ = nullptr;
    if (compact) {
        tyreLayout->addWidget(wearValue_); tyreLayout->addWidget(wearBar_, 1); tyreLayout->addWidget(wearDetail_);
    } else {
        auto* wearGroup = new QWidget; auto* wearLayout = new QVBoxLayout(wearGroup);
        wearLayout->setContentsMargins(0, 0, 0, 0); wearLayout->setSpacing(spacious ? 8 : 6);
        auto* values = new QHBoxLayout;
        values->addWidget(wearValue_); values->addStretch(); values->addWidget(wearDetail_);
        wearLayout->addLayout(values); wearLayout->addWidget(wearBar_);
        if (spacious) {
            wearBar_->setFixedHeight(10);
            limitingTyre_ = makeLabel(QString());
            limitingTyre_->setTextFormat(Qt::PlainText); wearLayout->addWidget(limitingTyre_);
        }
        tyreLayout->addWidget(wearGroup, 1);
    }
    row->addWidget(tyreCell, 1); addDivider();

    auto* cliffCell = new QWidget; auto* cliffLayout = new QBoxLayout(compact ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom, cliffCell);
    cliffLayout->setContentsMargins(horizontalPad, pad, horizontalPad, pad); cliffLayout->setSpacing(compact ? 8 : 4);
    cliffLayout->addWidget(makeLabel(QStringLiteral("<span style='color:%1'>TYRE CLIFF</span>").arg(secondary.name())));
    cliffValue_ = makeLabel(QString()); cliffLayout->addWidget(cliffValue_);
    cliffRemaining_ = nullptr;
    if (spacious) { cliffRemaining_ = makeLabel(QString()); cliffLayout->addWidget(cliffRemaining_); }
    row->addWidget(cliffCell);
    return header;
}

void StrategyPage::refreshHeader(const tnrp::StrategySnapshotRow* s) {
    const bool ready = s && s->state == "ready";
    const bool compact = density_ == tnr::DensityMode::Compact;
    const bool spacious = density_ == tnr::DensityMode::Spacious;
    const QColor secondary = palette().color(QPalette::PlaceholderText);
    const QString lap = s && s->lap_num > 0 ? QString::number(s->lap_num) : QStringLiteral("—");
    const QString total = s && s->total_laps > 0 ? QString::number(s->total_laps) : QStringLiteral("—");
    const int wear = ready ? qRound(s->average_wear) : 0;
    const QColor wearTint = ready ? wearColor(wear) : secondary;
    const QColor cliffTint = !ready ? secondary : s->laps_until_cliff <= 5 ? QColor("#c4162a")
        : s->laps_until_cliff <= 10 ? QColor("#fade2a") : palette().color(QPalette::Text);

    setHtml(lapValue_, QStringLiteral("<b style='font-size:%1px'>%2</b> <span style='color:%3'>/ %4</span>")
        .arg(compact ? 18 : spacious ? 36 : 30).arg(lap, secondary.name(), total));
    if (lapDistance_) {
        const bool known = s && s->lap_num > 0 && s->total_laps > 0;
        lapDistance_->setVisible(known);
        if (known) setHtml(lapDistance_, QStringLiteral("%1% distance").arg(qRound(100.0 * s->lap_num / s->total_laps)));
    }

    const QColor compound = compoundColor(s ? s->current_actual_compound : 0,
                                          s ? s->current_visual_compound : 0);
    const QString chip = ready ? QString::fromStdString(s->current_compound_name) : QStringLiteral("—");
    if (compoundChip_->text() != chip) compoundChip_->setText(chip);
    applyStyleSheet(compoundChip_, QStringLiteral("color:%1; border:1px solid %1; border-radius:3px; font-weight:bold;").arg(compound.name()));
    setHtml(wearValue_, ready ? QStringLiteral("%1%").arg(wear) : QStringLiteral("—"));
    applyStyleSheet(wearValue_, QStringLiteral("color:%1; font-weight:bold; font-size:%2px;")
        .arg(wearTint.name()).arg(compact ? 14 : spacious ? 24 : 18));
    setHtml(wearDetail_, ready ? QStringLiteral("%1L%2 · %3%/L")
        .arg(s->current_tyre_age_laps).arg(spacious ? QStringLiteral(" age") : QString()).arg(s->wear_per_lap, 0, 'f', 1)
        : QStringLiteral("—"));
    applyStyleSheet(wearDetail_, QStringLiteral("color:%1; font-size:%2px;").arg(secondary.name()).arg(spacious ? 12 : 10));
    setBar(wearBar_, wear, wearColor(wear));
    if (limitingTyre_) {
        const bool known = ready && !s->limiting_corner.empty();
        limitingTyre_->setVisible(known);
        if (known) {
            const QString text = QStringLiteral("%1 is limiting tyre").arg(QString::fromStdString(s->limiting_corner));
            if (limitingTyre_->text() != text) limitingTyre_->setText(text);
        }
    }

    setHtml(cliffValue_, QStringLiteral("<b style='color:%1; font-size:%2px'>%3</b>%4")
        .arg(cliffTint.name()).arg(compact ? 14 : spacious ? 24 : 18)
        .arg(ready ? QStringLiteral("Lap %1").arg(s->cliff_lap) : QStringLiteral("—"))
        .arg(ready ? QStringLiteral(" <span style='color:%1'>+%2%3</span>").arg(secondary.name()).arg(s->laps_until_cliff)
            .arg(spacious ? QStringLiteral("L") : QString()) : QString()));
    if (cliffRemaining_) {
        cliffRemaining_->setVisible(ready);
        if (ready) setHtml(cliffRemaining_, QStringLiteral("%1 laps remaining").arg(s->laps_until_cliff));
    }
}

QWidget* StrategyPage::buildBody() {
    body_ = new QWidget; auto* cols = new QHBoxLayout(body_);
    cols->setContentsMargins(0, 0, 0, 0); cols->setSpacing(0);
    pending_ = new QWidget; auto* centre = new QVBoxLayout(pending_);
    auto* bar = new QProgressBar; bar->setRange(0, 0); bar->setTextVisible(false);
    bar->setFixedSize(128, 4);
    centre->addStretch(); centre->addWidget(bar, 0, Qt::AlignCenter); centre->addStretch();
    cols->addWidget(pending_, 6);
    cols->addWidget(buildPlan(defensive_), 3);
    planRule_ = makeVerticalRule();
    cols->addWidget(planRule_);
    cols->addWidget(buildPlan(attacking_), 3);
    cols->addWidget(makeVerticalRule());
    cols->addWidget(buildSidebar(), 2);
    return body_;
}

QWidget* StrategyPage::buildPlan(PlanView& view) {
    auto* frame = new QFrame; frame->setFrameShape(QFrame::NoFrame);
    auto* outer = new QVBoxLayout(frame); outer->setContentsMargins(0, 0, 0, 0); outer->setSpacing(0);
    view.frame = frame;
    view.heading = makeLabel(QString());
    view.heading->setContentsMargins(12, 10, 12, 10);
    view.heading->setWordWrap(true);
    outer->addWidget(view.heading);
    outer->addWidget(makeRule());
    view.scroll = new QScrollArea; view.scroll->setWidgetResizable(true); view.scroll->setFrameShape(QFrame::NoFrame);
    auto* body = new QWidget;
    view.stints = new QVBoxLayout(body); view.stints->setContentsMargins(0, 0, 0, 0); view.stints->setSpacing(0);
    view.stints->addStretch();
    view.scroll->setWidget(body); outer->addWidget(view.scroll, 1);
    return frame;
}

void StrategyPage::refreshPlan(PlanView& view, const QString& title, const tnrp::StrategyPlan& plan, const QColor& accent) {
    const QString target = plan.target_idx >= 0
        ? QStringLiteral("%1 %2").arg(plan.mode == "attacking" ? QStringLiteral("Chasing") : QStringLiteral("Covering"),
            QString::fromStdString(plan.target_name).toHtmlEscaped())
        : QStringLiteral("Tyre-life baseline");
    const QString legality = plan.legal ? QString() : QStringLiteral(" · <span style='color:#c4162a'>No legal set path</span>");
    setHtml(view.heading, QStringLiteral("<b>%1</b>  ·  %2 stop%3%4<br><span style='color:palette(mid)'>%5 · %6% confidence%7</span>")
        .arg(title).arg(plan.stops).arg(plan.stops == 1 ? QString() : QStringLiteral("s"))
        .arg(legality, target).arg(qRound(plan.confidence * 100))
        .arg(plan.requires_compound_change ? QStringLiteral(" · compound change required") : QString()));
    applyStyleSheet(view.heading, QStringLiteral("border-left:4px solid %1;").arg(accent.name()));

    // Stints change only at the end of the list, so cards are reused by index.
    while (view.items.size() > plan.stints.size()) {
        delete view.items.back().rule; delete view.items.back().card;
        view.items.pop_back();
    }
    while (view.items.size() < plan.stints.size()) {
        StintView stint;
        const int at = view.stints->count() - 1;   // before the trailing stretch
        stint.rule = makeRule();
        view.stints->insertWidget(at, stint.rule);
        stint.rule->setVisible(!view.items.empty());
        auto* card = new QFrame; card->setFrameShape(QFrame::NoFrame);
        auto* cv = new QVBoxLayout(card); cv->setContentsMargins(0, 0, 0, 0); cv->setSpacing(0);
        stint.card = card;
        stint.title = makeLabel(QString()); stint.title->setContentsMargins(12, 8, 12, 0); cv->addWidget(stint.title);
        stint.counts = makeLabel(QString()); stint.counts->setContentsMargins(12, 4, 12, 8); cv->addWidget(stint.counts);
        auto* table = new QTableWidget(0, 6);
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
        stint.table = table; cv->addWidget(table);
        view.stints->insertWidget(at + 1, card);
        view.items.push_back(stint);
    }

    const QBrush pending = palette().color(QPalette::PlaceholderText);
    const QBrush slower = QColor("#c4162a"), faster = QColor("#73bf69"), stopLap = QColor(115, 191, 105, 38);
    for (size_t i = 0; i < plan.stints.size(); ++i) {
        const auto& stint = plan.stints[i];
        const auto& card = view.items[i];
        setHtml(card.title, QStringLiteral("<b><span style='color:%1'>●</span> Stint %2 · %3</b>  L%4–%5")
            .arg(compoundColor(stint.actual_compound, stint.visual_compound).name()).arg(stint.stint_number)
            .arg(QString::fromStdString(stint.compound_name).toHtmlEscaped()).arg(stint.start_lap).arg(stint.end_lap));
        setHtml(card.counts, QStringLiteral("Expected <b>%1</b> · Actual <b>%2</b>").arg(stint.expected_laps).arg(stint.actual_laps));
        auto* table = card.table;
        table->setVisible(!stint.rows.empty());
        if (stint.rows.empty()) continue;
        const int rows = int(stint.rows.size());
        if (table->rowCount() != rows) table->setRowCount(rows);
        // The plan owns scrolling, including long future stints, just as in Electron.
        const int rowHeight = table->verticalHeader()->defaultSectionSize();
        const int height = rowHeight + 4 + rowHeight * rows + 2 * table->frameWidth();
        if (table->minimumHeight() != height || table->maximumHeight() != height) table->setFixedHeight(height);
        for (int r = 0; r < rows; ++r) {
            const auto& x = stint.rows[size_t(r)];
            const QStringList values{QString::number(x.lap_num), timeText(x.required_ms),
                x.has_actual ? timeText(x.actual_ms) : QStringLiteral("—"),
                x.has_actual ? deltaText(x.delta_lap_ms) : QStringLiteral("—"),
                x.has_actual ? deltaText(x.delta_stint_ms) : QStringLiteral("—"),
                x.has_actual ? deltaText(x.delta_total_ms) : QStringLiteral("—")};
            const QBrush background = !stint.is_last && x.lap_num == stint.end_lap ? stopLap : QBrush();
            for (int c = 0; c < values.size(); ++c) {
                QBrush foreground;
                if (c >= 3) {
                    const double delta = c == 3 ? x.delta_lap_ms : c == 4 ? x.delta_stint_ms : x.delta_total_ms;
                    foreground = x.has_actual ? (delta > 0 ? slower : faster) : pending;
                }
                setCell(table, r, c, values[c], foreground, background);
            }
        }
    }
}

QWidget* StrategyPage::buildSidebar() {
    auto* column = new QWidget;
    auto* layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    sidebarScroll_ = new QScrollArea;
    sidebarScroll_->setWidgetResizable(true); sidebarScroll_->setFrameShape(QFrame::NoFrame);
    sidebarScroll_->setBackgroundRole(QPalette::Window);
    sidebarScroll_->viewport()->setBackgroundRole(QPalette::Window);
    auto* body = new QWidget; auto* v = new QVBoxLayout(body); v->setContentsMargins(10, 10, 10, 10); v->setSpacing(8);
    // Optional sections are containers shown or hidden as the snapshot changes.
    const auto section = [v](QWidget*& out) {
        out = new QWidget; auto* l = new QVBoxLayout(out);
        l->setContentsMargins(0, 0, 0, 0); l->setSpacing(8);
        v->addWidget(out); return l;
    };
    const auto wrapped = [] { auto* label = makeLabel(QString()); label->setWordWrap(true); return label; };

    auto* l = section(neutralisationSection_);
    neutralisation_ = wrapped(); l->addWidget(neutralisation_); l->addWidget(makeRule());

    l = section(callSection_);
    call_ = wrapped();
    l->addWidget(makeLabel(QStringLiteral("<b>RACE CALL</b>"))); l->addWidget(call_); l->addWidget(makeRule());

    pitWindow_ = wrapped(); v->addWidget(pitWindow_); v->addWidget(makeRule());

    l = section(weatherSection_);
    weather_ = makeLabel(QString()); l->addWidget(weather_); l->addWidget(makeRule());

    l = section(rivalsSection_);
    l->addWidget(makeLabel(QStringLiteral("<b>RACE BATTLE</b>")));
    rivalsList_ = new QVBoxLayout; rivalsList_->setContentsMargins(0, 0, 0, 0); rivalsList_->setSpacing(8);
    l->addLayout(rivalsList_); l->addWidget(makeRule());

    auto* tyres = new QWidget; auto* tl = new QVBoxLayout(tyres);
    tl->setContentsMargins(0, 0, 0, 0);
    tyreCondition_ = makeLabel(QString()); tl->addWidget(tyreCondition_);
    auto* grid = new QGridLayout; grid->setHorizontalSpacing(16); grid->setVerticalSpacing(8);
    const std::array<const char*, 4> names{{"FL", "FR", "RL", "RR"}};
    for (size_t i = 0; i < names.size(); ++i) {
        auto* cell = new QVBoxLayout;
        auto* values = new QHBoxLayout;
        corners_[i].value = makeLabel(QString());
        values->addWidget(makeLabel(QString::fromLatin1(names[i]))); values->addStretch(); values->addWidget(corners_[i].value);
        corners_[i].bar = makeBar(0, wearColor(0));
        cell->addLayout(values); cell->addWidget(corners_[i].bar);
        grid->addLayout(cell, int(i / 2), int(i % 2));
    }
    tl->addLayout(grid); v->addWidget(tyres);

    l = section(alertsSection_);
    l->addWidget(makeRule()); l->addWidget(makeLabel(QStringLiteral("<b>TYRE ALERTS</b>")));
    alertsList_ = new QVBoxLayout; alertsList_->setContentsMargins(0, 0, 0, 0); alertsList_->setSpacing(8);
    l->addLayout(alertsList_);

    v->addStretch(); sidebarScroll_->setWidget(body);
    layout->addWidget(sidebarScroll_, 1);
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
    return column;
}

// s is null while the strategy is still being calculated: placeholders only.
void StrategyPage::refreshSidebar(const tnrp::StrategySnapshotRow* s) {
    neutralisationSection_->setVisible(s && s->neutralisation);
    if (s && s->neutralisation) {
        const auto& n = *s->neutralisation;
        setHtml(neutralisation_, QStringLiteral("<b>%1 DECISION</b><br><span style='font-size:18px'><b>%2</b></span> · P%3 → P%4<br>"
            "<span style='color:palette(mid)'>%5</span><br><br>Box now %6 s · wait to L%7 / %8 s")
            .arg(n.kind == "safety_car" ? QStringLiteral("SAFETY CAR") : QStringLiteral("VSC"),
                n.recommendation == "box" ? QStringLiteral("BOX NOW") : QStringLiteral("STAY OUT"))
            .arg(n.current_position).arg(n.recommendation == "box" ? n.projected_box_position : n.projected_stay_position)
            .arg(words(n.reason).toHtmlEscaped(), deltaText(n.box_now_cost_ms))
            .arg(n.box_later_lap).arg(deltaText(n.box_later_cost_ms)));
        applyStyleSheet(neutralisation_, QStringLiteral("border-left:4px solid %1; padding:8px;")
            .arg(n.recommendation == "box" ? QStringLiteral("#73bf69") : QStringLiteral("#fade2a")));
    }

    callSection_->setVisible(s && s->call);
    if (s && s->call) {
        const QString color = s->call->kind == "undercut" || s->call->kind == "cover" ? QStringLiteral("#fade2a") : QStringLiteral("#73bf69");
        setHtml(call_, QStringLiteral("<b style='color:%1'>%2</b> <b>%3</b> · %4s<br><span style='color:palette(mid)'>%5%6</span>")
            .arg(color, QString::fromStdString(s->call->kind).toUpper().toHtmlEscaped(), QString::fromStdString(s->call->target_name).toHtmlEscaped())
            .arg(s->call->gap_ms / 1000.0, 0, 'f', 1)
            .arg(words(s->call->reason).toHtmlEscaped())
            .arg(s->call->crossover_laps ? QStringLiteral(" · %1 lap%2").arg(*s->call->crossover_laps)
                .arg(*s->call->crossover_laps == 1 ? QString() : QStringLiteral("s")) : QString()));
    }

    const QString pitTemplate = QStringLiteral("<b>NEXT PIT WINDOW</b><br><br>"
        "<span style='color:%5'><b>DEFEND %1</b></span> · %2<br>"
        "<span style='color:%6'><b>ATTACK %3</b></span> · %4");
    if (s) {
        auto nextStop = [&](const tnrp::StrategyPlan& plan) -> std::optional<int> {
            for (const auto& stint : plan.stints)
                if (!stint.is_last && stint.end_lap >= s->lap_num) return stint.end_lap;
            return std::nullopt;
        };
        auto stopText = [&](const std::optional<int>& lap) {
            if (!lap) return QStringLiteral("FLAG");
            return *lap <= s->lap_num ? QStringLiteral("NOW") : QStringLiteral("L%1").arg(*lap);
        };
        const QString defensiveTarget = s->conservative.target_idx >= 0
            ? QStringLiteral("Cover %1").arg(QString::fromStdString(s->conservative.target_name).toHtmlEscaped())
            : QStringLiteral("Tyre life");
        const QString attackingTarget = s->aggressive.target_idx >= 0
            ? QStringLiteral("Chase %1").arg(QString::fromStdString(s->aggressive.target_name).toHtmlEscaped())
            : QStringLiteral("Tyre life");
        setHtml(pitWindow_, pitTemplate.arg(stopText(nextStop(s->conservative)), defensiveTarget,
            stopText(nextStop(s->aggressive)), attackingTarget, defensiveColor().name(), attackingColor().name()));
    } else {
        const QString dash = QStringLiteral("—");
        setHtml(pitWindow_, pitTemplate.arg(dash, dash, dash, dash, defensiveColor().name(), attackingColor().name()));
    }

    const bool weatherKnown = s && s->weather_strategy &&
        (s->weather_strategy->crossover_lap > 0 || s->weather_strategy->lap_delta_ms);
    weatherSection_->setVisible(!s || weatherKnown);
    if (weatherKnown) {
        const auto& weather = *s->weather_strategy;
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
        setHtml(weather_, QStringLiteral("<b>WEATHER WINDOW</b><br>%1%2<br>"
            "<span style='color:palette(mid)'>%3%4</span>")
            .arg(words(weather.recommendation).toHtmlEscaped(),
                 weather.crossover_lap > 0 ? QStringLiteral(" · L%1").arg(weather.crossover_lap) : QString(),
                 when.toHtmlEscaped(),
                 pace.isEmpty() ? QString() : QStringLiteral("<br>") + pace.toHtmlEscaped()));
    } else if (!s) {
        setHtml(weather_, QStringLiteral("<b>WEATHER WINDOW</b><br>—<br><span style='color:palette(mid)'>—</span>"));
    }

    const size_t rivalCount = s ? s->rivals.size() : 0;
    rivalsSection_->setVisible(rivalCount > 0);
    syncLabels(rivalsList_, rivals_, rivalCount);
    for (size_t i = 0; i < rivalCount; ++i) {
        const auto& r = s->rivals[i];
        const QString pace = std::abs(r.pace_delta_ms) > 50.0
            ? QStringLiteral("%1 s/L %2").arg(std::abs(r.pace_delta_ms) / 1000.0, 0, 'f', 2)
                .arg(r.pace_delta_ms < 0 ? QStringLiteral("faster") : QStringLiteral("slower"))
            : QStringLiteral("matched pace");
        const QString context = r.last_pit_lap >= s->lap_num - 1
            ? QStringLiteral("stopped L%1").arg(r.last_pit_lap) : pace;
        setHtml(rivals_[i], QStringLiteral("P%1  <b>%2</b> · %3%4 s<br>"
            "<span style='color:palette(mid)'>%5 · %6L tyres · %7</span>")
            .arg(r.position).arg(QString::fromStdString(r.name).toHtmlEscaped())
            .arg(r.direction == "ahead" ? QStringLiteral("−") : QStringLiteral("+"))
            .arg(r.gap_ms / 1000.0, 0, 'f', 1)
            .arg(r.direction == "ahead" ? QStringLiteral("To catch") : QStringLiteral("To cover"))
            .arg(r.tyre_age_laps).arg(context));
    }

    setHtml(tyreCondition_, QStringLiteral("<b>TYRE CONDITION</b><br><span style='color:palette(mid)'>%1</span>")
        .arg(s ? QStringLiteral("%1 limits · cliff L%2").arg(QString::fromStdString(s->limiting_corner).toHtmlEscaped())
            .arg(s->cliff_lap) : QStringLiteral("—")));
    const std::array<double, 4> wear{{s ? s->wear_fl : 0, s ? s->wear_fr : 0, s ? s->wear_rl : 0, s ? s->wear_rr : 0}};
    for (size_t i = 0; i < corners_.size(); ++i) {
        setHtml(corners_[i].value, s ? QStringLiteral("<b style='color:%1'>%2%</b>").arg(wearColor(wear[i]).name()).arg(qRound(wear[i]))
            : QStringLiteral("—"));
        setBar(corners_[i].bar, wear[i], wearColor(wear[i]));
    }

    const size_t alertCount = s ? s->wear_warnings.size() : 0;
    alertsSection_->setVisible(alertCount > 0);
    syncLabels(alertsList_, alerts_, alertCount);
    for (size_t i = 0; i < alertCount; ++i) {
        const auto& warning = s->wear_warnings[i];
        const auto [scope, detail] = wearWarningParts(warning.text);
        const QString color = warning.severity == "danger" ? "#c4162a" : warning.severity == "warning" ? "#ff9830" : "#fade2a";
        setHtml(alerts_[i], QStringLiteral("<span style='color:%1'>⚠ <b>%2</b></span> <span style='color:palette(mid)'>%3</span>")
            .arg(color, scope.toHtmlEscaped(), detail.toHtmlEscaped()));
    }
}
