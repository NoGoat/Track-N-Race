#pragma once

#include "../AnalyzeMetrics.h"

#include <QAbstractListModel>
#include <QSet>
#include <QStyledItemDelegate>
#include <QVector>

// The charted metrics, in chart order. Delta is always present: it can be
// hidden and recoloured but never removed. A tyre row may appear either as
// separate corner series or as one combined series holding picked corners.
class AnalysisSeriesModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        MetricIdRole = Qt::UserRole + 1,
        SubtitleRole,
        ColorRole,
        NegativeColorRole,   // Delta only: colour of time gained
        ShowYAxisRole,
        RemovableRole,
        CornerColorsRole,    // combined only: QVariantList of four colours, FL..RR
        CornersRole,         // combined only: QStringList of picked corner keys
    };

    static const QColor kDeltaPositive;
    static const QColor kDeltaNegative;

    explicit AnalysisSeriesModel(QObject* parent = nullptr);

    const QVector<AnalyzeSeriesSetting>& series() const { return series_; }
    void setSeries(const QVector<AnalyzeSeriesSetting>& series);

    bool contains(const QString& metricId) const { return rowOf(metricId) >= 0; }
    int rowOf(const QString& metricId) const;
    QSet<QString> selectedIds() const;

    // Adds or removes series. Adding displaces conflicting series (a combined
    // tyre series and its own corners draw the same lines).
    void setMetricsSelected(const QStringList& ids, bool selected);
    void addMetric(const QString& metricId) { setMetricsSelected({metricId}, true); }
    void removeRowAt(int row);

    // Tyre-row editing, as in the metric picker's corner matrix.
    bool isCombined(const QString& idPrefix) const;
    bool cornerPicked(const QString& idPrefix, const QString& cornerKey) const;
    void toggleTyreCorner(const QString& idPrefix, const QString& cornerKey);
    void toggleTyreAllCorners(const QString& idPrefix);
    void toggleTyreCombined(const QString& idPrefix);

    void setColor(int row, const QColor& color, bool negative = false);
    void setCornerColor(int row, const QString& cornerKey, const QColor& color);
    void resetColors(int row);
    void setShowYAxis(int row, bool on);
    void setAllYAxes(bool on);
    bool allYAxesShown() const;
    // Delta needs lap-distance data; without it the row explains why it is empty.
    void setDeltaSupported(bool supported);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    Qt::DropActions supportedDropActions() const override { return Qt::MoveAction; }
    bool moveRows(const QModelIndex& sourceParent, int sourceRow, int count,
                  const QModelIndex& destinationParent, int destinationChild) override;

signals:
    // Any user-visible edit: order, colour, visibility, axis, membership.
    void seriesEdited();

private:
    QVector<AnalyzeSeriesSetting> series_;
    bool deltaSupported_ = true;

    void replaceSeries(const QVector<AnalyzeSeriesSetting>& series);
    void emitRowChanged(int row);
};

// Two-line metric row: check box (visibility), colour swatch, name, and a muted
// "group · unit" line, drawn with the active style's item panel so selection,
// hover and focus look like every other item view in the application. A
// combined tyre series shows one swatch per corner and its name in its colour.
class AnalysisSeriesDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

    static QString negativePart() { return QStringLiteral("negative"); }

signals:
    // A colour target was clicked: {} for the series colour, negativePart() for
    // Delta's faster colour, or a corner key for a combined series' corner.
    void swatchClicked(const QModelIndex& index, const QString& part);

protected:
    bool editorEvent(QEvent* event, QAbstractItemModel* model,
                     const QStyleOptionViewItem& option, const QModelIndex& index) override;

private:
    struct Geometry {
        QRect swatch;            // single-colour series and Delta
        QVector<QRect> corners;  // combined series
        QRect text;
    };
    Geometry geometry(const QStyleOptionViewItem& option, const QModelIndex& index) const;
};
