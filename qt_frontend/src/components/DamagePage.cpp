#include "DamagePage.h"
#include "CardColors.h"
#include "PageUiHelpers.h"
#include "SummaryCard.h"
#include "../Labels.h"
#include "../PlaybackPatchMerger.h"

#include <QEvent>
#include <QFile>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QHash>
#include <QLayout>
#include <QPainter>
#include <QPainterPath>
#include <QSvgRenderer>
#include <QVBoxLayout>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <algorithm>
#include <cmath>

namespace {

// A damage field, or nothing while it is unavailable (no row yet, or a V6
// field the recording does not carry for this driver).
std::optional<int> num(const std::optional<DamageRow>& row, int DamageRow::* field) {
    if (!row) return std::nullopt;
    const int value = (*row).*field;
    return value == kPlaybackMissingInt ? std::nullopt : std::optional<int>(value);
}

enum class Severity { NA, Ok, Warn, Crit };
// Body damage: any damage is worth seeing; above 20% it costs real performance.
Severity damageSeverity(std::optional<int> v) {
    return !v ? Severity::NA : *v == 0 ? Severity::Ok : *v <= 20 ? Severity::Warn : Severity::Crit;
}
// Power-unit and gearbox wear accumulate normally, so only high wear is a warning.
Severity wearSeverity(std::optional<int> v) {
    return !v ? Severity::NA : *v < 50 ? Severity::Ok : *v < 75 ? Severity::Warn : Severity::Crit;
}
// Tyre, brake and blister values use a five-band scale, 20% per band.
enum class TyreBand { NA, Green, GreenYellow, Yellow, YellowRed, Red };
TyreBand tyreBand(std::optional<int> v) {
    if (!v) return TyreBand::NA;
    return *v < 20 ? TyreBand::Green : *v < 40 ? TyreBand::GreenYellow : *v < 60 ? TyreBand::Yellow
         : *v < 80 ? TyreBand::YellowRed : TyreBand::Red;
}

// Electron's --text-muted / --text-dim (dark / light).
QColor textMuted() { return tnr::themed("#484c62", "#5f656b"); }
QColor textDim() { return tnr::themed("#5a5e78", "#63696f"); }

QColor severityColor(Severity severity) {
    switch (severity) {
        case Severity::Ok:   return tnr::themed("#37872D", "#137333");
        case Severity::Warn: return tnr::themed("#E0A800", "#8a6500");
        case Severity::Crit: return QColor("#C4162A");
        default:             return textMuted();
    }
}

QColor bandColor(TyreBand band) {
    switch (band) {
        case TyreBand::Green:       return tnr::themed("#37872D", "#137333");
        case TyreBand::GreenYellow: return tnr::themed("#9BBF2E", "#5E7A12");
        case TyreBand::Yellow:      return tnr::themed("#E0A800", "#8a6500");
        case TyreBand::YellowRed:   return tnr::themed("#F2711C", "#B84A08");
        case TyreBand::Red:         return QColor("#C4162A");
        default:                    return textMuted();
    }
}

// CSS color-mix(in srgb, color amount, base).
QColor mixInto(const QColor& color, double amount, const QColor& base) {
    return QColor::fromRgbF(color.redF() * amount + base.redF() * (1 - amount),
                            color.greenF() * amount + base.greenF() * (1 - amount),
                            color.blueF() * amount + base.blueF() * (1 - amount));
}

QString pct(std::optional<int> v) {
    return v ? QStringLiteral("%1%").arg(*v) : SummaryCard::kMissing;
}

// ── Drawing geometry (Electron DamagePage.tsx, viewBox 0 0 1580 520) ─────────
// The shared wireframe's viewBox is offset into the page drawing by
// kCarX/kCarY; the callouts sit around it.
constexpr double kViewWidth = 1580, kViewHeight = 520;
constexpr double kCarX = 350, kCarY = 70, kCarWidth = 952, kCarHeight = 370;

struct BodyPart { int DamageRow::* field; QStringList zones; };
const BodyPart kWingL{&DamageRow::wing_fl, {"z-fw-l"}};
const BodyPart kWingR{&DamageRow::wing_fr, {"z-fw-r"}};
const BodyPart kRearWing{&DamageRow::wing_rear, {"z-rw"}};
const BodyPart kFloor{&DamageRow::floor_damage, {"z-floor"}};
const BodyPart kSidepods{&DamageRow::sidepod_damage, {"z-sp-l", "z-sp-r"}};
const BodyPart kDiffuser{&DamageRow::diffuser_damage, {"z-diff"}};
const BodyPart* const kBodyParts[] = {&kWingL, &kWingR, &kRearWing, &kFloor, &kSidepods, &kDiffuser};

enum class Anchor { Start, Middle, End };
struct Callout { const BodyPart* part; const char* label; double x, y; Anchor anchor; };
const Callout kCallouts[] = {
    {&kRearWing, "REAR WING", 300, 232, Anchor::End},
    {&kDiffuser, "DIFFUSER", 300, 312, Anchor::End},
    {&kFloor, "FLOOR", 944, 30, Anchor::Middle},
    {&kSidepods, "SIDEPODS", 794, 476, Anchor::Middle},
    {&kWingL, "WING L", 1320, 182, Anchor::Start},
    {&kWingR, "WING R", 1320, 312, Anchor::Start},
};

struct Wheel {
    const char* id;
    int DamageRow::* tyre; int DamageRow::* brake; int DamageRow::* blisters;
    const char* color; const char* lightColor;
    double tx, ty; Anchor anchor; double cols[3];
};
const Wheel kWheels[] = {
    {"fl", &DamageRow::tyre_dmg_fl, &DamageRow::brake_dmg_fl, &DamageRow::blisters_fl, "#e10600", "#e10600", 1320, 56, Anchor::Start, {1320, 1405, 1490}},
    {"fr", &DamageRow::tyre_dmg_fr, &DamageRow::brake_dmg_fr, &DamageRow::blisters_fr, "#4488ff", "#0B57D0", 1320, 456, Anchor::Start, {1320, 1405, 1490}},
    {"rl", &DamageRow::tyre_dmg_rl, &DamageRow::brake_dmg_rl, &DamageRow::blisters_rl, "#37872D", "#137333", 300, 126, Anchor::End, {50, 140, 230}},
    {"rr", &DamageRow::tyre_dmg_rr, &DamageRow::brake_dmg_rr, &DamageRow::blisters_rr, "#ffd700", "#765900", 300, 406, Anchor::End, {50, 140, 230}},
};

// Leader lines (page drawing units), each ending in a dot on its part. Front
// tyres use elbows so the line clears the front wing.
struct Leader { QVector<QPointF> points; };
const Leader kLeaders[] = {
    {{{310, 120}, {500, 120}}}, {{{310, 240}, {400, 240}}}, {{{310, 320}, {500, 320}}}, {{{310, 400}, {500, 400}}},
    {{{944, 72}, {944, 112}}}, {{{794, 452}, {794, 368}}}, {{{1310, 50}, {1084, 50}, {1084, 112}}},
    {{{1310, 190}, {1240, 190}}}, {{{1310, 320}, {1240, 320}}}, {{{1310, 450}, {1084, 450}, {1084, 398}}},
};

// The shared wireframe restyled by id/class (Electron does the same with CSS):
// wires, tubes and solids take the theme colours, and a damaged zone takes
// its severity colour on its strokes with a 12% tint on its solids.
QByteArray styledCarSvg(const QByteArray& source, const QHash<QString, QColor>& zones,
                        const QColor& wire, const QColor& tube, const QColor& background) {
    QXmlStreamReader reader(source);
    reader.setNamespaceProcessing(false);
    QByteArray out;
    QXmlStreamWriter writer(&out);
    writer.writeStartDocument();
    QVector<QColor> zoneStack;   // innermost styled zone's colour (invalid = none)
    while (!reader.atEnd()) {
        const QXmlStreamReader::TokenType token = reader.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const QXmlStreamAttributes attributes = reader.attributes();
            const QString id = attributes.value(QLatin1String("id")).toString();
            const QString cls = attributes.value(QLatin1String("class")).toString();
            const bool styledZone = zones.contains(id);
            const QColor zone = styledZone ? zones.value(id)
                : zoneStack.isEmpty() ? QColor() : zoneStack.last();
            zoneStack.push_back(zone);
            QXmlStreamAttributes rewritten;
            bool hasStroke = false, hasStrokeWidth = false;
            for (const QXmlStreamAttribute& attribute : attributes) {
                const QString name = attribute.qualifiedName().toString();
                QString value = attribute.value().toString();
                if (name == QLatin1String("stroke")) {
                    hasStroke = true;
                    if (styledZone) value = zone.name();
                    else if (cls == QLatin1String("wire")) value = wire.name();
                    else if (cls == QLatin1String("tube")) value = tube.name();
                } else if (name == QLatin1String("stroke-width") && styledZone) {
                    hasStrokeWidth = true;
                    value = QStringLiteral("3.2");
                } else if (name == QLatin1String("fill") && cls == QLatin1String("solid")) {
                    value = (zone.isValid() ? mixInto(zone, .12, background) : background).name();
                }
                rewritten.append(name, value);
            }
            if (styledZone && !hasStroke) rewritten.append(QStringLiteral("stroke"), zone.name());
            if (styledZone && !hasStrokeWidth) rewritten.append(QStringLiteral("stroke-width"), QStringLiteral("3.2"));
            const QString element = reader.qualifiedName().toString();
            if (element == QLatin1String("svg") && !attributes.hasAttribute(QLatin1String("xmlns")))
                rewritten.append(QStringLiteral("xmlns"), QStringLiteral("http://www.w3.org/2000/svg"));
            writer.writeStartElement(element);
            writer.writeAttributes(rewritten);
        } else if (token == QXmlStreamReader::EndElement) {
            if (!zoneStack.isEmpty()) zoneStack.pop_back();
            writer.writeEndElement();
        }
    }
    writer.writeEndDocument();
    return out;
}

} // namespace

// The car wireframe with its callouts, scaled to fit like an SVG viewBox
// (xMidYMid meet). The asset is restyled only when a zone's colour or the
// theme changes; callout text is drawn at device size so it stays crisp.
class CarDamageDiagram final : public QWidget {
public:
    explicit CarDamageDiagram(QWidget* parent = nullptr) : QWidget(parent) {
        QFile file(QStringLiteral(":/car/f1-car-wireframe.svg"));
        if (file.open(QIODevice::ReadOnly)) source_ = file.readAll();
        setContentsMargins(12, 12, 12, 8);   // Electron px-3 pt-3 pb-2
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMinimumSize(1, 1);
        setAccessibleName(QStringLiteral("Car damage by part"));
    }

    void setDamage(const std::optional<DamageRow>& damage) {
        damage_ = damage;
        restyle();
        update();
    }
    void themeChanged() { styleKey_.clear(); restyle(); update(); }

protected:
    void paintEvent(QPaintEvent*) override {
        const QRectF area = contentsRect();
        if (area.width() <= 0 || area.height() <= 0) return;
        const double scale = std::min(area.width() / kViewWidth, area.height() / kViewHeight);
        const QPointF origin(area.left() + (area.width() - kViewWidth * scale) / 2,
                             area.top() + (area.height() - kViewHeight * scale) / 2);
        const auto device = [&](double x, double y) { return origin + QPointF(x * scale, y * scale); };

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);
        painter.save();
        painter.translate(origin);
        painter.scale(scale, scale);
        if (renderer_.isValid()) renderer_.render(&painter, QRectF(kCarX, kCarY, kCarWidth, kCarHeight));
        // Leader lines in --text-muted, each ending in a --text-dim dot.
        QPen leaderPen(textMuted(), 1.5);
        leaderPen.setCapStyle(Qt::FlatCap);
        leaderPen.setJoinStyle(Qt::MiterJoin);
        painter.setPen(leaderPen);
        painter.setBrush(Qt::NoBrush);
        for (const Leader& leader : kLeaders) painter.drawPolyline(leader.points.constData(), int(leader.points.size()));
        painter.setPen(Qt::NoPen);
        painter.setBrush(textDim());
        for (const Leader& leader : kLeaders) painter.drawEllipse(leader.points.last(), 4.0, 4.0);
        painter.restore();

        const QColor secondary = palette().color(QPalette::PlaceholderText);
        const auto drawText = [&](double x, double y, Anchor anchor, const QString& text,
                                  double size, bool bold, double spacing, const QColor& color) {
            QFont font = this->font();
            font.setPointSizeF(qMax(1.0, size * scale * 72.0 / qMax(1, logicalDpiY())));
            font.setBold(bold);
            font.setFeature(QFont::Tag("tnum"), 1);
            if (spacing > 0) font.setLetterSpacing(QFont::AbsoluteSpacing, spacing * scale);
            const double width = QFontMetricsF(font, this).horizontalAdvance(text);
            QPointF at = device(x, y);
            if (anchor == Anchor::Middle) at.rx() -= width / 2;
            else if (anchor == Anchor::End) at.rx() -= width;
            painter.setFont(font);
            painter.setPen(color);
            painter.drawText(at, text);
        };
        for (const Callout& callout : kCallouts) {
            const std::optional<int> value = num(damage_, callout.part->field);
            drawText(callout.x, callout.y, callout.anchor, QString::fromLatin1(callout.label), 14, false, 1.4, secondary);
            drawText(callout.x, callout.y + 34, callout.anchor, pct(value), 26, true, 0,
                     severityColor(damageSeverity(value)));
        }
        static const char* const kCells[3] = {"TYRE", "BRAKE", "BLISTERS"};
        for (const Wheel& wheel : kWheels) {
            drawText(wheel.tx, wheel.ty, wheel.anchor, QString::fromLatin1(wheel.id).toUpper(), 16, true, 1.6,
                     tnr::themed(wheel.color, wheel.lightColor));
            const std::optional<int> values[3] = {num(damage_, wheel.tyre), num(damage_, wheel.brake),
                                                  num(damage_, wheel.blisters)};
            for (int i = 0; i < 3; ++i) {
                drawText(wheel.cols[i], wheel.ty + 26, Anchor::Start, QString::fromLatin1(kCells[i]), 13, false, 1, secondary);
                drawText(wheel.cols[i], wheel.ty + 50, Anchor::Start, pct(values[i]), 20, true, 0,
                         bandColor(tyreBand(values[i])));
            }
        }
    }

private:
    // Zone styling for the current values. A part with no data is drawn like
    // an undamaged one; its callout shows the missing value.
    void restyle() {
        QHash<QString, QColor> zones;
        for (const BodyPart* part : kBodyParts) {
            const Severity severity = damageSeverity(num(damage_, part->field));
            if (severity == Severity::Warn || severity == Severity::Crit)
                for (const QString& zone : part->zones) zones.insert(zone, severityColor(severity));
        }
        for (const Wheel& wheel : kWheels) {
            // Tyre outlines always take the band colour of the worst wheel value, green included.
            std::optional<int> worst;
            for (int DamageRow::* field : {wheel.tyre, wheel.brake, wheel.blisters})
                if (const std::optional<int> value = num(damage_, field))
                    worst = worst ? std::max(*worst, *value) : *value;
            if (worst) zones.insert(QStringLiteral("t-") + QLatin1String(wheel.id), bandColor(tyreBand(worst)));
        }
        const bool dark = tnr::isDarkTheme();
        const QColor wire(dark ? "#8a93c4" : "#3d434b");
        const QColor tube(dark ? "#4a5175" : "#8f9397");
        const QColor background = palette().color(QPalette::Window);
        QStringList keyParts{wire.name(), tube.name(), background.name()};
        QStringList ids = zones.keys();
        std::sort(ids.begin(), ids.end());
        for (const QString& id : ids) keyParts << id + QLatin1Char('=') + zones.value(id).name();
        const QString key = keyParts.join(QLatin1Char('|'));
        if (key == styleKey_) return;
        styleKey_ = key;
        renderer_.load(styledCarSvg(source_, zones, wire, tube, background));
    }

    QByteArray source_;
    QSvgRenderer renderer_;
    QString styleKey_;
    std::optional<DamageRow> damage_;
};

DamagePage::DamagePage(QWidget* parent) : QWidget(parent) {
    density_ = tnr::densityFromValue(
        settings_.value(tnr::compactKey(tnr::CompactSection::DamageSummary), "normal"));

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    statusBar_ = new QWidget;
    auto* statusLayout = new QHBoxLayout(statusBar_);
    statusLayout->setContentsMargins(0, 0, 0, 0);
    statusLayout->setSpacing(0);
    column->addWidget(statusBar_);
    statusDivider_ = tnrui::hline();
    column->addWidget(statusDivider_);

    diagram_ = new CarDamageDiagram;
    column->addWidget(diagram_, 1);
    diagramSpacer_ = new QWidget;
    diagramSpacer_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    column->addWidget(diagramSpacer_, 1);

    wearDivider_ = tnrui::hline();
    column->addWidget(wearDivider_);
    wearBar_ = new QWidget;
    wearBar_->setAccessibleName(QStringLiteral("Power unit and gearbox wear"));
    auto* wearLayout = new QHBoxLayout(wearBar_);
    wearLayout->setContentsMargins(0, 0, 0, 0);
    wearLayout->setSpacing(0);
    column->addWidget(wearBar_);

    buildCards();
    applyLayout(loadLayout());
    refresh();
}

void DamagePage::buildCards() {
    auto clear = [](QWidget* bar) {
        QLayout* layout = bar->layout();
        while (QLayoutItem* item = layout->takeAt(0)) {
            delete item->widget();
            delete item;
        }
    };
    clear(statusBar_);
    clear(wearBar_);

    static const char* const kStatusUnits[DamageLayout::StatusCount] = {"%", "%", "", "", ""};
    auto* statusLayout = static_cast<QHBoxLayout*>(statusBar_->layout());
    for (int i = 0; i < DamageLayout::StatusCount; ++i) {
        statusSeps_[i] = nullptr;
        if (i > 0) { statusSeps_[i] = tnrui::vline(); statusLayout->addWidget(statusSeps_[i]); }
        statusCards_[i] = new SummaryCard(QString(), QString::fromLatin1(kStatusUnits[i]), density_);
        statusLayout->addWidget(statusCards_[i], 1);
    }
    refreshTitles();

    // One row of Overview stat cards, laid out like the Overview stats row.
    auto* wearLayout = static_cast<QHBoxLayout*>(wearBar_->layout());
    for (int i = 0; i < DamageLayout::WearCount; ++i) {
        wearSeps_[i] = nullptr;
        if (i > 0) { wearSeps_[i] = tnrui::vline(); wearLayout->addWidget(wearSeps_[i]); }
        wearCards_[i] = new SummaryCard(QStringLiteral("%1 (Wear)").arg(QLatin1String(DamageLayout::wearName(i))),
                                        QStringLiteral("%"), density_);
        wearLayout->addWidget(wearCards_[i], 1);
    }
}

void DamagePage::refreshTitles() {
    static const char* const kLabels[DamageLayout::StatusCount] = {"Engine", "Gearbox", "", "ERS", "Engine status"};
    for (int i = 0; i < DamageLayout::StatusCount; ++i) {
        if (!statusCards_[i]) continue;
        // m_drsFault: titled DRS, or Rear Wing for F1 26 sessions (format
        // catalog, like the Overview wing card).
        statusCards_[i]->setLabel(i == DamageLayout::WingFault ? tnr::L(QStringLiteral("ui.damage.wing_fault"))
                                                               : QString::fromLatin1(kLabels[i]));
    }
}

void DamagePage::setDensityMode(tnr::DensityMode mode) {
    if (density_ == mode) return;
    density_ = mode;
    buildCards();
    applyLayout(loadLayout());
    refresh();
}

DamageLayout DamagePage::loadLayout() {
    DamageLayout layout;
    settings_.beginGroup(QStringLiteral("damageLayout"));
    settings_.beginGroup(QStringLiteral("statusCards"));
    for (int i = 0; i < DamageLayout::StatusCount; ++i)
        layout.statusCards[i] = settings_.value(DamageLayout::statusKey(i), true).toBool();
    settings_.endGroup();
    layout.showDiagram = settings_.value(QStringLiteral("showDiagram"), true).toBool();
    settings_.beginGroup(QStringLiteral("wearTiles"));
    for (int i = 0; i < DamageLayout::WearCount; ++i)
        layout.wearTiles[i] = settings_.value(DamageLayout::wearKey(i), true).toBool();
    settings_.endGroup();
    settings_.endGroup();
    return layout;
}

void DamagePage::saveLayout(const DamageLayout& layout) {
    settings_.beginGroup(QStringLiteral("damageLayout"));
    settings_.beginGroup(QStringLiteral("statusCards"));
    for (int i = 0; i < DamageLayout::StatusCount; ++i)
        settings_.setValue(DamageLayout::statusKey(i), layout.statusCards[i]);
    settings_.endGroup();
    settings_.setValue(QStringLiteral("showDiagram"), layout.showDiagram);
    settings_.beginGroup(QStringLiteral("wearTiles"));
    for (int i = 0; i < DamageLayout::WearCount; ++i)
        settings_.setValue(DamageLayout::wearKey(i), layout.wearTiles[i]);
    settings_.endGroup();
    settings_.endGroup();
}

void DamagePage::applyLayout(const DamageLayout& layout) {
    // A separator shows only between two visible cards.
    auto applyRow = [](SummaryCard* const* cards, QFrame* const* seps, const bool* visible, int count) {
        bool any = false;
        for (int i = 0; i < count; ++i) {
            if (cards[i]) cards[i]->setVisible(visible[i]);
            if (seps[i]) seps[i]->setVisible(visible[i] && any);
            any = any || visible[i];
        }
        return any;
    };
    const bool anyStatus = applyRow(statusCards_, statusSeps_, layout.statusCards, DamageLayout::StatusCount);
    statusBar_->setVisible(anyStatus);
    statusDivider_->setVisible(anyStatus);
    diagram_->setVisible(layout.showDiagram);
    diagramSpacer_->setVisible(!layout.showDiagram);
    const bool anyWear = applyRow(wearCards_, wearSeps_, layout.wearTiles, DamageLayout::WearCount);
    wearBar_->setVisible(anyWear);
    wearDivider_->setVisible(anyWear);
}

void DamagePage::applyAndSaveLayout(const DamageLayout& layout) {
    applyLayout(layout);
    saveLayout(layout);
}

void DamagePage::update(const DamageRow* damage) {
    if (damage) damage_ = *damage;
    else damage_.reset();
    refresh();
}

void DamagePage::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) {
        if (diagram_) diagram_->themeChanged();
        refresh();   // severity colours have light-theme variants
    }
}

// The page follows the streamed driver: the player live, the driver
// selector's car in V6 playback. Everything comes from the latest damage row.
void DamagePage::refresh() {
    const std::optional<int> engine = num(damage_, &DamageRow::engine_damage);
    const std::optional<int> gearbox = num(damage_, &DamageRow::gearbox_damage);
    const std::optional<int> drs = num(damage_, &DamageRow::drs_fault);
    const std::optional<int> ers = num(damage_, &DamageRow::ers_fault);
    const std::optional<int> blown = num(damage_, &DamageRow::engine_blown);
    const std::optional<int> seized = num(damage_, &DamageRow::engine_seized);
    // The game sets blown/seized for some failures only (e.g. not an MGU-H
    // failure), so overall engine wear at 100% without either flag reads as FAIL.
    QString engineStatus;
    if (seized && *seized) engineStatus = QStringLiteral("SEIZED");
    else if (blown && *blown) engineStatus = QStringLiteral("BLOWN");
    else if (engine && *engine >= 100) engineStatus = QStringLiteral("FAIL");
    else if (seized || blown) engineStatus = QStringLiteral("OK");

    const auto pctText = [](std::optional<int> v) { return v ? QString::number(*v) : SummaryCard::kMissing; };
    const auto wearColor = [](std::optional<int> v) { return v ? severityColor(wearSeverity(v)) : QColor(); };
    const auto fault = [](std::optional<int> v) {
        return !v ? SummaryCard::kMissing : *v ? QStringLiteral("FAULT") : QStringLiteral("OK");
    };
    const auto faultColor = [](std::optional<int> v) {
        return !v ? QColor() : severityColor(*v ? Severity::Crit : Severity::Ok);
    };

    struct Card { QString value; QColor color; const char* sub; };
    const Card cards[DamageLayout::StatusCount] = {
        {pctText(engine), wearColor(engine), "Overall wear"},
        {pctText(gearbox), wearColor(gearbox), "Wear"},
        {fault(drs), faultColor(drs), "Fault flag"},
        {fault(ers), faultColor(ers), "Fault flag"},
        {engineStatus.isEmpty() ? SummaryCard::kMissing : engineStatus,
         engineStatus.isEmpty() ? QColor()
             : severityColor(engineStatus == QLatin1String("OK") ? Severity::Ok : Severity::Crit),
         "Blown / seized / fail"},
    };
    for (int i = 0; i < DamageLayout::StatusCount; ++i) {
        if (!statusCards_[i]) continue;
        statusCards_[i]->setValue(cards[i].value, cards[i].color);
        statusCards_[i]->setSub(QString::fromLatin1(cards[i].sub));
    }

    int DamageRow::* const wearFields[DamageLayout::WearCount] = {
        &DamageRow::engine_ice_wear, &DamageRow::engine_mguh_wear, &DamageRow::engine_mguk_wear,
        &DamageRow::engine_es_wear, &DamageRow::engine_ce_wear, &DamageRow::engine_tc_wear,
        // engine_damage / gearbox_damage are reported as "damage" but behave as wear.
        &DamageRow::engine_damage, &DamageRow::gearbox_damage,
    };
    for (int i = 0; i < DamageLayout::WearCount; ++i) {
        if (!wearCards_[i]) continue;
        const std::optional<int> value = num(damage_, wearFields[i]);
        wearCards_[i]->setValue(pctText(value), wearColor(value));
    }

    if (diagram_) diagram_->setDamage(damage_);
}
