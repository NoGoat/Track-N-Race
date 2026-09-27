#include "AnalysisSeriesModel.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>

#include <algorithm>

namespace {
constexpr int kPadding = 6;
constexpr int kGap = 8;
constexpr int kSwatch = 16;
constexpr int kCornerSwatch = 12;
constexpr int kCornerGap = 3;
const QString kDelta = QStringLiteral("delta");

QFont subtitleFont(const QFont& base) {
    QFont font = base;
    font.setPointSizeF(qMax(7.0, base.pointSizeF() * 0.85));
    return font;
}

QString cornerId(const AnalyzeTyreRow& row, const QString& key) {
    return row.idPrefix + QLatin1Char('-') + key;
}

QStringList allCornerKeys() {
    QStringList keys;
    for (const AnalyzeTyreCorner& corner : analyzeTyreCorners()) keys << corner.key;
    return keys;
}

// The two text lines of a row, stacked and vertically centred as a block.
struct TextBlock {
    QRect title;
    QRect subtitle;
};
TextBlock textBlock(const QRect& text, const QFont& font) {
    const QFontMetrics titleMetrics(font);
    const QFontMetrics subtitleMetrics(subtitleFont(font));
    const int block = titleMetrics.height() + subtitleMetrics.height();
    const int top = text.top() + (text.height() - block) / 2;
    return {QRect(text.left(), top, text.width(), titleMetrics.height()),
            QRect(text.left(), top + titleMetrics.height(), text.width(), subtitleMetrics.height())};
}
}

const QColor AnalysisSeriesModel::kDeltaPositive("#C4162A");
const QColor AnalysisSeriesModel::kDeltaNegative("#37872D");

AnalysisSeriesModel::AnalysisSeriesModel(QObject* parent) : QAbstractListModel(parent) {}

void AnalysisSeriesModel::setSeries(const QVector<AnalyzeSeriesSetting>& series) {
    QVector<AnalyzeSeriesSetting> clean;
    QSet<QString> seen;
    for (AnalyzeSeriesSetting setting : series) {
        const QString id = setting.metricId;
        const AnalyzeTyreRow* combined = analyzeCombinedRow(id);
        if (id != kDelta && !combined && !analyzeMetric(id)) continue;
        if (seen.contains(id)) continue;
        const QStringList conflicts = analyzeSeriesConflicts(id);
        if (std::any_of(conflicts.cbegin(), conflicts.cend(),
                        [&](const QString& other) { return seen.contains(other); }))
            continue;
        seen.insert(id);
        if (combined) {
            setting.corners = analyzeSanitizeCorners(setting.corners);
            if (!setting.color.isValid()) setting.color = combined->combinedColor;
        } else {
            setting.corners.clear();
            setting.cornerColors.clear();
        }
        clean.push_back(setting);
    }
    if (!seen.contains(kDelta))
        clean.push_back({kDelta, kDeltaPositive, kDeltaNegative, true, true, {}, {}});
    beginResetModel();
    series_ = clean;
    endResetModel();
}

int AnalysisSeriesModel::rowOf(const QString& metricId) const {
    for (int row = 0; row < series_.size(); ++row)
        if (series_[row].metricId == metricId) return row;
    return -1;
}

QSet<QString> AnalysisSeriesModel::selectedIds() const {
    QSet<QString> ids;
    for (const AnalyzeSeriesSetting& setting : series_) ids.insert(setting.metricId);
    return ids;
}

void AnalysisSeriesModel::replaceSeries(const QVector<AnalyzeSeriesSetting>& series) {
    beginResetModel();
    series_ = series;
    endResetModel();
    emit seriesEdited();
}

void AnalysisSeriesModel::setMetricsSelected(const QStringList& ids, bool selected) {
    if (!selected) {
        QSet<QString> removed(ids.cbegin(), ids.cend());
        removed.remove(kDelta);
        QVector<AnalyzeSeriesSetting> next;
        for (const AnalyzeSeriesSetting& setting : series_)
            if (!removed.contains(setting.metricId)) next.push_back(setting);
        if (next.size() != series_.size()) replaceSeries(next);
        return;
    }
    QSet<QString> charted = selectedIds();
    QSet<QString> displaced;
    QVector<AnalyzeSeriesSetting> added;
    for (const QString& id : ids) {
        const AnalyzeTyreRow* combined = analyzeCombinedRow(id);
        const AnalyzeMetric* metric = analyzeMetric(id);
        if ((!combined && !metric) || charted.contains(id)) continue;
        charted.insert(id);
        // A combined series and its own corners draw the same lines.
        for (const QString& conflict : analyzeSeriesConflicts(id)) displaced.insert(conflict);
        if (combined)
            added.push_back({id, combined->combinedColor, QColor(), true, true, allCornerKeys(), {}});
        else
            added.push_back({id, metric->defaultColor, QColor(), true, true, {}, {}});
    }
    if (added.isEmpty()) return;
    QVector<AnalyzeSeriesSetting> next;
    for (const AnalyzeSeriesSetting& setting : series_)
        if (!displaced.contains(setting.metricId)) next.push_back(setting);
    next += added;
    replaceSeries(next);
}

void AnalysisSeriesModel::removeRowAt(int row) {
    if (row < 0 || row >= series_.size() || series_[row].metricId == kDelta) return;
    beginRemoveRows(QModelIndex(), row, row);
    series_.removeAt(row);
    endRemoveRows();
    emit seriesEdited();
}

bool AnalysisSeriesModel::isCombined(const QString& idPrefix) const {
    return contains(idPrefix + QStringLiteral("-all"));
}

bool AnalysisSeriesModel::cornerPicked(const QString& idPrefix, const QString& cornerKey) const {
    const int row = rowOf(idPrefix + QStringLiteral("-all"));
    if (row >= 0) return series_[row].corners.contains(cornerKey);
    return contains(idPrefix + QLatin1Char('-') + cornerKey);
}

void AnalysisSeriesModel::toggleTyreCorner(const QString& idPrefix, const QString& cornerKey) {
    const int row = rowOf(idPrefix + QStringLiteral("-all"));
    if (row < 0) {
        // Not combined: each corner is its own series.
        const QString id = idPrefix + QLatin1Char('-') + cornerKey;
        setMetricsSelected({id}, !contains(id));
        return;
    }
    QStringList corners = series_[row].corners;
    if (corners.contains(cornerKey)) corners.removeAll(cornerKey);
    else corners << cornerKey;
    series_[row].corners = analyzeSanitizeCorners(corners);
    emitRowChanged(row);
}

void AnalysisSeriesModel::toggleTyreAllCorners(const QString& idPrefix) {
    const int row = rowOf(idPrefix + QStringLiteral("-all"));
    const QStringList keys = allCornerKeys();
    if (row >= 0) {
        const bool all = series_[row].corners.size() == keys.size();
        series_[row].corners = all ? QStringList() : keys;
        emitRowChanged(row);
        return;
    }
    QStringList ids;
    for (const QString& key : keys) ids << idPrefix + QLatin1Char('-') + key;
    const bool all = std::all_of(ids.cbegin(), ids.cend(),
                                 [this](const QString& id) { return contains(id); });
    setMetricsSelected(ids, !all);
}

void AnalysisSeriesModel::toggleTyreCombined(const QString& idPrefix) {
    const QString combinedId = idPrefix + QStringLiteral("-all");
    const AnalyzeTyreRow* tyreRow = analyzeCombinedRow(combinedId);
    if (!tyreRow) return;
    const int row = rowOf(combinedId);
    if (row >= 0) {
        // Split the combined card back into a card per picked corner, in place.
        const AnalyzeSeriesSetting combined = series_[row];
        QVector<AnalyzeSeriesSetting> next = series_;
        next.removeAt(row);
        int insertAt = row;
        for (const QString& key : combined.corners) {
            const QString id = cornerId(*tyreRow, key);
            if (!analyzeMetric(id)) continue;
            next.insert(insertAt++, AnalyzeSeriesSetting{id, analyzeSeriesLineColor(combined, id), QColor(),
                                     combined.visible, combined.showYAxis, {}, {}});
        }
        replaceSeries(next);
        return;
    }
    // Merge the row's charted corners into one card where the first of them sat.
    QVector<AnalyzeSeriesSetting> members;
    QVector<AnalyzeSeriesSetting> rest;
    int insertAt = -1;
    for (const AnalyzeSeriesSetting& setting : series_) {
        if (analyzeCornerRow(setting.metricId) == tyreRow) {
            if (insertAt < 0) insertAt = static_cast<int>(rest.size());
            members.push_back(setting);
        } else {
            rest.push_back(setting);
        }
    }
    AnalyzeSeriesSetting merged{combinedId, tyreRow->combinedColor, QColor(), true, true, {}, {}};
    QStringList corners;
    for (const AnalyzeSeriesSetting& member : members) {
        QString key;
        analyzeCornerRow(member.metricId, &key);
        corners << key;
        const AnalyzeMetric* metric = analyzeMetric(member.metricId);
        if (metric && member.color != metric->defaultColor) merged.cornerColors.insert(key, member.color);
    }
    merged.corners = analyzeSanitizeCorners(corners);
    if (!members.isEmpty()) {
        merged.visible = std::any_of(members.cbegin(), members.cend(),
                                     [](const AnalyzeSeriesSetting& s) { return s.visible; });
        merged.showYAxis = std::any_of(members.cbegin(), members.cend(),
                                       [](const AnalyzeSeriesSetting& s) { return s.showYAxis; });
    }
    rest.insert(insertAt < 0 ? rest.size() : insertAt, merged);
    replaceSeries(rest);
}

void AnalysisSeriesModel::setColor(int row, const QColor& color, bool negative) {
    if (row < 0 || row >= series_.size() || !color.isValid()) return;
    AnalyzeSeriesSetting& setting = series_[row];
    if (negative && setting.metricId != kDelta) return;
    QColor& target = negative ? setting.negativeColor : setting.color;
    if (target == color) return;
    target = color;
    emitRowChanged(row);
}

void AnalysisSeriesModel::setCornerColor(int row, const QString& cornerKey, const QColor& color) {
    if (row < 0 || row >= series_.size() || !color.isValid()) return;
    AnalyzeSeriesSetting& setting = series_[row];
    if (!analyzeCombinedRow(setting.metricId)) return;
    setting.cornerColors.insert(cornerKey, color);
    emitRowChanged(row);
}

void AnalysisSeriesModel::resetColors(int row) {
    if (row < 0 || row >= series_.size()) return;
    AnalyzeSeriesSetting& setting = series_[row];
    if (setting.metricId == kDelta) {
        setting.color = kDeltaPositive;
        setting.negativeColor = kDeltaNegative;
    } else if (const AnalyzeTyreRow* combined = analyzeCombinedRow(setting.metricId)) {
        setting.color = combined->combinedColor;
        setting.cornerColors.clear();
    } else if (const AnalyzeMetric* metric = analyzeMetric(setting.metricId)) {
        setting.color = metric->defaultColor;
    }
    emitRowChanged(row);
}

void AnalysisSeriesModel::setShowYAxis(int row, bool on) {
    if (row < 0 || row >= series_.size() || series_[row].showYAxis == on) return;
    series_[row].showYAxis = on;
    emitRowChanged(row);
}

void AnalysisSeriesModel::setAllYAxes(bool on) {
    if (series_.isEmpty()) return;
    for (AnalyzeSeriesSetting& setting : series_) setting.showYAxis = on;
    emit dataChanged(index(0), index(static_cast<int>(series_.size()) - 1));
    emit seriesEdited();
}

bool AnalysisSeriesModel::allYAxesShown() const {
    return std::all_of(series_.cbegin(), series_.cend(),
                       [](const AnalyzeSeriesSetting& setting) { return setting.showYAxis; });
}

void AnalysisSeriesModel::setDeltaSupported(bool supported) {
    if (deltaSupported_ == supported) return;
    deltaSupported_ = supported;
    const int row = rowOf(kDelta);
    if (row >= 0) emit dataChanged(index(row), index(row));
}

int AnalysisSeriesModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(series_.size());
}

QVariant AnalysisSeriesModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= series_.size()) return {};
    const AnalyzeSeriesSetting& setting = series_[index.row()];
    const bool delta = setting.metricId == kDelta;
    const AnalyzeTyreRow* combined = analyzeCombinedRow(setting.metricId);
    const AnalyzeMetric* metric = delta || combined ? nullptr : analyzeMetric(setting.metricId);

    const auto title = [&] {
        return delta ? QStringLiteral("Delta") : combined ? combined->label : metric->label;
    };
    const auto subtitle = [&] {
        QString text;
        if (delta) {
            text = deltaSupported_ ? QStringLiteral("Time gap · slower / faster")
                                   : QStringLiteral("Needs lap distance data");
        } else if (combined) {
            QStringList picked;
            for (const AnalyzeTyreCorner& corner : analyzeTyreCorners())
                if (setting.corners.contains(corner.key)) picked << corner.label;
            text = QStringLiteral("Combined · ") +
                   (picked.size() == analyzeTyreCorners().size() ? QStringLiteral("All")
                    : picked.isEmpty() ? QStringLiteral("None") : picked.join(QLatin1Char(' ')));
        } else {
            text = metric->group;
            if (!metric->unit.isEmpty()) text += QStringLiteral(" · ") + metric->unit;
        }
        if (!setting.showYAxis) text += QStringLiteral(" · no axis");
        return text;
    };

    switch (role) {
    case Qt::DisplayRole:
        return title();
    case SubtitleRole:
        return subtitle();
    case Qt::ToolTipRole: {
        const QString how = delta
            ? QStringLiteral("Click the left half of the swatch for the slower colour, the right "
                             "half for the faster colour.")
            : combined
            ? QStringLiteral("Click a corner swatch to recolour that corner, or the name to "
                             "recolour the axis. Drag to reorder.")
            : QStringLiteral("Click the swatch to change its colour. Drag to reorder.");
        return QStringLiteral("<b>%1</b><br>%2<br>%3")
            .arg(title().toHtmlEscaped(), subtitle().toHtmlEscaped(), how);
    }
    case Qt::AccessibleTextRole:
        return title() + QStringLiteral(", ") + subtitle();
    case Qt::CheckStateRole:
        return setting.visible ? Qt::Checked : Qt::Unchecked;
    case MetricIdRole:
        return setting.metricId;
    case ColorRole:
        return setting.color;
    case NegativeColorRole:
        return delta ? QVariant(setting.negativeColor) : QVariant();
    case ShowYAxisRole:
        return setting.showYAxis;
    case RemovableRole:
        return !delta;
    case CornerColorsRole: {
        if (!combined) return {};
        QVariantList colors;
        for (const AnalyzeTyreCorner& corner : analyzeTyreCorners())
            colors << analyzeSeriesLineColor(setting, cornerId(*combined, corner.key));
        return colors;
    }
    case CornersRole:
        return combined ? QVariant(setting.corners) : QVariant();
    default:
        return {};
    }
}

bool AnalysisSeriesModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (!index.isValid() || index.row() >= series_.size()) return false;
    AnalyzeSeriesSetting& setting = series_[index.row()];
    if (role == Qt::CheckStateRole) {
        const bool visible = static_cast<Qt::CheckState>(value.toInt()) == Qt::Checked;
        if (setting.visible == visible) return true;
        setting.visible = visible;
        emitRowChanged(index.row());
        return true;
    }
    if (role == ShowYAxisRole) {
        setShowYAxis(index.row(), value.toBool());
        return true;
    }
    return false;
}

Qt::ItemFlags AnalysisSeriesModel::flags(const QModelIndex& index) const {
    // Rows accept drops only *between* them (the root), never onto one another.
    if (!index.isValid()) return Qt::ItemIsDropEnabled;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable |
           Qt::ItemIsDragEnabled;
}

bool AnalysisSeriesModel::moveRows(const QModelIndex& sourceParent, int sourceRow, int count,
                                   const QModelIndex& destinationParent,
                                   int destinationChild) {
    const int rows = static_cast<int>(series_.size());
    if (sourceParent.isValid() || destinationParent.isValid() || count != 1) return false;
    if (sourceRow < 0 || sourceRow >= rows || destinationChild < 0 || destinationChild > rows)
        return false;
    if (destinationChild == sourceRow || destinationChild == sourceRow + 1) return false;
    if (!beginMoveRows(QModelIndex(), sourceRow, sourceRow, QModelIndex(), destinationChild))
        return false;
    series_.move(sourceRow, destinationChild > sourceRow ? destinationChild - 1 : destinationChild);
    endMoveRows();
    emit seriesEdited();
    return true;
}

void AnalysisSeriesModel::emitRowChanged(int row) {
    emit dataChanged(index(row), index(row));
    emit seriesEdited();
}

// ── Delegate ────────────────────────────────────────────────────────────────

AnalysisSeriesDelegate::Geometry
AnalysisSeriesDelegate::geometry(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    const QWidget* widget = option.widget;
    const QStyle* style = widget ? widget->style() : QApplication::style();
    const QRect check = style->subElementRect(QStyle::SE_ItemViewItemCheckIndicator,
                                              &option, widget);
    const int left = (check.isValid() ? check.right() + 1 : option.rect.left() + kPadding) + kGap;
    Geometry out;
    int swatchesRight = left;
    if (index.data(AnalysisSeriesModel::CornerColorsRole).isValid()) {
        const int top = option.rect.center().y() - kCornerSwatch / 2 + 1;
        for (int corner = 0; corner < analyzeTyreCorners().size(); ++corner) {
            const QRect rect(left + corner * (kCornerSwatch + kCornerGap), top, kCornerSwatch,
                             kCornerSwatch);
            out.corners.push_back(rect);
            swatchesRight = rect.right() + 1;
        }
    } else {
        out.swatch = QRect(left, option.rect.center().y() - kSwatch / 2 + 1, kSwatch, kSwatch);
        swatchesRight = out.swatch.right() + 1;
    }
    out.text = QRect(swatchesRight + kGap, option.rect.top(),
                     option.rect.right() - swatchesRight - kGap - kPadding, option.rect.height());
    return out;
}

void AnalysisSeriesDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                   const QModelIndex& index) const {
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    const QWidget* widget = opt.widget;
    QStyle* style = widget ? widget->style() : QApplication::style();

    // Let the style draw panel, selection, focus and check box; we draw the rest.
    const QString title = opt.text;
    opt.text.clear();
    opt.icon = QIcon();
    opt.features &= ~(QStyleOptionViewItem::HasDisplay | QStyleOptionViewItem::HasDecoration);
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

    const Geometry g = geometry(opt, index);
    const bool visible =
        static_cast<Qt::CheckState>(index.data(Qt::CheckStateRole).toInt()) == Qt::Checked;
    const bool selected = opt.state.testFlag(QStyle::State_Selected);
    const QPalette::ColorGroup group = !opt.state.testFlag(QStyle::State_Enabled)
        ? QPalette::Disabled
        : opt.state.testFlag(QStyle::State_Active) ? QPalette::Active : QPalette::Inactive;
    const QColor rim = opt.palette.color(group, QPalette::Mid);
    const QColor color = index.data(AnalysisSeriesModel::ColorRole).value<QColor>();

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    // One rounded swatch; Delta's is split into its slower (left) and faster halves.
    auto drawSwatch = [&](const QRect& rect, const QColor& fill, const QColor* rightHalf,
                          double opacity) {
        QPainterPath path;
        path.addRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        painter->setOpacity(opacity);
        painter->setClipPath(path);
        painter->fillRect(rect, rightHalf ? *rightHalf : fill);
        if (rightHalf) {
            QRect left = rect;
            left.setRight(rect.center().x());
            painter->fillRect(left, fill);
        }
        painter->setClipping(false);
        painter->setOpacity(1.0);
        painter->setPen(rim);
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(path);
    };

    if (!g.corners.isEmpty()) {
        // Combined tyre series: a swatch per corner; corners it does not draw are faded.
        const QVariantList colors = index.data(AnalysisSeriesModel::CornerColorsRole).toList();
        const QStringList picked = index.data(AnalysisSeriesModel::CornersRole).toStringList();
        const auto& corners = analyzeTyreCorners();
        for (int corner = 0; corner < g.corners.size() && corner < colors.size(); ++corner) {
            const bool on = picked.contains(corners[corner].key);
            drawSwatch(g.corners[corner], colors[corner].value<QColor>(), nullptr,
                       visible && on ? 1.0 : 0.25);
        }
    } else {
        const QVariant negative = index.data(AnalysisSeriesModel::NegativeColorRole);
        const QColor faster = negative.value<QColor>();
        drawSwatch(g.swatch, color, negative.isValid() ? &faster : nullptr, visible ? 1.0 : 0.35);
    }

    // A combined card's name carries its own colour, which is also its axis's.
    const TextBlock text = textBlock(g.text, opt.font);
    QColor titleColor = opt.palette.color(group, selected ? QPalette::HighlightedText : QPalette::Text);
    if (!g.corners.isEmpty() && visible && !selected && color.isValid()) titleColor = color;
    QColor subtitleColor = selected ? opt.palette.color(group, QPalette::HighlightedText)
                                    : opt.palette.color(group, QPalette::PlaceholderText);
    if (selected) subtitleColor.setAlphaF(0.75);

    QFont titleFont = opt.font;
    if (!g.corners.isEmpty()) titleFont.setBold(true);
    painter->setFont(titleFont);
    painter->setPen(titleColor);
    painter->drawText(text.title, Qt::AlignLeft | Qt::AlignVCenter,
                      QFontMetrics(titleFont).elidedText(title, Qt::ElideRight, text.title.width()));
    const QFont subtitleFontValue = subtitleFont(opt.font);
    painter->setFont(subtitleFontValue);
    painter->setPen(subtitleColor);
    const QString subtitle = index.data(AnalysisSeriesModel::SubtitleRole).toString();
    painter->drawText(text.subtitle, Qt::AlignLeft | Qt::AlignVCenter,
                      QFontMetrics(subtitleFontValue)
                          .elidedText(subtitle, Qt::ElideRight, text.subtitle.width()));
    painter->restore();
}

QSize AnalysisSeriesDelegate::sizeHint(const QStyleOptionViewItem& option,
                                       const QModelIndex&) const {
    const QFontMetrics titleMetrics(option.font);
    const QFontMetrics subtitleMetrics(subtitleFont(option.font));
    return QSize(160, titleMetrics.height() + subtitleMetrics.height() + 2 * kPadding);
}

bool AnalysisSeriesDelegate::editorEvent(QEvent* event, QAbstractItemModel* model,
                                         const QStyleOptionViewItem& option,
                                         const QModelIndex& index) {
    if (event->type() == QEvent::MouseButtonRelease) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        const QPoint pos = mouse->position().toPoint();
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const Geometry g = geometry(opt, index);
        if (mouse->button() == Qt::LeftButton) {
            if (!g.corners.isEmpty()) {
                const auto& corners = analyzeTyreCorners();
                for (int corner = 0; corner < g.corners.size(); ++corner) {
                    if (!g.corners[corner].contains(pos)) continue;
                    emit swatchClicked(index, corners[corner].key);
                    return true;
                }
                // The combined card's name is its own colour control.
                QFont titleFont = opt.font;
                titleFont.setBold(true);
                QRect title = textBlock(g.text, opt.font).title;
                title.setWidth(qMin(title.width(),
                                    QFontMetrics(titleFont).horizontalAdvance(opt.text)));
                if (title.contains(pos)) {
                    emit swatchClicked(index, {});
                    return true;
                }
            } else if (g.swatch.contains(pos)) {
                const bool negative = index.data(AnalysisSeriesModel::NegativeColorRole).isValid() &&
                                      pos.x() > g.swatch.center().x();
                emit swatchClicked(index, negative ? negativePart() : QString());
                return true;
            }
        }
    }
    return QStyledItemDelegate::editorEvent(event, model, option, index);
}
