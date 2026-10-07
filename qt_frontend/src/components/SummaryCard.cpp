#include "SummaryCard.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QVBoxLayout>

const QString SummaryCard::kMissing = QString::fromUtf8("—");

// Sizes follow the Qt Overview stat card, which Electron's StatCard shares
// with these pages.
SummaryCard::SummaryCard(const QString& label, const QString& unit, tnr::DensityMode density,
                         QWidget* parent)
    : QFrame(parent), density_(density) {
    const bool compact = density == tnr::DensityMode::Compact;
    const bool spacious = density == tnr::DensityMode::Spacious;
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    label_ = new QLabel(label.toUpper());
    QFont lf; lf.setPointSize(compact ? 8 : spacious ? 10 : 7);
    lf.setBold(spacious);
    label_->setFont(lf);
    label_->setForegroundRole(QPalette::PlaceholderText);

    value_ = new QLabel(kMissing);
    QFont vf; vf.setPointSize(compact ? 12 : spacious ? 24 : 15); vf.setBold(true);
    vf.setFeature(QFont::Tag("tnum"), 1);
    value_->setFont(vf);

    unit_ = new QLabel(unit);
    QFont uf; uf.setPointSize(compact ? 8 : spacious ? 10 : 7);
    unit_->setFont(uf);
    unit_->setForegroundRole(QPalette::PlaceholderText);
    unit_->hide();   // shown once the value is known

    if (compact) {
        // One line: label · value+unit pinned right.
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(12, 3, 12, 3);
        row->setSpacing(4);
        row->addWidget(label_);
        row->addStretch();
        row->addWidget(value_);
        row->addWidget(unit_);
        return;
    }

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(spacious ? 16 : 12, spacious ? 12 : 8,
                               spacious ? 16 : 12, spacious ? 12 : 8);
    column->setSpacing(spacious ? 4 : 2);
    if (spacious) setMinimumHeight(96);

    auto* valueRow = new QWidget;
    auto* valueLayout = new QHBoxLayout(valueRow);
    valueLayout->setContentsMargins(0, 0, 0, 0);
    valueLayout->setSpacing(4);
    valueLayout->addWidget(value_);
    valueLayout->addWidget(unit_);
    valueLayout->addStretch();

    column->addWidget(label_);
    column->addWidget(valueRow);
    if (spacious) {
        sub_ = new QLabel;
        QFont sf; sf.setPointSize(10); sf.setBold(true);
        sub_->setFont(sf);
        sub_->setForegroundRole(QPalette::PlaceholderText);
        sub_->hide();
        column->addWidget(sub_);
    }
    column->addStretch();
}

void SummaryCard::setLabel(const QString& label) {
    const QString text = label.toUpper();
    if (label_->text() != text) label_->setText(text);
}

void SummaryCard::setValue(const QString& text, const QColor& color) {
    if (value_->text() != text) value_->setText(text);
    const bool known = text != kMissing;
    if (!unit_->text().isEmpty() && unit_->isVisibleTo(this) != known) unit_->setVisible(known);
    if (color == valueColor_) return;
    valueColor_ = color;
    if (color.isValid()) {
        QPalette palette = value_->palette();
        palette.setColor(QPalette::WindowText, color);
        value_->setPalette(palette);
    } else {
        value_->setPalette(QPalette());   // inherit the theme's text colour again
    }
}

void SummaryCard::setSub(const QString& text) {
    if (!sub_) return;
    if (sub_->text() != text) sub_->setText(text);
    sub_->setVisible(!text.isEmpty());
}
