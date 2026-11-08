#include "AnalysisLapSlot.h"
#include "AnalysisWidgets.h"

#include <QAbstractButton>
#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStyleOptionComboBox>
#include <QStylePainter>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
constexpr int kLabelLimit = 40;
constexpr int kSwatchSize = 20;
constexpr int kCompoundColorRole = Qt::UserRole + 1;
constexpr int kCompoundLengthRole = Qt::UserRole + 2;
const QString kSeparator = QStringLiteral(" · ");

// The part of a line drawn in the tyre compound's colour.
struct TintedSpan {
    int start = 0;
    int length = 0;
    QColor color;
};

void paintTintedText(QPainter& painter, const QRect& rect, const QString& text, const TintedSpan& span) {
    const int flags = Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine;
    const QFontMetrics metrics(painter.font());
    const QString shown = metrics.elidedText(text, Qt::ElideRight, rect.width());
    const int start = qMin(span.start, int(shown.size()));
    const int end = qMin(span.start + span.length, int(shown.size()));
    if (!span.color.isValid() || end <= start) {
        painter.drawText(rect, flags, shown);
        return;
    }
    const QColor textColor = painter.pen().color();
    QRect remaining = rect;
    auto draw = [&](const QString& piece, const QColor& color) {
        painter.setPen(color);
        painter.drawText(remaining, flags, piece);
        remaining.setLeft(remaining.left() + metrics.horizontalAdvance(piece));
    };
    draw(shown.left(start), textColor);
    draw(shown.mid(start, end - start), span.color);
    draw(shown.mid(end), textColor);
    painter.setPen(textColor);
}

TintedSpan compoundSpan(const QAbstractItemModel* model, const QModelIndex& index, int start = 0) {
    return {start, model->data(index, kCompoundLengthRole).toInt(),
            model->data(index, kCompoundColorRole).value<QColor>()};
}

// "C3 · Lap 7 · 1:46.272 · FL"
QString lapText(const AnalysisLapChoice& lap, const QString& time) {
    QStringList parts;
    if (!lap.compound.isEmpty()) parts << lap.compound;
    parts << QStringLiteral("Lap %1").arg(lap.lapNum) << time;
    if (lap.fastest) parts << QStringLiteral("FL");
    return parts.join(kSeparator);
}

void setRowCompound(QComboBox* box, int row, const AnalysisLapChoice& lap) {
    box->setItemData(row, lap.compoundColor, kCompoundColorRole);
    box->setItemData(row, int(lap.compound.size()), kCompoundLengthRole);
}

QString driverKey(const AnalysisDriverRef& ref) {
    return QStringLiteral("%1:%2").arg(ref.secondary ? 2 : 1).arg(ref.driverIndex);
}

AnalysisDriverRef parseDriverKey(const QString& key) {
    const QStringList parts = key.split(':');
    if (parts.size() != 2) return {};
    bool ok = false;
    const int index = parts[1].toInt(&ok);
    if (!ok || (parts[0] != QLatin1String("1") && parts[0] != QLatin1String("2"))) return {};
    return {true, parts[0] == QLatin1String("2"), index};
}

void addHeader(QComboBox* box, const QString& text) {
    box->addItem(text);
    auto* model = qobject_cast<QStandardItemModel*>(box->model());
    if (!model) return;
    QStandardItem* item = model->item(box->count() - 1);
    item->setFlags(Qt::NoItemFlags);
    QFont font = item->font();
    font.setBold(true);
    item->setFont(font);
}

// The lap's colour as a plain rounded swatch.
class SwatchButton : public QToolButton {
public:
    using QToolButton::QToolButton;
    QColor fill;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF box = QRectF(rect()).adjusted(1, 1, -1, -1);
        QColor color = fill.isValid() ? fill : palette().color(QPalette::Button);
        if (underMouse() || isDown()) color = color.lighter(isDown() ? 90 : 115);
        p.setPen(hasFocus() ? QPen(palette().color(QPalette::Highlight), 2) : Qt::NoPen);
        p.setBrush(color);
        p.drawRoundedRect(box, 4, 4);
    }
};

// A lap list row with its compound in the compound's colour.
class LapItemDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& base, const QModelIndex& index) const override {
        QStyleOptionViewItem option = base;
        initStyleOption(&option, index);
        const QString text = option.text;
        option.text.clear();
        const QStyle* style = option.widget ? option.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &option, painter, option.widget);
        const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &option, option.widget);
        painter->save();
        painter->setFont(option.font);
        painter->setPen(option.palette.color(option.state & QStyle::State_Enabled ? QPalette::Normal : QPalette::Disabled,
                                             option.state & QStyle::State_Selected ? QPalette::HighlightedText : QPalette::Text));
        paintTintedText(*painter, textRect, text, compoundSpan(index.model(), index));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(qMax(size.height(), option.fontMetrics.height() + 6));
        return size;
    }
};

// A combo box whose shown lap has its compound in the compound's colour.
class AnalysisLapCombo : public QComboBox {
public:
    using QComboBox::QComboBox;

protected:
    void paintEvent(QPaintEvent*) override {
        QStylePainter painter(this);
        QStyleOptionComboBox option;
        initStyleOption(&option);
        const QString text = option.currentText;
        option.currentText.clear();
        option.currentIcon = QIcon();
        painter.drawComplexControl(QStyle::CC_ComboBox, option);
        const QRect field = style()->subControlRect(QStyle::CC_ComboBox, &option,
                                                    QStyle::SC_ComboBoxEditField, this).adjusted(1, 0, -1, 0);
        painter.setPen(palette().color(isEnabled() ? QPalette::Normal : QPalette::Disabled, QPalette::ButtonText));
        paintTintedText(painter, field, text, compoundSpan(model(), model()->index(currentIndex(), modelColumn())));
    }
};

void configureCombo(QComboBox* box) {
    box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    box->setMinimumContentsLength(10);
    box->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QLabel* fieldLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setForegroundRole(QPalette::PlaceholderText);
    return label;
}
} // namespace

// The section header's one-line summary; clicking it opens or closes the section.
class AnalysisLapSummary : public QAbstractButton {
public:
    explicit AnalysisLapSummary(QWidget* parent) : QAbstractButton(parent) {
        setFocusPolicy(Qt::NoFocus);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    }

    void setContent(const QString& text, const TintedSpan& span) {
        if (text == text_ && span.start == span_.start && span.length == span_.length && span.color == span_.color) return;
        text_ = text;
        span_ = span;
        setToolTip(text);
        setAccessibleName(text);
        update();
    }

    QSize sizeHint() const override { return {0, fontMetrics().height()}; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setPen(palette().color(isEnabled() ? QPalette::Normal : QPalette::Disabled, QPalette::WindowText));
        paintTintedText(painter, rect(), text_, span_);
    }

private:
    QString text_;
    TintedSpan span_;
};

AnalysisLapSlot::AnalysisLapSlot(const QString& title, const QString& defaultLabel, Mode mode, QWidget* parent)
    : QWidget(parent), mode_(mode), title_(title), defaultLabel_(defaultLabel) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── Header: arrow, colour and summary ──
    header_ = new QWidget(this);
    header_->setBackgroundRole(QPalette::Button);
    header_->setMinimumHeight(kSwatchSize + 10);
    auto* headerRow = new QHBoxLayout(header_);
    headerRow->setContentsMargins(8, 0, 8, 0);
    headerRow->setSpacing(6);
    arrow_ = new QToolButton(header_);
    arrow_->setAutoRaise(true);
    arrow_->setFixedSize(16, kSwatchSize + 4);
    headerRow->addWidget(arrow_);
    auto* swatch = new SwatchButton(header_);
    swatch_ = swatch;
    swatch_->setFixedSize(kSwatchSize, kSwatchSize);
    swatch_->setToolTip(QStringLiteral("%1 colour").arg(title_));
    swatch_->setAccessibleName(QStringLiteral("%1 colour").arg(title_));
    headerRow->addWidget(swatch_);
    summary_ = new AnalysisLapSummary(header_);
    headerRow->addWidget(summary_, 1);
    root->addWidget(header_);

    auto toggle = [this] {
        setExpanded(!expanded_);
        emit expandedChanged();
    };
    connect(arrow_, &QToolButton::clicked, this, toggle);
    connect(summary_, &QAbstractButton::clicked, this, toggle);
    connect(swatch_, &QToolButton::clicked, this, [this] {
        const QColor picked = QColorDialog::getColor(color_, this, QStringLiteral("%1 colour").arg(title_));
        if (!picked.isValid() || picked == color_) return;
        setColor(picked);
        emit colorPicked(picked);
    });

    // ── Fields ──
    body_ = new QWidget(this);
    auto* form = new QFormLayout(body_);
    form->setContentsMargins(8, 6, 8, 8);
    form->setHorizontalSpacing(6);
    form->setVerticalSpacing(4);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    driverBox_ = new QComboBox(body_);
    configureCombo(driverBox_);
    driverBox_->setPlaceholderText(QStringLiteral("No driver"));
    driverBox_->setAccessibleName(QStringLiteral("%1 driver").arg(title_));
    form->addRow(fieldLabel(QStringLiteral("Driver"), body_), driverBox_);

    lapBox_ = new AnalysisLapCombo(body_);
    configureCombo(lapBox_);
    lapBox_->setItemDelegate(new LapItemDelegate(lapBox_));
    lapBox_->setMaxVisibleItems(14);
    lapBox_->setAccessibleName(QStringLiteral("%1 lap").arg(title_));
    form->addRow(fieldLabel(QStringLiteral("Lap"), body_), lapBox_);

    labelEdit_ = new QLineEdit(body_);
    labelEdit_->setMaxLength(kLabelLimit);
    labelEdit_->setPlaceholderText(defaultLabel_);
    labelEdit_->setClearButtonEnabled(true);
    labelEdit_->setToolTip(QStringLiteral("Name shown in chart tooltips, legends and the map"));
    labelEdit_->setAccessibleName(QStringLiteral("%1 name").arg(title_));
    form->addRow(fieldLabel(QStringLiteral("Name"), body_), labelEdit_);
    root->addWidget(body_);

    auto* line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    root->addWidget(line);

    connect(labelEdit_, &QLineEdit::textEdited, this, &AnalysisLapSlot::labelEdited);

    if (mode_ == Mode::Selectable) {
        connect(driverBox_, &QComboBox::activated, this, [this] {
            // The owner clears the lap and offers this driver's laps.
            emit driverActivated();
            refreshSummary();
        });
        connect(lapBox_, &QComboBox::activated, this, [this] {
            refreshSummary();
            emit lapActivated();
        });
    } else {
        driverBox_->addItem(QString());
        driverBox_->setEnabled(false);
        lapBox_->addItem(QString(), -1);
        lapBox_->setEnabled(false);
        const QString followTip = QStringLiteral("Follows the playback cursor");
        driverBox_->setToolTip(followTip);
        lapBox_->setToolTip(followTip);
        setFollowed({}, {});
    }

    setExpanded(true);
    refreshSwatch();
    refreshSummary();
}

void AnalysisLapSlot::setExpanded(bool expanded) {
    expanded_ = expanded;
    body_->setVisible(expanded);
    arrow_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    arrow_->setToolTip(expanded ? QStringLiteral("Collapse %1").arg(title_) : QStringLiteral("Expand %1").arg(title_));
    arrow_->setAccessibleName(arrow_->toolTip());
    header_->setAutoFillBackground(expanded);
}

void AnalysisLapSlot::refreshSwatch() {
    static_cast<SwatchButton*>(swatch_)->fill = color_;
    swatch_->update();
}

void AnalysisLapSlot::refreshSummary() {
    const bool following = mode_ == Mode::FollowsPlayback;
    const int lapRow = lapBox_->currentIndex();
    const bool hasLap = lapRow >= 0 && lapBox_->currentData().toInt() > 0;
    const QString driverName = following ? followedDriver_
                               : driverBox_->currentIndex() >= 0 ? driverBox_->currentText() : QString();
    if (!following && driverName.isEmpty()) {
        summary_->setContent(QStringLiteral("Choose a driver and lap"), {});
    } else if (hasLap) {
        const QString prefix = driverName.isEmpty() ? QString() : driverName + kSeparator;
        const QAbstractItemModel* model = lapBox_->model();
        summary_->setContent(prefix + lapBox_->itemText(lapRow),
                             compoundSpan(model, model->index(lapRow, lapBox_->modelColumn()), int(prefix.size())));
    } else if (following) {
        summary_->setContent(driverName.isEmpty() ? QStringLiteral("Follows the playback cursor")
                                                  : driverName + kSeparator + QStringLiteral("follows playback"), {});
    } else {
        summary_->setContent(driverName + kSeparator + QStringLiteral("None"), {});
    }
}

void AnalysisLapSlot::setDriverChoices(const QVector<AnalysisDriverChoice>& primary,
                                       const QVector<AnalysisDriverChoice>& secondary,
                                       const AnalysisDriverRef& fallback) {
    if (mode_ != Mode::Selectable) return;
    {
        QSignalBlocker guard(driverBox_);
        const QString previous = driverBox_->currentData().toString();
        driverBox_->clear();
        const bool grouped = !secondary.isEmpty();
        auto add = [this](const QVector<AnalysisDriverChoice>& choices) {
            for (const AnalysisDriverChoice& choice : choices)
                driverBox_->addItem(choice.name, driverKey(choice.ref));
        };
        if (grouped) addHeader(driverBox_, QStringLiteral("Primary recording"));
        add(primary);
        if (grouped) {
            addHeader(driverBox_, QStringLiteral("Secondary recording"));
            add(secondary);
        }
        int index = previous.isEmpty() ? -1 : driverBox_->findData(previous);
        if (index < 0 && fallback.valid) index = driverBox_->findData(driverKey(fallback));
        driverBox_->setCurrentIndex(index);
    }
    refreshSummary();
}

AnalysisDriverRef AnalysisLapSlot::driver() const {
    return parseDriverKey(driverBox_->currentData().toString());
}

void AnalysisLapSlot::setDriverSelectable(bool selectable) {
    if (mode_ == Mode::Selectable) driverBox_->setEnabled(selectable);
}

void AnalysisLapSlot::setLapChoices(const QVector<AnalysisLapChoice>& laps) {
    if (mode_ != Mode::Selectable) return;
    {
        QSignalBlocker guard(lapBox_);
        const int previous = lap();
        lapBox_->clear();
        lapBox_->addItem(QStringLiteral("None"), -1);
        for (const AnalysisLapChoice& choice : laps) {
            lapBox_->addItem(lapText(choice, analysis::formatLapTime(choice.lapTimeMs)), choice.lapNum);
            const int row = lapBox_->count() - 1;
            setRowCompound(lapBox_, row, choice);
            if (choice.fastest) {
                QFont font = lapBox_->font();
                font.setBold(true);
                lapBox_->setItemData(row, font, Qt::FontRole);
                lapBox_->setItemData(row, QStringLiteral("Fastest lap"), Qt::ToolTipRole);
            }
        }
        const int index = lapBox_->findData(previous);
        lapBox_->setCurrentIndex(index >= 0 ? index : 0);
    }
    refreshSummary();
}

void AnalysisLapSlot::setLapSelectable(bool selectable) {
    if (mode_ == Mode::Selectable) lapBox_->setEnabled(selectable);
}

int AnalysisLapSlot::lap() const {
    if (lapBox_->currentIndex() < 0) return -1;
    return lapBox_->currentData().toInt();
}

void AnalysisLapSlot::clearLap() {
    if (mode_ != Mode::Selectable) return;
    {
        QSignalBlocker guard(lapBox_);
        lapBox_->setCurrentIndex(lapBox_->count() > 0 ? 0 : -1);
    }
    refreshSummary();
}

bool AnalysisLapSlot::selectDriver(const AnalysisDriverRef& ref) {
    if (mode_ != Mode::Selectable) return false;
    const int index = driverBox_->findData(driverKey(ref));
    if (index < 0) return false;
    {
        QSignalBlocker guard(driverBox_);
        driverBox_->setCurrentIndex(index);
    }
    refreshSummary();
    return true;
}

bool AnalysisLapSlot::selectLap(int lapNum) {
    if (mode_ != Mode::Selectable) return false;
    const int index = lapBox_->findData(lapNum);
    if (index < 0) return false;
    {
        QSignalBlocker guard(lapBox_);
        lapBox_->setCurrentIndex(index);
    }
    refreshSummary();
    return true;
}

void AnalysisLapSlot::resetSelection() {
    if (mode_ != Mode::Selectable) return;
    {
        QSignalBlocker guard(driverBox_);
        driverBox_->setCurrentIndex(-1);
    }
    clearLap();
}

void AnalysisLapSlot::setFollowed(const QString& driverName, const AnalysisLapChoice& lap,
                                  const QString& runningTime) {
    if (mode_ != Mode::FollowsPlayback) return;
    followedDriver_ = driverName;
    const QString driverText = driverName.isEmpty() ? QStringLiteral("No driver") : driverName;
    if (driverBox_->itemText(0) != driverText) driverBox_->setItemText(0, driverText);

    const bool hasLap = lap.lapNum > 0;
    const QString time = lap.lapTimeMs > 0 || runningTime.isEmpty() ? analysis::formatLapTime(lap.lapTimeMs)
                                                                    : runningTime;
    const QString lapLine = hasLap ? lapText(lap, time) : QStringLiteral("No current lap");
    if (lapBox_->itemText(0) != lapLine) lapBox_->setItemText(0, lapLine);
    if (lapBox_->itemData(0).toInt() != (hasLap ? lap.lapNum : -1)) lapBox_->setItemData(0, hasLap ? lap.lapNum : -1);
    if (lapBox_->itemData(0, kCompoundColorRole).value<QColor>() != lap.compoundColor ||
        lapBox_->itemData(0, kCompoundLengthRole).toInt() != lap.compound.size())
        setRowCompound(lapBox_, 0, lap);
    refreshSummary();
}

QString AnalysisLapSlot::label() const {
    return labelEdit_->text();
}

QString AnalysisLapSlot::resolvedLabel() const {
    const QString trimmed = labelEdit_->text().trimmed();
    return trimmed.isEmpty() ? defaultLabel_ : trimmed;
}

void AnalysisLapSlot::setLabel(const QString& label) {
    if (labelEdit_->text() != label) labelEdit_->setText(label.left(kLabelLimit));
}

void AnalysisLapSlot::setColor(const QColor& color) {
    color_ = color;
    refreshSwatch();
}
