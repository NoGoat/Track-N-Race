#include "AnalysisMapView.h"
#include "AnalysisInputCard.h"

#include "../AnalyzeChart.h"
#include "../CardColors.h"
#include "../TrackMapWidget.h"
#include "../../IconUtils.h"

#include <QAction>
#include <QBoxLayout>
#include <QComboBox>
#include <QEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFrame>
#include <QHideEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

#include <tnrp/AnalysisMap.h>
#include <tnrp/AnalysisMapPaint.h>

// A layer's key in the map's bottom-right corner: text, colour ramps and
// swatches in one row.
class AnalysisMapLegend : public QWidget {
public:
    struct Item {
        enum class Kind { Text, Strong, Ramp, Swatch } kind = Kind::Text;
        QString text;
        QColor color;
        QVector<QColor> ramp;
    };

    explicit AnalysisMapLegend(QWidget* parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        QFont legendFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        legendFont.setPointSizeF(std::max(7.0, legendFont.pointSizeF() * 0.85));
        setFont(legendFont);
        hide();
    }

    void setItems(QVector<Item> items) {
        items_ = std::move(items);
        setVisible(!items_.isEmpty());
        resize(sizeHint());
        update();
    }

    QSize sizeHint() const override {
        const QFontMetrics fm(font());
        int width = 0;
        for (const Item& item : items_) {
            if (width > 0) width += kGap;
            width += itemWidth(item, fm);
        }
        return {width, fm.height() + 2};
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QFontMetrics fm(font());
        const double mid = height() / 2.0;
        int x = 0;
        for (const Item& item : items_) {
            const int w = itemWidth(item, fm);
            switch (item.kind) {
            case Item::Kind::Text:
            case Item::Kind::Strong:
                p.setPen(palette().color(item.kind == Item::Kind::Strong ? QPalette::Text
                                                                         : QPalette::PlaceholderText));
                p.drawText(QRect(x, 0, w, height()), Qt::AlignVCenter | Qt::AlignLeft, item.text);
                break;
            case Item::Kind::Ramp: {
                QLinearGradient gradient(x, 0, x + kRampW, 0);
                for (int i = 0; i < item.ramp.size(); ++i)
                    gradient.setColorAt(item.ramp.size() > 1 ? double(i) / (item.ramp.size() - 1) : 0.0,
                                        item.ramp[i]);
                p.setPen(Qt::NoPen);
                p.setBrush(gradient);
                p.drawRoundedRect(QRectF(x, mid - 3, kRampW, 6), 3, 3);
                break;
            }
            case Item::Kind::Swatch:
                p.setPen(Qt::NoPen);
                p.setBrush(item.color);
                p.drawRoundedRect(QRectF(x, mid - 3, kSwatchW, 6), 3, 3);
                if (!item.text.isEmpty()) {
                    p.setPen(palette().color(QPalette::PlaceholderText));
                    p.drawText(QRect(x + kSwatchW + 4, 0, w - kSwatchW - 4, height()),
                               Qt::AlignVCenter | Qt::AlignLeft, item.text);
                }
                break;
            }
            x += w + kGap;
        }
    }

private:
    static constexpr int kGap = 6;
    static constexpr int kRampW = 64;
    static constexpr int kSwatchW = 12;

    static int itemWidth(const Item& item, const QFontMetrics& fm) {
        switch (item.kind) {
        case Item::Kind::Ramp: return kRampW;
        case Item::Kind::Swatch:
            return kSwatchW + (item.text.isEmpty() ? 0 : 4 + fm.horizontalAdvance(item.text));
        default: return fm.horizontalAdvance(item.text);
        }
    }

    QVector<Item> items_;
};

namespace {

using LegendItem = AnalysisMapLegend::Item;

// How much one press of Zoom In or Zoom Out changes the map's zoom.
constexpr double kMapZoomStep = 1.5;

QIcon themed(const QWidget* widget, const char* name, QStyle::StandardPixmap fallback) {
    return adaptThemeIcon(QIcon::fromTheme(QString::fromLatin1(name)),
                          widget->palette().color(QPalette::WindowText),
                          widget->style()->standardIcon(fallback));
}

QString clockText(double seconds) {
    const int ms = qMax(0, qRound(seconds * 1000));
    return QStringLiteral("%1:%2.%3")
        .arg(ms / 60000)
        .arg((ms / 1000) % 60, 2, 10, QChar('0'))
        .arg((ms % 1000) / 100);
}

bool positionAt(const LapBlock& lap, double elapsed, double& x, double& z) {
    const auto& points = lap.positions;
    if (points.isEmpty()) return false;
    const double duration = qMax(0.0, double(lap.endSessionTime - lap.startSessionTime));
    const float target = static_cast<float>(lap.startSessionTime + qBound(0.0, elapsed, duration));
    const auto it = std::lower_bound(points.cbegin(), points.cend(), target,
                                     [](const LapPositionSample& p, float t) { return p.t < t; });
    if (it == points.cbegin()) { x = it->x; z = it->z; return true; }
    if (it == points.cend()) { x = points.last().x; z = points.last().z; return true; }
    const LapPositionSample& a = *(it - 1);
    const LapPositionSample& b = *it;
    const double span = b.t - a.t;
    const double ratio = span > 0 ? (target - a.t) / span : 0.0;
    x = a.x + (b.x - a.x) * ratio;
    z = a.z + (b.z - a.z) * ratio;
    return true;
}

QVector<QColor> rampColors(const tnrp::AnalysisMapRamp& ramp) {
    QVector<QColor> out;
    out.reserve(qsizetype(ramp.colors.size()));
    for (const std::string& color : ramp.colors) out.push_back(QColor(QString::fromStdString(color)));
    return out;
}

QColor color(const std::string& hex) { return QColor(QString::fromStdString(hex)); }

QString formatKw(double kw) {
    return std::floor(kw) == kw ? QStringLiteral("%1 kW").arg(qint64(kw))
                                : QStringLiteral("%1 kW").arg(kw, 0, 'f', 1);
}

QString metres(double m) { return QLocale().toString(qRound64(m)) + QStringLiteral(" m"); }
QString metresOrDash(double m) { return m < 0 ? QStringLiteral("—") : metres(m); }
QString kph(double v) { return QStringLiteral("%1 km/h").arg(qRound(v)); }
QString sideText(int side) { return side < 0 ? QStringLiteral(" · L") : side > 0 ? QStringLiteral(" · R") : QString(); }

// ── Braking zones ───────────────────────────────────────────────────────────

// Laps this close through a braking zone count as level.
constexpr double kBrakingLevelS = 0.01;
// A zone's braking half is drawn at this opacity against its acceleration
// half; a single lap's zone splits at its slowest point when each half is at
// least kPhaseMinM long.
constexpr double kBrakingHalfOpacity = 0.45;
constexpr double kPhaseMinM = 30.0;
// Apexes this close on two laps are the same corner.
constexpr double kCornerMatchM = 80.0;
// The stretch either side of a corner's apex that its caret on the Speed map
// is placed from and hovered along.
constexpr double kApexMarkM = 6.0;

// Which half of a braking zone: up to the slowest point, or after it.
enum class Phase { Whole, Braking, Acceleration };

struct Labels {
    QString current;
    QString comparison;
    QColor currentColor;
    QColor comparisonColor;
};

// Words one of libtnrp's braking zone observations with the laps' names.
QString observationText(const tnrp::BrakingObservation& o, const Labels& labels) {
    const QString lap = o.lap == 0 ? labels.current : labels.comparison;
    const QString other = o.lap == 0 ? labels.comparison : labels.current;
    const std::string& k = o.kind;
    if (k == "quicker_despite_coast")
        return QStringLiteral("%1 is %2 s quicker despite lifting %3 s earlier")
            .arg(lap).arg(o.value, 0, 'f', 3).arg(o.other, 0, 'f', 1);
    if (k == "apex_speed")
        return QStringLiteral("%1 carries %2 km/h more through the apex (%3 vs %4)")
            .arg(lap).arg(qRound(o.value - o.other)).arg(qRound(o.value)).arg(qRound(o.other));
    if (k == "later_but_slower")
        return QStringLiteral("%1 brakes %2 m later but is %3 s slower")
            .arg(lap).arg(qRound(o.value)).arg(o.other, 0, 'f', 3);
    if (k == "more_harvest")
        return QStringLiteral("%1 harvests %2 kJ more than %3").arg(lap).arg(qRound(o.value - o.other)).arg(other);
    if (k == "low_battery")
        return QStringLiteral("%1 starts the zone with a near-empty battery (%2 MJ)").arg(lap).arg(o.value, 0, 'f', 2);
    if (k == "wheelspin")
        return QStringLiteral("%1 spins the rears on exit (%2% for %3 s)")
            .arg(lap).arg(qRound(o.value)).arg(o.other, 0, 'f', 1);
    if (k == "earlier_throttle_spin")
        return QStringLiteral("%1 is back on throttle %2 m earlier but spins the rears (%3%)")
            .arg(lap).arg(qRound(o.value)).arg(qRound(o.other));
    if (k == "earlier_throttle")
        return QStringLiteral("%1 is back on throttle %2 m earlier than %3").arg(lap).arg(qRound(o.value)).arg(other);
    if (k == "faster_to_full")
        return QStringLiteral("%1 is flat out %2 m sooner than %3").arg(lap).arg(qRound(o.value)).arg(other);
    if (k == "lift_only")
        return QStringLiteral("%1 takes it on a lift where %2 brakes").arg(lap, other);
    if (k == "tighter_track_limits")
        return QStringLiteral("%1 runs %2 m further inside at the apex but breaks track limits")
            .arg(lap).arg(o.value, 0, 'f', 1);
    if (k == "straighter_line") {
        const QString line = QStringLiteral("%1 runs a wider arc, %2 m further inside at the apex")
            .arg(lap).arg(o.value, 0, 'f', 1);
        if (o.other > 0.0f) return line + QStringLiteral(", and is %1 s quicker").arg(o.other, 0, 'f', 3);
        if (o.other < 0.0f) return line + QStringLiteral(", but is %1 s slower").arg(-o.other, 0, 'f', 3);
        return line;
    }
    if (k == "tighter_helped")
        return QStringLiteral("%1 takes a %2 m tighter line and is %3 s quicker")
            .arg(lap).arg(o.value, 0, 'f', 1).arg(o.other, 0, 'f', 3);
    if (k == "tighter_no_gain")
        return QStringLiteral("%1's %2 m tighter line gains no time").arg(lap).arg(o.value, 0, 'f', 1);
    if (k == "tighter_lost_speed")
        return QStringLiteral("%1 takes a %2 m tighter line but loses %3 km/h through the apex")
            .arg(lap).arg(o.value, 0, 'f', 1).arg(qRound(o.other));
    if (k == "tighter_wheelspin")
        return QStringLiteral("%1 takes a %2 m tighter line but spins the rears on exit (%3%)")
            .arg(lap).arg(o.value, 0, 'f', 1).arg(qRound(o.other));
    if (k == "tighter_slower_exit")
        return QStringLiteral("%1 takes a %2 m tighter line but is on the power %3 m later")
            .arg(lap).arg(o.value, 0, 'f', 1).arg(qRound(o.other));
    if (k == "tighter_slower")
        return QStringLiteral("%1 takes a %2 m tighter line but is %3 s slower")
            .arg(lap).arg(o.value, 0, 'f', 1).arg(o.other, 0, 'f', 3);
    if (k == "track_limits")
        return QStringLiteral("%1 breaks track limits here").arg(lap);
    return {};
}

bool exitObservation(const std::string& kind) {
    return kind == "wheelspin" || kind == "earlier_throttle_spin" || kind == "earlier_throttle" ||
           kind == "faster_to_full" || kind == "tighter_wheelspin" || kind == "tighter_slower_exit";
}

// Two laps' corners lined up: each current-lap apex with the comparison lap's
// nearest within kCornerMatchM, and any left over on their own, in lap order.
using ApexPair = std::pair<const tnrp::CornerApex*, const tnrp::CornerApex*>;
std::vector<ApexPair> alignCorners(const std::vector<tnrp::CornerApex>& a,
                                   const std::vector<tnrp::CornerApex>& b) {
    std::vector<bool> taken(b.size(), false);
    std::vector<ApexPair> rows;
    for (const tnrp::CornerApex& apex : a) {
        int best = -1;
        double bestGap = kCornerMatchM;
        for (size_t i = 0; i < b.size(); ++i) {
            const double gap = std::abs(b[i].distanceM - apex.distanceM);
            if (!taken[i] && gap <= bestGap) { best = int(i); bestGap = gap; }
        }
        if (best < 0) rows.push_back({&apex, nullptr});
        else {
            taken[size_t(best)] = true;
            rows.push_back({&apex, &b[size_t(best)]});
        }
    }
    for (size_t i = 0; i < b.size(); ++i)
        if (!taken[i]) rows.push_back({nullptr, &b[i]});
    std::sort(rows.begin(), rows.end(), [](const ApexPair& x, const ApexPair& y) {
        return (x.first ? x.first : x.second)->distanceM < (y.first ? y.first : y.second)->distanceM;
    });
    return rows;
}

QString brakeAtText(const tnrp::BrakingPoint& point) {
    return point.liftOnly ? QStringLiteral("lift ") + metres(point.distanceM) : metres(point.distanceM);
}
QString coastText(const tnrp::BrakingPoint& point) {
    return point.coastS > 0 ? QStringLiteral("%1 s · %2 m").arg(point.coastS, 0, 'f', 1).arg(qRound(point.coastM))
                            : QStringLiteral("none");
}
// A lap's line against the other's at a corner's apex, named on the lap further inside.
QString lineText(float gapM) {
    return gapM == 0.0f ? QStringLiteral("level")
        : gapM > 0.0f ? QStringLiteral("%1 m inside").arg(gapM, 0, 'f', 1) : QStringLiteral("—");
}
QString spinText(const tnrp::BrakingPoint& point) {
    return point.wheelspin ? QStringLiteral("%1% · %2 s").arg(qRound(point.wheelspinPct)).arg(point.wheelspinS, 0, 'f', 1)
                           : QStringLiteral("none");
}

// Which lap did better: 0 current, 1 comparison, -1 neither.
int pick(double a, double b, bool higher, double margin = 0) {
    if (std::abs(a - b) <= margin) return -1;
    return (a > b) == higher ? 0 : 1;
}

// The better corner exit point: the earlier one when neither lap spun, the
// clean lap when only one did, and none when both did.
int exitBetter(double a, double b, bool aSpun, bool bSpun) {
    if (a < 0 || b < 0 || (aSpun && bSpun)) return -1;
    if (aSpun != bSpun) return aSpun ? 1 : 0;
    return std::abs(a - b) < 0.5 ? -1 : a < b ? 0 : 1;
}

// One half of a braking zone on hover, head to head: a row per measure with
// each lap's value either side and the better one in bold in its lap's
// colour, then libtnrp's observations. One lap on its own shows its column.
QString brakingCard(int zone, Phase phase, double fromM, double splitM, double toM,
                    const tnrp::BrakingPoint* current, const tnrp::BrakingPoint* comparison,
                    const tnrp::BrakingPointPair* pair, const Labels& labels, const QPalette& palette) {
    const bool braking = phase != Phase::Acceleration;
    const bool acceleration = phase != Phase::Braking;
    const bool both = current && comparison;
    double times[2] = {-1, -1};
    if (pair) {
        if (phase == Phase::Braking) { times[0] = pair->currentBrakingS; times[1] = pair->comparisonBrakingS; }
        else if (phase == Phase::Acceleration) {
            times[0] = pair->currentAccelerationS;
            times[1] = pair->comparisonAccelerationS;
        } else { times[0] = pair->currentTimeS; times[1] = pair->comparisonTimeS; }
    }
    const bool timed = pair && times[0] >= 0 && times[1] >= 0;

    struct Row {
        QString measure;
        std::optional<QString> a;
        std::optional<QString> b;
        int better = -1;
        bool warnA = false;
        bool warnB = false;
    };
    const auto opt = [](bool has, const QString& text) { return has ? std::optional<QString>(text) : std::nullopt; };
    const bool sameBrake = both && (!pair || pair->sameBrakingPoint);
    std::vector<ApexPair> corners;
    for (const ApexPair& row : alignCorners(current ? current->apexes : std::vector<tnrp::CornerApex>{},
                                            comparison ? comparison->apexes : std::vector<tnrp::CornerApex>{})) {
        const double at = (row.first ? row.first : row.second)->distanceM;
        if (splitM < 0 || (braking && at <= splitM) || (acceleration && at > splitM)) corners.push_back(row);
    }
    std::vector<Row> rows;
    if (braking) {
        rows.push_back({QStringLiteral("Brakes at"), opt(current, current ? brakeAtText(*current) : QString()),
                        opt(comparison, comparison ? brakeAtText(*comparison) : QString()),
                        sameBrake ? pick(current->distanceM, comparison->distanceM, true, 0.5) : -1});
        rows.push_back({QStringLiteral("Braking"),
                        opt(current, current ? metresOrDash(current->brakingDistanceM) : QString()),
                        opt(comparison, comparison ? metresOrDash(comparison->brakingDistanceM) : QString()),
                        sameBrake && current->brakingDistanceM >= 0 && comparison->brakingDistanceM >= 0
                            ? pick(current->brakingDistanceM, comparison->brakingDistanceM, false, 0.5) : -1});
        rows.push_back({QStringLiteral("Entry"), opt(current, current ? kph(current->speedKph) : QString()),
                        opt(comparison, comparison ? kph(comparison->speedKph) : QString()),
                        sameBrake ? pick(current->speedKph, comparison->speedKph, true, 0.5) : -1});
        rows.push_back({QStringLiteral("Coast"), opt(current, current ? coastText(*current) : QString()),
                        opt(comparison, comparison ? coastText(*comparison) : QString())});
    }
    for (size_t i = 0; i < corners.size(); ++i) {
        const auto& [a, b] = corners[i];
        const QString name = (corners.size() > 1 ? QStringLiteral("Apex %1").arg(i + 1) : QStringLiteral("Apex")) +
                             sideText((a ? a : b)->side);
        rows.push_back({name, opt(current, a ? kph(a->speedKph) : QStringLiteral("—")),
                        opt(comparison, b ? kph(b->speedKph) : QStringLiteral("—")),
                        a && b ? pick(a->speedKph, b->speedKph, true, 0.5) : -1});
        // Which lap ran further inside at the apex, when both recorded positions.
        if (a && b && a->hasLine && b->hasLine)
            rows.push_back({corners.size() > 1 ? QStringLiteral("Line %1").arg(i + 1) : QStringLiteral("Line"),
                            lineText(a->lineGapM), lineText(b->lineGapM)});
    }
    if (braking && pair) {
        const auto harvest = [](float kj) {
            return kj >= 0 ? std::optional<QString>(QStringLiteral("+%1 kJ").arg(QLocale().toString(qRound(kj))))
                           : std::nullopt;
        };
        rows.push_back({QStringLiteral("ERS"), harvest(pair->currentHarvestKj), harvest(pair->comparisonHarvestKj),
                        pair->currentHarvestKj >= 0 && pair->comparisonHarvestKj >= 0
                            ? pick(pair->currentHarvestKj, pair->comparisonHarvestKj, true, 0.5) : -1});
    }
    if (acceleration) {
        rows.push_back({QStringLiteral("Throttle on"),
                        opt(current, current ? metresOrDash(current->throttleOnM) : QString()),
                        opt(comparison, comparison ? metresOrDash(comparison->throttleOnM) : QString()),
                        both ? exitBetter(current->throttleOnM, comparison->throttleOnM,
                                          current->wheelspin, comparison->wheelspin) : -1,
                        current && current->wheelspin && current->throttleOnM >= 0,
                        comparison && comparison->wheelspin && comparison->throttleOnM >= 0});
        rows.push_back({QStringLiteral("Full throttle"),
                        opt(current, current ? metresOrDash(current->fullThrottleM) : QString()),
                        opt(comparison, comparison ? metresOrDash(comparison->fullThrottleM) : QString()),
                        both ? exitBetter(current->fullThrottleM, comparison->fullThrottleM,
                                          current->wheelspin, comparison->wheelspin) : -1});
        rows.push_back({QStringLiteral("0 → 100%"),
                        opt(current, current ? metresOrDash(current->throttleToFullM) : QString()),
                        opt(comparison, comparison ? metresOrDash(comparison->throttleToFullM) : QString()),
                        both ? exitBetter(current->throttleToFullM, comparison->throttleToFullM,
                                          current->wheelspin, comparison->wheelspin) : -1});
        int spinBetter = -1;
        if (both && (current->wheelspin || comparison->wheelspin))
            spinBetter = current->wheelspin && comparison->wheelspin
                ? pick(current->wheelspinPct, comparison->wheelspinPct, false, 0.5)
                : current->wheelspin ? 1 : 0;
        rows.push_back({QStringLiteral("Wheelspin"), opt(current, current ? spinText(*current) : QString()),
                        opt(comparison, comparison ? spinText(*comparison) : QString()), spinBetter});
    }
    rows.push_back({QStringLiteral("Time"), opt(timed, QStringLiteral("%1 s").arg(times[0], 0, 'f', 3)),
                    opt(timed, QStringLiteral("%1 s").arg(times[1], 0, 'f', 3)),
                    timed ? pick(times[0], times[1], false, kBrakingLevelS / 2) : -1});

    const QString muted = palette.color(QPalette::PlaceholderText).name();
    const QString warn = QStringLiteral("#f59e0b");
    double shownFrom = fromM, shownTo = toM;
    if (phase == Phase::Braking) shownTo = splitM;
    else if (phase == Phase::Acceleration) shownFrom = splitM;

    QString html = QStringLiteral("<div style='white-space:nowrap'>");
    html += QStringLiteral("<table width='100%' cellspacing='0' cellpadding='0'><tr>"
                           "<td style='color:%1'><small>%2 ZONE %3%4</small></td>"
                           "<td align='right' style='color:%1'><small>&nbsp;&nbsp;%5 – %6</small></td></tr></table>")
                .arg(muted, phase == Phase::Acceleration ? QStringLiteral("ACCELERATION") : QStringLiteral("BRAKING"))
                .arg(zone)
                .arg(corners.size() > 1 ? QStringLiteral(" · %1 CORNERS").arg(corners.size()) : QString())
                .arg(metres(shownFrom), metres(shownTo));
    if (timed) {
        const double quicker = times[1] - times[0];
        if (std::abs(quicker) < kBrakingLevelS)
            html += QStringLiteral("<p style='margin:4px 0'><b>Level through the %1</b></p>")
                        .arg(braking && acceleration ? QStringLiteral("zone") : QStringLiteral("half"));
        else
            html += QStringLiteral("<p style='margin:4px 0;color:%1'><b>%2 %3 s quicker</b></p>")
                        .arg((quicker > 0 ? labels.currentColor : labels.comparisonColor).name(),
                             (quicker > 0 ? labels.current : labels.comparison).toHtmlEscaped())
                        .arg(std::abs(quicker), 0, 'f', 3);
    }
    html += QStringLiteral("<table cellspacing='0' cellpadding='2' width='100%'><tr>");
    if (current)
        html += QStringLiteral("<th align='left' style='color:%1'><small>%2</small></th>")
                    .arg(labels.currentColor.name(), labels.current.toHtmlEscaped());
    html += QStringLiteral("<th></th>");
    if (comparison)
        html += QStringLiteral("<th align='right' style='color:%1'><small>%2</small></th>")
                    .arg(labels.comparisonColor.name(), labels.comparison.toHtmlEscaped());
    html += QStringLiteral("</tr>");
    const auto cell = [&](const std::optional<QString>& value, bool wins, bool warning, const QColor& lapColor,
                          const char* align) {
        QString style;
        if (warning) style = QStringLiteral("color:%1;font-weight:bold").arg(warn);
        else if (wins) style = QStringLiteral("color:%1;font-weight:bold").arg(lapColor.name());
        return QStringLiteral("<td align='%1' style='%2'>%3</td>")
            .arg(QLatin1String(align), style, (value ? *value : QStringLiteral("—")).toHtmlEscaped());
    };
    for (const Row& row : rows) {
        if (!row.a && !row.b) continue;
        html += QStringLiteral("<tr>");
        if (current) html += cell(row.a, row.better == 0, row.warnA, labels.currentColor, "left");
        html += QStringLiteral("<td align='center' style='color:%1'><small>&nbsp;%2&nbsp;</small></td>")
                    .arg(muted, row.measure.toHtmlEscaped());
        if (comparison) html += cell(row.b, row.better == 1, row.warnB, labels.comparisonColor, "right");
        html += QStringLiteral("</tr>");
    }
    html += QStringLiteral("</table>");
    QStringList observations;
    if (pair)
        for (const tnrp::BrakingObservation& o : pair->observations)
            if (exitObservation(o.kind) ? acceleration : braking) observations << observationText(o, labels);
    if (!observations.isEmpty()) {
        html += QStringLiteral("<table cellspacing='0' cellpadding='1' style='color:%1' width='270'>").arg(muted);
        for (const QString& text : observations)
            html += QStringLiteral("<tr><td valign='top'>▸&nbsp;</td><td style='white-space:normal'>%1</td></tr>")
                        .arg(text.toHtmlEscaped());
        html += QStringLiteral("</table>");
    }
    html += QStringLiteral("</div>");
    return html;
}

// Braking zones, highlighted on the racing line in two halves: braking, up to
// the slowest point, dimmed, then acceleration. With two laps each half takes
// the colour of the lap that was quicker through it, neutral when level; one
// lap on its own shows its zones in its colour.
QVector<TrackMapWidget::Mark> brakingMarks(const tnrp::AnalysisMapResult* result, const Labels& labels,
                                           bool dark, const QPalette& palette) {
    QVector<TrackMapWidget::Mark> marks;
    if (!result || !result->hasCurrent) return marks;
    const tnrp::BrakingPointsMap& braking = result->braking;
    const double length = braking.trackLengthM;
    if (!(length > 0)) return marks;
    const QColor level(dark ? "#E9EAEE" : "#1B1D24");
    const auto winner = [&](double currentS, double comparisonS) {
        if (currentS < 0 || comparisonS < 0) return level;
        const double quicker = comparisonS - currentS;
        return std::abs(quicker) < kBrakingLevelS ? level : quicker > 0 ? labels.currentColor : labels.comparisonColor;
    };
    const auto halves = [&](double fromM, double splitM, double toM, const QColor& brakingColor,
                            const QColor& accelerationColor, int zone, const tnrp::BrakingPoint* a,
                            const tnrp::BrakingPoint* b, const tnrp::BrakingPointPair* pair) {
        if (splitM < 0) {
            marks.push_back({fromM / length, toM / length, accelerationColor, 1.0,
                             brakingCard(zone, Phase::Whole, fromM, splitM, toM, a, b, pair, labels, palette)});
            return;
        }
        marks.push_back({fromM / length, splitM / length, brakingColor, kBrakingHalfOpacity,
                         brakingCard(zone, Phase::Braking, fromM, splitM, toM, a, b, pair, labels, palette)});
        marks.push_back({splitM / length, toM / length, accelerationColor, 1.0,
                         brakingCard(zone, Phase::Acceleration, fromM, splitM, toM, a, b, pair, labels, palette)});
    };
    if (braking.comparison.empty()) {
        for (size_t i = 0; i < braking.current.size(); ++i) {
            const tnrp::BrakingPoint& point = braking.current[i];
            const double toM = std::max(point.distanceM, point.endM);
            const double split = point.minSpeedM - point.distanceM >= kPhaseMinM && toM - point.minSpeedM >= kPhaseMinM
                ? point.minSpeedM : -1.0;
            halves(point.distanceM, split, toM, labels.currentColor, labels.currentColor, int(i) + 1, &point,
                   nullptr, nullptr);
        }
        return marks;
    }
    for (const tnrp::BrakingPointPair& pair : braking.pairs) {
        const tnrp::BrakingPoint* a = pair.current >= 0 && size_t(pair.current) < braking.current.size()
            ? &braking.current[size_t(pair.current)] : nullptr;
        const tnrp::BrakingPoint* b = pair.comparison >= 0 && size_t(pair.comparison) < braking.comparison.size()
            ? &braking.comparison[size_t(pair.comparison)] : nullptr;
        halves(pair.fromM, pair.splitM, pair.toM, winner(pair.currentBrakingS, pair.comparisonBrakingS),
               pair.splitM < 0 ? winner(pair.currentTimeS, pair.comparisonTimeS)
                               : winner(pair.currentAccelerationS, pair.comparisonAccelerationS),
               pair.current + 1, a, b, &pair);
    }
    return marks;
}

// Braking zone highlights along a lap's own path, for the Racing Line layer.
QVector<TrackMapWidget::Mark> marksOnLap(const QVector<TrackMapWidget::Mark>& marks, const LapBlock& lap,
                                         double trackLengthM) {
    QVector<TrackMapWidget::Mark> out;
    out.reserve(marks.size());
    for (TrackMapWidget::Mark mark : marks) {
        const double from = elapsedAtDistance(&lap, mark.from * trackLengthM);
        const double to = elapsedAtDistance(&lap, mark.to * trackLengthM);
        double x = 0;
        double z = 0;
        if (!std::isfinite(from) || !std::isfinite(to) || !positionAt(lap, from, x, z)) continue;
        mark.world << QPointF(x, z);
        const double start = lap.startSessionTime + from;
        const double end = lap.startSessionTime + to;
        for (const LapPositionSample& sample : lap.positions)
            if (sample.t > start && sample.t < end && std::isfinite(sample.x) && std::isfinite(sample.z))
                mark.world << QPointF(sample.x, sample.z);
        if (positionAt(lap, to, x, z)) mark.world << QPointF(x, z);
        out.push_back(std::move(mark));
    }
    return out;
}

// Each corner's apex on a lap's Speed map, with its slowest speed on hover.
QVector<TrackMapWidget::Mark> apexMarks(const tnrp::SpeedMap* speed, bool dark, const QPalette& palette) {
    QVector<TrackMapWidget::Mark> marks;
    if (!speed || !(speed->trackLengthM > 0)) return marks;
    const double length = speed->trackLengthM;
    const QColor color(dark ? "#F4F5F8" : "#14151A");
    const QString muted = palette.color(QPalette::PlaceholderText).name();
    for (size_t i = 0; i < speed->apexes.size(); ++i) {
        const tnrp::CornerApex& apex = speed->apexes[i];
        marks.push_back({(apex.distanceM - kApexMarkM) / length,
                         (apex.distanceM + kApexMarkM) / length, color, 1.0,
                         QStringLiteral("<div style='white-space:nowrap'><small style='color:%1'>APEX %2%3 · %4</small>"
                                        "<br><b>%5</b></div>")
                             .arg(muted).arg(i + 1).arg(sideText(apex.side), metres(apex.distanceM),
                                                        kph(apex.speedKph)),
                         true});
    }
    return marks;
}

} // namespace

QString AnalysisMapView::brakingZoneCard(const tnrp::AnalysisMapResult& result, int row,
                                         const QString& currentLabel, const QColor& currentColor,
                                         const QString& comparisonLabel, const QColor& comparisonColor,
                                         const QPalette& palette) {
    const tnrp::BrakingPointsMap& braking = result.braking;
    const Labels labels{currentLabel, comparisonLabel, currentColor, comparisonColor};
    if (row < 0) return {};
    if (!braking.comparison.empty()) {
        if (size_t(row) >= braking.pairs.size()) return {};
        const tnrp::BrakingPointPair& pair = braking.pairs[size_t(row)];
        const tnrp::BrakingPoint* a = pair.current >= 0 && size_t(pair.current) < braking.current.size()
            ? &braking.current[size_t(pair.current)] : nullptr;
        const tnrp::BrakingPoint* b = pair.comparison >= 0 && size_t(pair.comparison) < braking.comparison.size()
            ? &braking.comparison[size_t(pair.comparison)] : nullptr;
        return brakingCard(pair.current + 1, Phase::Whole, pair.fromM, pair.splitM, pair.toM, a, b, &pair,
                           labels, palette);
    }
    if (size_t(row) >= braking.current.size()) return {};
    const tnrp::BrakingPoint& point = braking.current[size_t(row)];
    return brakingCard(row + 1, Phase::Whole, point.distanceM, -1.0, std::max(point.distanceM, point.endM),
                       &point, nullptr, nullptr, labels, palette);
}

AnalysisMapView::AnalysisMapView(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // The maps fill their area; the Data Comparison card floats over them.
    mapArea_ = new QWidget(this);
    auto* body = new QVBoxLayout(mapArea_);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    mapsLayout_ = new QBoxLayout(QBoxLayout::LeftToRight);
    mapsLayout_->setSpacing(0);
    body->addLayout(mapsLayout_, 1);
    const QSettings settings(QStringLiteral("TrackNRace"), QStringLiteral("NativeRecorder"));
    for (TrackMapWidget** map : {&map_, &comparisonMap_}) {
        *map = new TrackMapWidget(mapArea_);
        (*map)->setControlledMode(true);
        (*map)->setSectorColors(settings.value("ui/trackMapSectorColors", false).toBool());
        (*map)->setMapOpacity(settings.value("ui/trackMapOpacity", 100).toInt() / 100.0);
        (*map)->setDark(dark());
        (*map)->setViewEnabled(true);
        (*map)->installEventFilter(this);
    }
    for (auto [from, to] : {std::pair{map_, comparisonMap_}, std::pair{comparisonMap_, map_}})
        connect(from, &TrackMapWidget::viewChanged, this, [this, other = to](const TrackMapWidget::View& view) {
            other->setView(view);
            emit zoomChanged(view.zoom);
        });
    mapDivider_ = new QFrame(mapArea_);
    mapDivider_->setFrameShape(QFrame::VLine);
    mapDivider_->setFrameShadow(QFrame::Plain);
    mapDivider_->setForegroundRole(QPalette::Mid);
    mapsLayout_->addWidget(map_, 1);
    mapsLayout_->addWidget(mapDivider_);
    mapsLayout_->addWidget(comparisonMap_, 1);
    mapDivider_->hide();
    comparisonMap_->hide();
    legend_ = new AnalysisMapLegend(map_);
    comparisonLegend_ = new AnalysisMapLegend(comparisonMap_);
    mismatch_ = new QLabel(mapArea_);
    mismatch_->setAttribute(Qt::WA_TransparentForMouseEvents);
    mismatch_->setAutoFillBackground(true);
    mismatch_->setFrameShape(QFrame::StyledPanel);
    mismatch_->setContentsMargins(6, 3, 6, 3);
    mismatch_->setAccessibleName(QStringLiteral("Peak MGU-K differs"));
    mismatch_->hide();
    inputs_ = new AnalysisInputCard(mapArea_);
    root->addWidget(mapArea_, 1);

    transport_ = new QToolBar(QStringLiteral("Lap Replay"), this);
    transport_->setIconSize(QSize(16, 16));
    transport_->setMovable(false);
    back_ = transport_->addAction(themed(this, "media-seek-backward", QStyle::SP_MediaSeekBackward),
                                  QStringLiteral("Back 5 Seconds"));
    play_ = transport_->addAction(themed(this, "media-playback-start", QStyle::SP_MediaPlay),
                                  QStringLiteral("Play"));
    play_->setShortcut(Qt::Key_Space);
    play_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addAction(play_);   // Space works anywhere in the map view, not only on the toolbar
    forward_ = transport_->addAction(themed(this, "media-seek-forward", QStyle::SP_MediaSeekForward),
                                     QStringLiteral("Forward 5 Seconds"));
    slider_ = new QSlider(Qt::Horizontal, transport_);
    slider_->setRange(0, 1000);
    slider_->setAccessibleName(QStringLiteral("Replay position"));
    transport_->addWidget(slider_);
    time_ = new QLabel(transport_);
    time_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    time_->setContentsMargins(6, 0, 6, 0);
    transport_->addWidget(time_);
    speed_ = new QComboBox(transport_);
    speed_->setToolTip(QStringLiteral("Replay speed"));
    for (double rate : {0.25, 0.5, 1.0, 2.0, 4.0})
        speed_->addItem(QString::number(rate) + QStringLiteral("×"), rate);
    speed_->setCurrentIndex(2);
    transport_->addWidget(speed_);
    root->addWidget(transport_);
    transport_->hide();

    timer_ = new QTimer(this);
    timer_->setInterval(16);
    connect(timer_, &QTimer::timeout, this, [this] {
        if (playing_ && localTime() >= total_) {
            cursor_ = total_;
            setPlaying(false);
        }
        refresh();
        refreshTransport();
    });
    connect(play_, &QAction::triggered, this, [this] {
        if (!playing_) {
            cursor_ = localTime();
            if (cursor_ >= total_) cursor_ = 0;
        }
        setPlaying(!playing_);
    });
    connect(back_, &QAction::triggered, this, [this] { seek(localTime() - 5); });
    connect(forward_, &QAction::triggered, this, [this] { seek(localTime() + 5); });
    connect(slider_, &QSlider::valueChanged, this, [this](int value) {
        seek(total_ * value / 1000.0);
    });
    connect(speed_, &QComboBox::currentIndexChanged, this, [this] {
        cursor_ = localTime();
        rate_ = speed_->currentData().toDouble();
        clock_.restart();
    });
    refreshTransport();
}

AnalysisMapView::~AnalysisMapView() = default;

bool AnalysisMapView::dark() const {
    return tnr::isDarkTheme();
}

bool AnalysisMapView::separateMaps() const {
    return comparisonSelected_ &&
           (layerMode_ == QLatin1String("ers") || layerMode_ == QLatin1String("harvest") ||
            layerMode_ == QLatin1String("inputs") || layerMode_ == QLatin1String("speed"));
}

void AnalysisMapView::setLaps(const LapBlock* current, const LapBlock* comparison, bool fixedMode,
                              int trackId, bool compatibleCircuit) {
    // Copy the laps only when they actually changed; the page re-applies state often.
    auto describe = [](const LapBlock* lap) {
        return lap ? QStringLiteral("%1/%2/%3/%4/%5")
                         .arg(quintptr(lap)).arg(lap->lapNum).arg(lap->positions.size())
                         .arg(lap->tel.size()).arg(lap->sts.size())
                   : QStringLiteral("-");
    };
    const QString signature = describe(current) + QLatin1Char('|') + describe(comparison) +
                              QLatin1Char('|') + QString::number(fixedMode) +
                              QLatin1Char('|') + QString::number(trackId);
    if (signature != signature_) {
        signature_ = signature;
        current_ = current ? *current : LapBlock{};
        comparison_ = comparison ? *comparison : LapBlock{};
        hasCurrent_ = current;
        hasComparison_ = comparison;
        total_ = qMax(hasCurrent_ ? double(current_.endSessionTime - current_.startSessionTime) : 0.0,
                      hasComparison_ ? double(comparison_.endSessionTime - comparison_.startSessionTime)
                                     : 0.0);
        cursor_ = 0;
        focused_ = false;
        setPlaying(false);
        // No result follows new laps on this layer; it draws them directly.
        if (layerMode_ == QLatin1String("racing")) refreshLayers();
    }
    fixed_ = fixedMode;
    transport_->setVisible(fixed_);
    map_->setTrack(compatibleCircuit ? trackId : -1);
    comparisonMap_->setTrack(compatibleCircuit ? trackId : -1);
    refresh();
    refreshTransport();
}

void AnalysisMapView::setColors(const QColor& current, const QColor& comparison) {
    if (current == currentColor_ && comparison == comparisonColor_) return;
    currentColor_ = current;
    comparisonColor_ = comparison;
    inputs_->setIdentity(currentLabel_, currentColor_, comparisonLabel_, comparisonColor_);
    refreshLayers();
    refresh();
}

void AnalysisMapView::setLabels(const QString& current, const QString& comparison) {
    if (current == currentLabel_ && comparison == comparisonLabel_) return;
    currentLabel_ = current;
    comparisonLabel_ = comparison;
    inputs_->setIdentity(currentLabel_, currentColor_, comparisonLabel_, comparisonColor_);
    refreshLayers();
    refresh();
}

void AnalysisMapView::setMapAppearance(bool sectorColors, int opacityPercent) {
    for (TrackMapWidget* map : {map_, comparisonMap_}) {
        map->setSectorColors(sectorColors);
        map->setMapOpacity(qBound(0, opacityPercent, 100) / 100.0);
    }
}

void AnalysisMapView::setAeroMode(bool slm) {
    map_->setAeroMode(slm);
    comparisonMap_->setAeroMode(slm);
}

void AnalysisMapView::setLayerMode(const QString& mode, bool comparisonSelected) {
    if (mode == requestedLayerMode_ && comparisonSelected == requestedComparisonSelected_) return;
    requestedLayerMode_ = mode;
    requestedComparisonSelected_ = comparisonSelected;
    // A coloured layer waits for its result with the old one still painted;
    // switching at once would show the bare map in between. Racing Line has
    // no result to wait for.
    if (mode == QLatin1String("none") || mode == QLatin1String("racing") || !layer_) applyRequestedLayer();
}

void AnalysisMapView::setLayerResult(std::shared_ptr<const tnrp::AnalysisMapResult> result) {
    const bool waiting = layerMode_ != requestedLayerMode_ || comparisonSelected_ != requestedComparisonSelected_;
    if (result == layer_ && !waiting) return;
    layer_ = std::move(result);
    if (waiting) applyRequestedLayer();
    else refreshLayers();
}

void AnalysisMapView::applyRequestedLayer() {
    layerMode_ = requestedLayerMode_;
    comparisonSelected_ = requestedComparisonSelected_;
    refreshLayers();
    refresh();
}

void AnalysisMapView::setBrakingResult(std::shared_ptr<const tnrp::AnalysisMapResult> result) {
    if (result == braking_) return;
    braking_ = std::move(result);
    refreshLayers();
}

void AnalysisMapView::refreshLayers() {
    const bool isDark = dark();
    const bool separate = separateMaps();
    comparisonMap_->setVisible(separate);
    mapDivider_->setVisible(separate);
    if (separate) comparisonMap_->setView(map_->view());

    // libtnrp's result for the layer on show, once it has arrived.
    const tnrp::AnalysisMapResult* result =
        layer_ && QString::fromStdString(layer_->mode) == layerMode_ ? layer_.get() : nullptr;
    const std::string currentColor = currentColor_.name().toStdString();
    const std::string comparisonColor = comparisonColor_.name().toStdString();
    map_->setPaint(result ? tnrp::analysisMapPaint(*result, 0, isDark, currentColor, comparisonColor)
                          : tnrp::AnalysisMapPaint{});
    comparisonMap_->setPaint(result && separate
        ? tnrp::analysisMapPaint(*result, 1, isDark, currentColor, comparisonColor) : tnrp::AnalysisMapPaint{});

    // Racing Line: both laps' recorded positions on the one map, as they are.
    const bool racing = layerMode_ == QLatin1String("racing");
    QVector<TrackMapWidget::RacingLine> racingLines;
    if (racing) {
        const auto addLine = [&](const LapBlock& lap, const QColor& lineColor) {
            QPolygonF world;
            world.reserve(lap.positions.size());
            for (const LapPositionSample& sample : lap.positions)
                if (std::isfinite(sample.x) && std::isfinite(sample.z)) world << QPointF(sample.x, sample.z);
            racingLines.push_back({world, lineColor});
        };
        if (hasCurrent_) addLine(current_, currentColor_);
        if (hasComparison_) addLine(comparison_, comparisonColor_);
    }
    map_->setRacingLines(racing, racingLines);

    // Legends.
    const tnrp::AnalysisMapPalette& palette = tnrp::analysisMapPalette(isDark);
    const auto legendFor = [&](int lap) {
        QVector<LegendItem> items;
        using Kind = LegendItem::Kind;
        if (racing) {
            if (hasCurrent_) items.push_back({Kind::Swatch, currentLabel_, currentColor_});
            if (hasComparison_) items.push_back({Kind::Swatch, comparisonLabel_, comparisonColor_});
            return items;
        }
        if (!result) return items;
        const bool has = lap == 0 ? result->hasCurrent : result->hasComparison;
        if (!has) return items;
        if (layerMode_ == QLatin1String("ers")) {
            const tnrp::ErsUsageMap& usage = lap == 0 ? result->current : result->comparison;
            if (!usage.restricted && !usage.usesNormal && !usage.usesReduced) return items;
            if (usage.restricted) return QVector<LegendItem>{{Kind::Text, QStringLiteral("No ERS data")}};
            items.push_back({Kind::Text, QStringLiteral("0")});
            items.push_back({Kind::Ramp, {}, {}, rampColors(palette.ers)});
            if (usage.usesNormal) items.push_back({Kind::Strong, formatKw(usage.normalPeakKw)});
            if (usage.usesReduced) {
                if (usage.usesNormal) items.push_back({Kind::Text, QStringLiteral("·")});
                items.push_back({Kind::Strong, formatKw(usage.reducedPeakKw)});
                items.push_back({Kind::Text, QStringLiteral("reduced grip")});
            }
        } else if (layerMode_ == QLatin1String("harvest")) {
            const tnrp::ErsHarvestMap& harvest = lap == 0 ? result->currentHarvest : result->comparisonHarvest;
            if (!harvest.restricted && harvest.level.empty()) return items;
            if (harvest.restricted) return QVector<LegendItem>{{Kind::Text, QStringLiteral("No ERS data")}};
            items.push_back({Kind::Text, QStringLiteral("0")});
            items.push_back({Kind::Ramp, {}, {}, rampColors(palette.harvest)});
            items.push_back({Kind::Strong, formatKw(std::round(harvest.peakKw))});
            if (std::any_of(harvest.superclip.begin(), harvest.superclip.end(), [](int v) { return v != 0; }))
                items.push_back({Kind::Swatch, QStringLiteral("Superclip"), color(palette.superclip)});
        } else if (layerMode_ == QLatin1String("speed")) {
            const tnrp::SpeedMap& speed = lap == 0 ? result->currentSpeed : result->comparisonSpeed;
            if (speed.speedKph.empty()) return items;
            items.push_back({Kind::Text, QString::number(qRound(result->speedScaleMinKph))});
            items.push_back({Kind::Ramp, {}, {}, rampColors(palette.speed)});
            items.push_back({Kind::Strong, kph(result->speedScaleMaxKph)});
        } else if (layerMode_ == QLatin1String("inputs")) {
            for (size_t state = 0; state < palette.inputs.size(); ++state)
                items.push_back({Kind::Swatch, QString::fromStdString(palette.inputLabels[state]),
                                 color(palette.inputs[state])});
        }
        return items;
    };
    legend_->setItems(legendFor(0));
    comparisonLegend_->setItems(separate ? legendFor(1) : QVector<LegendItem>{});

    // Two cars whose peaks differ in a grip both laps saw are drawn against different caps.
    QStringList mismatches;
    if (result && separate && result->hasCurrent && result->hasComparison) {
        if (layerMode_ == QLatin1String("ers")) {
            if (result->normalPeakMismatch)
                mismatches << QStringLiteral("normal grip %1 %2, %3 %4")
                                  .arg(currentLabel_, formatKw(result->current.normalPeakKw),
                                       comparisonLabel_, formatKw(result->comparison.normalPeakKw));
            if (result->reducedPeakMismatch)
                mismatches << QStringLiteral("reduced grip %1 %2, %3 %4")
                                  .arg(currentLabel_, formatKw(result->current.reducedPeakKw),
                                       comparisonLabel_, formatKw(result->comparison.reducedPeakKw));
        } else if (layerMode_ == QLatin1String("harvest") && result->harvestPeakMismatch) {
            mismatches << QStringLiteral("harvest %1 %2, %3 %4")
                              .arg(currentLabel_, formatKw(std::round(result->currentHarvest.peakKw)),
                                   comparisonLabel_, formatKw(std::round(result->comparisonHarvest.peakKw)));
        }
    }
    mismatch_->setText(QStringLiteral("⚠ Peak MGU-K differs: ") + mismatches.join(QStringLiteral("; ")));
    mismatch_->setVisible(!mismatches.isEmpty());
    mismatch_->adjustSize();

    // Braking zones on every map, and each lap's corners on its Speed map.
    // Racing Line has no track, so its zones follow a lap's own path.
    const Labels labels{currentLabel_, comparisonLabel_, currentColor_, comparisonColor_};
    const tnrp::AnalysisMapResult* zoneResult = braking_ && braking_->mode == "braking" ? braking_.get() : nullptr;
    QVector<TrackMapWidget::Mark> zones = brakingMarks(zoneResult, labels, isDark, this->palette());
    if (racing)
        zones = zoneResult && (hasCurrent_ || hasComparison_)
            ? marksOnLap(zones, hasCurrent_ ? current_ : comparison_, zoneResult->braking.trackLengthM)
            : QVector<TrackMapWidget::Mark>{};
    const bool speed = result && layerMode_ == QLatin1String("speed");
    map_->setMarks(zones + apexMarks(speed && result->hasCurrent ? &result->currentSpeed : nullptr, isDark,
                                     this->palette()));
    comparisonMap_->setMarks(separate ? zones + apexMarks(speed && result->hasComparison ? &result->comparisonSpeed
                                                                                         : nullptr,
                                                          isDark, this->palette())
                                      : QVector<TrackMapWidget::Mark>{});
    placeOverlays();
}

void AnalysisMapView::placeOverlays() {
    // Side by side on a wide pane, stacked on a tall one.
    const bool stack = mapArea_->height() > mapArea_->width();
    mapsLayout_->setDirection(stack ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    mapDivider_->setFrameShape(stack ? QFrame::HLine : QFrame::VLine);
    for (auto [map, legend] : {std::pair{map_, legend_}, std::pair{comparisonMap_, comparisonLegend_}}) {
        legend->resize(legend->sizeHint());
        legend->move(map->width() - legend->width() - 8, map->height() - legend->height() - 8);
        legend->raise();
    }
    mismatch_->move(mapArea_->width() - mismatch_->width() - 8, 8);
    mismatch_->raise();
}

bool AnalysisMapView::eventFilter(QObject* watched, QEvent* event) {
    if ((watched == map_ || watched == comparisonMap_) && event->type() == QEvent::Resize) placeOverlays();
    return QWidget::eventFilter(watched, event);
}

void AnalysisMapView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    placeOverlays();
}

void AnalysisMapView::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange ||
        event->type() == QEvent::ThemeChange)
        refreshTheme();
}

void AnalysisMapView::refreshTheme() {
    map_->setDark(dark());
    comparisonMap_->setDark(dark());
    refreshLayers();
}

void AnalysisMapView::setCurrentTime(float sessionTime) {
    sessionTime_ = sessionTime;
    if (!fixed_ && !focused_) refresh();
}

void AnalysisMapView::focusElapsed(double seconds) {
    focused_ = true;
    cursor_ = qBound(0.0, seconds, total_);
    setPlaying(false);
    refresh();
    refreshTransport();
}

void AnalysisMapView::zoomIn() { map_->zoomBy(kMapZoomStep); }
void AnalysisMapView::zoomOut() { map_->zoomBy(1.0 / kMapZoomStep); }
void AnalysisMapView::fitCircuit() { map_->fitView(); }
double AnalysisMapView::zoom() const { return map_->view().zoom; }
bool AnalysisMapView::isZoomed() const { return zoom() > 1.0; }
bool AnalysisMapView::isAtMaxZoom() const { return zoom() >= TrackMapWidget::kMaxViewZoom; }

void AnalysisMapView::setReadoutVisible(bool visible) {
    inputs_->setVisible(visible);
    refresh();
}

void AnalysisMapView::hideEvent(QHideEvent* event) {
    setPlaying(false);
    QWidget::hideEvent(event);
}

double AnalysisMapView::localTime() const {
    return playing_ ? qMin(total_, cursor_ + clock_.elapsed() / 1000.0 * rate_) : cursor_;
}

double AnalysisMapView::cursorElapsed() const {
    if (fixed_ || focused_) return localTime();
    return hasCurrent_ ? sessionTime_ - current_.startSessionTime : 0.0;
}

void AnalysisMapView::seek(double seconds) {
    cursor_ = qBound(0.0, seconds, total_);
    clock_.restart();
    refresh();
    refreshTransport();
}

void AnalysisMapView::setPlaying(bool playing) {
    playing_ = playing && total_ > 0;
    clock_.restart();
    if (playing_) timer_->start();
    else timer_->stop();
    play_->setText(playing_ ? QStringLiteral("Pause") : QStringLiteral("Play"));
    play_->setIcon(playing_ ? themed(this, "media-playback-pause", QStyle::SP_MediaPause)
                            : themed(this, "media-playback-start", QStyle::SP_MediaPlay));
    refreshTransport();
}

void AnalysisMapView::refresh() {
    const double elapsed = cursorElapsed();
    emit cursorElapsedChanged(elapsed);

    // Separate maps each carry their own lap's car.
    const bool separate = separateMaps();
    QVector<TrackMapWidget::Marker> markers;
    QVector<TrackMapWidget::Marker> comparisonMarkers;
    double x = 0;
    double z = 0;
    if (hasCurrent_ && positionAt(current_, elapsed, x, z))
        markers.push_back({x, z, currentLabel_, currentColor_});
    if (hasComparison_ && positionAt(comparison_, elapsed, x, z))
        (separate ? comparisonMarkers : markers).push_back({x, z, comparisonLabel_, comparisonColor_});
    map_->setControlledMarkers(markers);
    comparisonMap_->setControlledMarkers(comparisonMarkers);

    if (!inputs_->isHidden())
        inputs_->setReadings(hasCurrent_ ? &current_ : nullptr,
                              hasComparison_ ? &comparison_ : nullptr, elapsed);
}

void AnalysisMapView::refreshTransport() {
    const double now = localTime();
    {
        QSignalBlocker guard(slider_);
        slider_->setValue(total_ > 0 ? qRound(now / total_ * 1000) : 0);
    }
    time_->setText(clockText(now) + QStringLiteral(" / ") + clockText(total_));
    const bool ready = total_ > 0;
    for (QAction* action : {back_, play_, forward_}) action->setEnabled(ready);
    slider_->setEnabled(ready);
}
