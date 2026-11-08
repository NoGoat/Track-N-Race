#include "EditMapDialog.h"

#include "TrackMapWidget.h"
#include "../IconUtils.h"
#include "../TrackMapService.h"

#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

#include <tnrp/TrackMaps.h>

namespace {

double wrapDegrees(double value) {
    const double wrapped = std::fmod(value, 360.0);
    return wrapped < 0 ? wrapped + 360.0 : wrapped;
}

} // namespace

EditMapDialog::EditMapDialog(const QString& mapId, QWidget* parent)
    : QDialog(parent), mapId_(mapId), base_(TrackMapService::instance().map(mapId)) {
    setWindowTitle(QStringLiteral("Edit Map"));
    auto* root = new QVBoxLayout(this);
    auto* body = new QHBoxLayout;
    root->addLayout(body, 1);

    preview_ = new TrackMapWidget(this);
    preview_->setControlledMode(true);
    preview_->setMinimumSize(420, 340);
    body->addWidget(preview_, 1);

    auto* form = new QFormLayout;
    body->addLayout(form);
    name_ = new QLineEdit(this);
    trackName_ = new QLineEdit(this);
    circuitName_ = new QLineEdit(this);
    form->addRow(QStringLiteral("Name:"), name_);
    form->addRow(QStringLiteral("Grand Prix:"), trackName_);
    form->addRow(QStringLiteral("Circuit:"), circuitName_);

    auto* rotation = new QWidget(this);
    auto* rotationLayout = new QVBoxLayout(rotation);
    rotationLayout->setContentsMargins(0, 0, 0, 0);
    rotationSlider_ = new QSlider(Qt::Horizontal, rotation);
    rotationSlider_->setRange(0, 359);
    rotationSlider_->setAccessibleName(QStringLiteral("Rotation"));
    rotationLayout->addWidget(rotationSlider_);
    auto* steps = new QHBoxLayout;
    const QColor tint = palette().color(QPalette::WindowText);
    auto* left = new QToolButton(rotation);
    left->setIcon(adaptThemeIcon(QIcon::fromTheme(QStringLiteral("object-rotate-left")), tint,
                                 style()->standardIcon(QStyle::SP_ArrowLeft)));
    left->setToolTip(QStringLiteral("Rotate 90° Left"));
    auto* right = new QToolButton(rotation);
    right->setIcon(adaptThemeIcon(QIcon::fromTheme(QStringLiteral("object-rotate-right")), tint,
                                  style()->standardIcon(QStyle::SP_ArrowRight)));
    right->setToolTip(QStringLiteral("Rotate 90° Right"));
    rotationSpin_ = new QDoubleSpinBox(rotation);
    rotationSpin_->setRange(-720, 720);
    rotationSpin_->setDecimals(1);
    rotationSpin_->setSuffix(QStringLiteral("°"));
    rotationSpin_->setAccessibleName(QStringLiteral("Rotation in degrees"));
    steps->addWidget(left);
    steps->addWidget(rotationSpin_, 1);
    steps->addWidget(right);
    rotationLayout->addLayout(steps);
    form->addRow(QStringLiteral("Rotation:"), rotation);

    if (mapId_.startsWith(QLatin1String("builtin:"))) {
        auto* note = new QLabel(QStringLiteral("Saved as a copy for this track."), this);
        note->setEnabled(false);
        form->addRow(QString(), note);
    }
    error_ = new QLabel(this);
    error_->setWordWrap(true);
    error_->hide();
    form->addRow(QString(), error_);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons_);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons_, &QDialogButtonBox::accepted, this, &EditMapDialog::save);

    connect(rotationSlider_, &QSlider::valueChanged, this, [this](int value) { setRotation(value); });
    connect(rotationSpin_, &QDoubleSpinBox::valueChanged, this, [this](double value) { setRotation(value); });
    connect(left, &QToolButton::clicked, this, [this] { setRotation(rotation_ - 90); });
    connect(right, &QToolButton::clicked, this, [this] { setRotation(rotation_ + 90); });

    if (!base_) {
        error_->setText(QStringLiteral("This map could not be loaded."));
        error_->show();
        buttons_->button(QDialogButtonBox::Save)->setEnabled(false);
        return;
    }
    name_->setText(QString::fromStdString(base_->display_name.value_or(base_->track_name)));
    trackName_->setText(QString::fromStdString(base_->track_name));
    circuitName_->setText(QString::fromStdString(base_->circuit_name));
    setRotation(base_->rotation_deg);
}

void EditMapDialog::setRotation(double degrees) {
    rotation_ = wrapDegrees(degrees);
    {
        QSignalBlocker sliderGuard(rotationSlider_);
        QSignalBlocker spinGuard(rotationSpin_);
        rotationSlider_->setValue(int(std::lround(rotation_)) % 360);
        rotationSpin_->setValue(rotation_);
    }
    if (!base_) return;
    auto preview = std::make_shared<tnrp::TrackMapData>(*base_);
    preview->rotation_deg = rotation_;
    preview_->setMapOverride(std::move(preview));
}

void EditMapDialog::save() {
    tnrp::TrackMapEdit edit;
    edit.display_name = name_->text().toStdString();
    edit.track_name = trackName_->text().toStdString();
    edit.circuit_name = circuitName_->text().toStdString();
    edit.rotation_deg = rotation_;
    buttons_->button(QDialogButtonBox::Save)->setEnabled(false);
    buttons_->button(QDialogButtonBox::Save)->setText(QStringLiteral("Saving…"));
    error_->hide();
    QPointer<EditMapDialog> self(this);
    TrackMapService::instance().edit(mapId_, edit, [self](const TrackMapService::Result& result) {
        if (!self) return;
        if (result.ok) {
            self->accept();
            return;
        }
        self->error_->setText(result.error.isEmpty() ? QStringLiteral("The map could not be saved.")
                                                     : result.error);
        self->error_->show();
        self->buttons_->button(QDialogButtonBox::Save)->setText(QStringLiteral("Save"));
        self->buttons_->button(QDialogButtonBox::Save)->setEnabled(true);
    });
}
