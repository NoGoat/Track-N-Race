#include "AnalysisLapSlot.h"
#include "AnalysisWidgets.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QStandardItemModel>

namespace {
constexpr int kLabelLimit = 40;

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

QString lapText(const AnalysisLapChoice& lap) {
    QStringList parts{QStringLiteral("Lap %1").arg(lap.lapNum)};
    if (!lap.compound.isEmpty()) parts << lap.compound;
    parts << analysis::formatLapTime(lap.lapTimeMs);
    return parts.join(QStringLiteral("  ·  "));
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
}

AnalysisLapSlot::AnalysisLapSlot(const QString& title, const QString& defaultLabel, Mode mode,
                                 QWidget* parent)
    : QGroupBox(title, parent), mode_(mode), defaultLabel_(defaultLabel) {
    auto* form = new QFormLayout(this);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);

    if (mode_ == Mode::Selectable) {
        driverBox_ = new QComboBox(this);
        driverBox_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        driverBox_->setMinimumContentsLength(8);
        driverBox_->setPlaceholderText(QStringLiteral("No driver"));
        lapBox_ = new QComboBox(this);
        lapBox_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        lapBox_->setMinimumContentsLength(8);
        form->addRow(QStringLiteral("Driver:"), driverBox_);
        form->addRow(QStringLiteral("Lap:"), lapBox_);
        connect(driverBox_, &QComboBox::activated, this, [this] { emit driverActivated(); });
        connect(lapBox_, &QComboBox::activated, this, [this] { emit lapActivated(); });
    } else {
        followedDriver_ = new QLabel(this);
        followedDriver_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto* lapRow = new QWidget(this);
        auto* lapLayout = new QHBoxLayout(lapRow);
        lapLayout->setContentsMargins(0, 0, 0, 0);
        lapLayout->setSpacing(6);
        followedCompound_ = new QLabel(lapRow);
        followedLap_ = new QLabel(lapRow);
        followedLap_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        lapLayout->addWidget(followedCompound_);
        lapLayout->addWidget(followedLap_, 1);
        form->addRow(QStringLiteral("Driver:"), followedDriver_);
        form->addRow(QStringLiteral("Lap:"), lapRow);
        setFollowed({}, {});
    }

    // Name and colour identify this lap everywhere it is drawn, so they share a row.
    auto* labelRow = new QWidget(this);
    auto* labelLayout = new QHBoxLayout(labelRow);
    labelLayout->setContentsMargins(0, 0, 0, 0);
    labelLayout->setSpacing(4);
    labelEdit_ = new QLineEdit(labelRow);
    labelEdit_->setMaxLength(kLabelLimit);
    labelEdit_->setPlaceholderText(defaultLabel_);
    labelEdit_->setClearButtonEnabled(true);
    labelEdit_->setToolTip(QStringLiteral("Name shown in chart tooltips, legends and the map"));
    colorButton_ = new AnalysisColorButton(QStringLiteral("%1 colour").arg(title), labelRow);
    colorButton_->setFixedSize(labelEdit_->sizeHint().height(), labelEdit_->sizeHint().height());
    labelLayout->addWidget(labelEdit_, 1);
    labelLayout->addWidget(colorButton_);
    form->addRow(QStringLiteral("Name:"), labelRow);

    connect(labelEdit_, &QLineEdit::textEdited, this, &AnalysisLapSlot::labelEdited);
    connect(colorButton_, &AnalysisColorButton::colorPicked, this, &AnalysisLapSlot::colorPicked);
}

void AnalysisLapSlot::setDriverChoices(const QVector<AnalysisDriverChoice>& primary,
                                       const QVector<AnalysisDriverChoice>& secondary,
                                       const AnalysisDriverRef& fallback) {
    if (!driverBox_) return;
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

AnalysisDriverRef AnalysisLapSlot::driver() const {
    return driverBox_ ? parseDriverKey(driverBox_->currentData().toString()) : AnalysisDriverRef{};
}

void AnalysisLapSlot::setDriverSelectable(bool selectable) {
    if (driverBox_) driverBox_->setEnabled(selectable);
}

void AnalysisLapSlot::setLapChoices(const QVector<AnalysisLapChoice>& laps) {
    if (!lapBox_) return;
    QSignalBlocker guard(lapBox_);
    const int previous = lap();
    lapBox_->clear();
    lapBox_->addItem(QStringLiteral("None"), -1);
    for (const AnalysisLapChoice& choice : laps) {
        lapBox_->addItem(analysis::dotIcon(choice.compoundColor, this), lapText(choice),
                         choice.lapNum);
        const int row = lapBox_->count() - 1;
        if (choice.fastest) {
            QFont font = lapBox_->font();
            font.setBold(true);
            lapBox_->setItemData(row, font, Qt::FontRole);
            lapBox_->setItemData(row, QStringLiteral("Fastest lap"), Qt::ToolTipRole);
            lapBox_->setItemText(row, lapText(choice) + QStringLiteral("  ·  Fastest"));
        }
    }
    const int index = lapBox_->findData(previous);
    lapBox_->setCurrentIndex(index >= 0 ? index : 0);
}

void AnalysisLapSlot::setLapSelectable(bool selectable) {
    if (lapBox_) lapBox_->setEnabled(selectable);
}

int AnalysisLapSlot::lap() const {
    if (!lapBox_ || lapBox_->currentIndex() < 0) return -1;
    return lapBox_->currentData().toInt();
}

void AnalysisLapSlot::clearLap() {
    if (!lapBox_) return;
    QSignalBlocker guard(lapBox_);
    lapBox_->setCurrentIndex(lapBox_->count() > 0 ? 0 : -1);
}

bool AnalysisLapSlot::selectDriver(const AnalysisDriverRef& ref) {
    if (!driverBox_) return false;
    const int index = driverBox_->findData(driverKey(ref));
    if (index < 0) return false;
    QSignalBlocker guard(driverBox_);
    driverBox_->setCurrentIndex(index);
    return true;
}

bool AnalysisLapSlot::selectLap(int lapNum) {
    if (!lapBox_) return false;
    const int index = lapBox_->findData(lapNum);
    if (index < 0) return false;
    QSignalBlocker guard(lapBox_);
    lapBox_->setCurrentIndex(index);
    return true;
}

void AnalysisLapSlot::resetSelection() {
    if (!driverBox_) return;
    {
        QSignalBlocker guard(driverBox_);
        driverBox_->setCurrentIndex(-1);
    }
    clearLap();
}

void AnalysisLapSlot::setFollowed(const QString& driverName, const AnalysisLapChoice& lap,
                                  const QString& runningTime) {
    if (!followedDriver_) return;
    const QString none = QString::fromUtf8("—");
    followedDriver_->setText(driverName.isEmpty() ? none : driverName);
    if (lap.lapNum <= 0) {
        followedCompound_->clear();
        followedCompound_->hide();
        followedLap_->setText(QStringLiteral("Follows the playback cursor"));
        followedLap_->setForegroundRole(QPalette::PlaceholderText);
        return;
    }
    followedCompound_->setVisible(lap.compoundColor.isValid());
    followedCompound_->setPixmap(analysis::dotIcon(lap.compoundColor, this).pixmap(12, 12));
    QStringList parts{QStringLiteral("Lap %1").arg(lap.lapNum)};
    if (!lap.compound.isEmpty()) parts << lap.compound;
    parts << (lap.lapTimeMs > 0 || runningTime.isEmpty() ? analysis::formatLapTime(lap.lapTimeMs)
                                                         : runningTime);
    if (lap.fastest) parts << QStringLiteral("Fastest");
    const QString text = parts.join(QStringLiteral("  ·  "));
    followedLap_->setText(text);
    followedLap_->setForegroundRole(QPalette::WindowText);
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

QColor AnalysisLapSlot::color() const {
    return colorButton_->color();
}

void AnalysisLapSlot::setColor(const QColor& color) {
    colorButton_->setColor(color);
}
