#pragma once

// Small widget builders shared by the page classes. These were previously
// duplicated as file-local lambdas/statics across the page translation units.

#include <QWidget>
#include <QFrame>
#include <QLabel>
#include <QHBoxLayout>
#include <QFont>
#include <QPalette>
#include <QColor>
#include <QList>
#include <QPair>
#include <QString>

namespace tnrui {

inline QFrame* hline() {
    QFrame* f = new QFrame;
    f->setFrameShape(QFrame::HLine);
    f->setFrameShadow(QFrame::Sunken);
    return f;
}

inline QFrame* vline() {
    QFrame* f = new QFrame;
    f->setFrameShape(QFrame::VLine);
    f->setFrameShadow(QFrame::Sunken);
    return f;
}

// Chart header strip: bold 8pt title on the left, optional colour-chip legend
// on the right. An invalid/transparent legend colour renders label-only.
inline QWidget* makeChartHeader(const QString& titleText,
                                const QList<QPair<QString, QColor>>& legend = {}) {
    QWidget* header = new QWidget;
    header->setFixedHeight(24);
    QHBoxLayout* hl = new QHBoxLayout(header);
    hl->setContentsMargins(8, 0, 8, 0);
    QLabel* label = new QLabel(titleText);
    QFont f; f.setPointSize(8); f.setBold(true);
    label->setFont(f);
    label->setForegroundRole(QPalette::PlaceholderText);
    hl->addWidget(label);
    hl->addStretch();

    for (const auto& item : legend) {
        if (item.second.isValid() && item.second != Qt::transparent) {
            QWidget* colorBox = new QWidget;
            colorBox->setFixedSize(12, 12);
            colorBox->setStyleSheet(QString("background-color: %1; border-radius: 2px;").arg(item.second.name()));
            hl->addWidget(colorBox);
        }
        QLabel* legLabel = new QLabel(item.first);
        QFont lf; lf.setPointSize(8);
        legLabel->setFont(lf);
        legLabel->setForegroundRole(QPalette::PlaceholderText);

        hl->addWidget(legLabel);
        hl->addSpacing(8);
    }

    return header;
}

// Muted bold section caption ("TIMING", "EVENTS", …) used by the side panels.
inline QLabel* makeSectionLabel(const QString& title, int leftPad = 14, int topPad = 0) {
    QLabel* lbl = new QLabel(title);
    QFont f; f.setPointSize(8); f.setBold(true);
    lbl->setFont(f);
    lbl->setForegroundRole(QPalette::PlaceholderText);
    lbl->setContentsMargins(leftPad, topPad, leftPad, 0);
    return lbl;
}

} // namespace tnrui
