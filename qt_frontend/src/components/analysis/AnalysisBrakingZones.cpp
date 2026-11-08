#include "AnalysisBrakingZones.h"

#include "AnalysisMapView.h"
#include "AnalysisPanels.h"
#include "AnalysisWidgets.h"

#include <QEvent>
#include <QFontDatabase>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QPainter>
#include <QShowEvent>
#include <QTimer>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>

#include <tnrp/AnalysisMap.h>

namespace {

// Laps this close through a zone count as level, as on the map's card.
constexpr double kLevelS = 0.01;

enum Column { ZoneColumn, AtColumn, GapColumn };

// The list part of the default split.
constexpr double kListShare = 0.6;

// Cells draw without selection, hover or focus panels; the row's selection
// is drawn once across the whole row (ZoneTree::drawRow).
class ZoneDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        option->state &= ~(QStyle::State_Selected | QStyle::State_MouseOver | QStyle::State_HasFocus);
    }
};

// The zone list, its selected row drawn like the segmented controls' selected segment.
class ZoneTree : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;

protected:
    void drawRow(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        if (selectionModel() && selectionModel()->isSelected(index))
            analysisPaintSelectedRow(painter, option.rect, option.palette,
                                     option.state.testFlag(QStyle::State_Enabled), this);
        QTreeWidget::drawRow(painter, option, index);
    }
};

QString metres(double m) { return QLocale().toString(qRound64(m)) + QStringLiteral(" m"); }

} // namespace

AnalysisBrakingZones::AnalysisBrakingZones(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(4);

    summary_ = new QLabel(this);
    summary_->setForegroundRole(QPalette::PlaceholderText);
    summary_->setWordWrap(true);
    root->addWidget(summary_);

    auto* sections = new AnalysisGripSplitter(Qt::Vertical, this);
    sections_ = sections;
    sections->setChildrenCollapsible(false);
    list_ = new ZoneTree(sections);
    list_->setItemDelegate(new ZoneDelegate(list_));
    list_->setRootIsDecorated(false);
    list_->setUniformRowHeights(true);
    list_->setAllColumnsShowFocus(false);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setAccessibleName(QStringLiteral("Braking zones"));
    list_->setHeaderLabels({QStringLiteral("Zone"), QStringLiteral("At"), QStringLiteral("Gap")});
    list_->header()->setStretchLastSection(false);
    list_->header()->setSectionResizeMode(ZoneColumn, QHeaderView::ResizeToContents);
    list_->header()->setSectionResizeMode(AtColumn, QHeaderView::Stretch);
    list_->header()->setSectionResizeMode(GapColumn, QHeaderView::ResizeToContents);
    list_->headerItem()->setTextAlignment(GapColumn, Qt::AlignRight | Qt::AlignVCenter);

    detailScroll_ = new QScrollArea(sections);
    detailScroll_->setWidgetResizable(true);
    detailScroll_->setFrameShape(QFrame::StyledPanel);
    detail_ = new QLabel(detailScroll_);
    detail_->setTextFormat(Qt::RichText);
    detail_->setWordWrap(true);
    detail_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    detail_->setMargin(6);
    detail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detailScroll_->setWidget(detail_);
    sections->addWidget(list_);
    sections->addWidget(detailScroll_);
    sections->setStretchFactor(0, 3);   // 60 / 40 as the panel grows or shrinks
    sections->setStretchFactor(1, 2);
    root->addWidget(sections, 1);
    connect(sections, &QSplitter::splitterMoved, this, &AnalysisBrakingZones::splitChanged);

    connect(list_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        showDetail();
        if (item) emit zoneActivated(item->data(ZoneColumn, Qt::UserRole + 1).toDouble());
    });
    rebuild();
}

void AnalysisBrakingZones::setResult(std::shared_ptr<const tnrp::AnalysisMapResult> result) {
    if (result == result_) return;
    result_ = std::move(result);
    rebuild();
}

void AnalysisBrakingZones::setLaps(const QString& currentLabel, const QColor& currentColor,
                                   const QString& comparisonLabel, const QColor& comparisonColor) {
    if (currentLabel == currentLabel_ && comparisonLabel == comparisonLabel_ &&
        currentColor == currentColor_ && comparisonColor == comparisonColor_) return;
    currentLabel_ = currentLabel;
    comparisonLabel_ = comparisonLabel;
    currentColor_ = currentColor;
    comparisonColor_ = comparisonColor;
    rebuild();
}

QByteArray AnalysisBrakingZones::splitState() const {
    return sections_->saveState();
}

void AnalysisBrakingZones::restoreSplitState(const QByteArray& state) {
    if (!state.isEmpty() && sections_->restoreState(state)) splitApplied_ = true;
}

// Without a saved split, the list takes 60% and the detail 40% the first time
// the panel has a height to share.
void AnalysisBrakingZones::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (splitApplied_) return;
    // After the layout has given the splitter its height.
    QTimer::singleShot(0, this, [this] {
        if (splitApplied_) return;
        const int total = sections_->height() - sections_->handleWidth();
        if (total <= 0) return;
        const int list = qRound(total * kListShare);
        sections_->setSizes({list, total - list});
        splitApplied_ = true;
    });
}

void AnalysisBrakingZones::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) showDetail();
}

void AnalysisBrakingZones::rebuild() {
    const int keep = list_->currentItem() ? list_->indexOfTopLevelItem(list_->currentItem()) : 0;
    QSignalBlocker guard(list_);
    list_->clear();

    const tnrp::AnalysisMapResult* result = result_ && result_->hasCurrent ? result_.get() : nullptr;
    if (!result) {
        summary_->setText(QStringLiteral("Braking zones show once a lap is chosen."));
        detail_->clear();
        return;
    }
    const tnrp::BrakingPointsMap& braking = result->braking;
    const bool paired = !braking.comparison.empty();
    list_->headerItem()->setText(GapColumn, paired ? QStringLiteral("Gap") : QStringLiteral("Entry"));
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    int currentQuicker = 0;
    int comparisonQuicker = 0;
    const auto add = [&](int zone, double atM, const QString& value, const QColor& color) {
        auto* item = new QTreeWidgetItem(list_);
        item->setText(ZoneColumn, QString::number(zone));
        item->setText(AtColumn, metres(atM));
        item->setText(GapColumn, value);
        item->setData(ZoneColumn, Qt::UserRole + 1, atM);
        item->setFont(AtColumn, mono);
        item->setFont(GapColumn, mono);
        item->setTextAlignment(GapColumn, Qt::AlignRight | Qt::AlignVCenter);
        if (color.isValid()) {
            item->setForeground(GapColumn, color);
            QFont bold = mono;
            bold.setBold(true);
            item->setFont(GapColumn, bold);
        }
    };
    if (paired) {
        for (const tnrp::BrakingPointPair& pair : braking.pairs) {
            const bool timed = pair.currentTimeS >= 0 && pair.comparisonTimeS >= 0;
            // Positive: the current lap was slower through the zone, as the delta reads.
            const double gap = timed ? double(pair.currentTimeS) - pair.comparisonTimeS : qQNaN();
            QColor color;
            if (timed && std::abs(gap) >= kLevelS) {
                color = gap > 0 ? comparisonColor_ : currentColor_;
                ++(gap > 0 ? comparisonQuicker : currentQuicker);
            }
            add(pair.current + 1, pair.fromM, timed ? analysis::formatDelta(gap) : QStringLiteral("—"), color);
        }
        summary_->setText(QStringLiteral("%1 zones · %2 quicker in %3, %4 in %5")
                              .arg(braking.pairs.size())
                              .arg(currentLabel_).arg(currentQuicker)
                              .arg(comparisonLabel_).arg(comparisonQuicker));
    } else {
        for (size_t i = 0; i < braking.current.size(); ++i) {
            const tnrp::BrakingPoint& point = braking.current[i];
            add(int(i) + 1, point.distanceM, QStringLiteral("%1 km/h").arg(qRound(point.speedKph)), {});
        }
        summary_->setText(QStringLiteral("%1 zones · choose a lap to compare for the gaps")
                              .arg(braking.current.size()));
    }
    if (list_->topLevelItemCount() > 0)
        list_->setCurrentItem(list_->topLevelItem(qBound(0, keep, list_->topLevelItemCount() - 1)));
    showDetail();
}

void AnalysisBrakingZones::showDetail() {
    QTreeWidgetItem* item = list_->currentItem();
    if (!result_ || !item) {
        detail_->clear();
        return;
    }
    detail_->setText(AnalysisMapView::brakingZoneCard(*result_, list_->indexOfTopLevelItem(item),
                                                      currentLabel_, currentColor_, comparisonLabel_,
                                                      comparisonColor_, palette()));
}
