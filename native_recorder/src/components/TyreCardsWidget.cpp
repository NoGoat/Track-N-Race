#include "TyreCardsWidget.h"
#include "TyreHelpers.h"

#include <QBoxLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>
#include <QFont>
#include <QPalette>
#include <QSizePolicy>

static const char* kCornerNames[] = { "FRONT LEFT", "FRONT RIGHT", "REAR LEFT", "REAR RIGHT" };

TyreCardsWidget::TyreCardsWidget(Qt::Orientation orientation, QWidget* parent)
    : QWidget(parent)
{
    QBoxLayout* outer = (orientation == Qt::Horizontal)
        ? static_cast<QBoxLayout*>(new QHBoxLayout(this))
        : static_cast<QBoxLayout*>(new QVBoxLayout(this));
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    for (int i = 0; i < 4; ++i) {
        QWidget* card = new QWidget;
        card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        QVBoxLayout* cv = new QVBoxLayout(card);
        cv->setContentsMargins(10, 8, 10, 8);
        cv->setSpacing(2);

        QLabel* title = new QLabel(kCornerNames[i]);
        QFont tf; tf.setPointSize(7); tf.setBold(true);
        title->setFont(tf);
        title->setForegroundRole(QPalette::PlaceholderText);
        cv->addWidget(title);

        auto makeRow = [&](const QString& label, QLabel*& valueOut) {
            QWidget* row = new QWidget;
            QHBoxLayout* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            QLabel* lbl = new QLabel(label);
            QFont lf; lf.setPointSize(8); lbl->setFont(lf);
            lbl->setForegroundRole(QPalette::PlaceholderText);
            valueOut = new QLabel("—");
            QFont vf; vf.setPointSize(8); vf.setBold(true);
            valueOut->setFont(vf);
            valueOut->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            h->addWidget(lbl);
            h->addStretch();
            h->addWidget(valueOut);
            return row;
        };

        cv->addWidget(makeRow("Surface", surfaceTemp_[i]));
        cv->addWidget(makeRow("Inner",   innerTemp_[i]));
        cv->addWidget(makeRow("Brake",   brakeTemp_[i]));
        cv->addWidget(makeRow("Wear",    wearLabel_[i]));

        auto* wearBar = new QProgressBar;
        wearBar->setRange(0, 100);
        wearBar->setValue(0);
        wearBar->setTextVisible(false);
        wearBar->setFixedHeight(6);
        wearBar->setStyleSheet(
            "QProgressBar { border: none; background: palette(mid); border-radius: 3px; }"
            "QProgressBar::chunk { background: #73BF69; border-radius: 3px; }"
        );
        wear_[i] = wearBar;
        cv->addWidget(wearBar);

        blisters_[i] = new QLabel;
        blisters_[i]->setVisible(false);
        QFont bf; bf.setPointSize(7);
        blisters_[i]->setFont(bf);
        blisters_[i]->setForegroundRole(QPalette::PlaceholderText);
        cv->addWidget(blisters_[i]);

        outer->addWidget(card, 1);

        if (i < 3) {
            QFrame* div = new QFrame;
            if (orientation == Qt::Horizontal) {
                div->setFrameShape(QFrame::VLine);
            } else {
                div->setFrameShape(QFrame::HLine);
            }
            div->setFrameShadow(QFrame::Sunken);
            outer->addWidget(div);
        }
    }
}

void TyreCardsWidget::update(const nlohmann::json& telemetry, const nlohmann::json& damage) {
    static const char* surfKeys[]    = { "tyre_temp_surface_fl", "tyre_temp_surface_fr", "tyre_temp_surface_rl", "tyre_temp_surface_rr" };
    static const char* innerKeys[]   = { "tyre_temp_inner_fl",   "tyre_temp_inner_fr",   "tyre_temp_inner_rl",   "tyre_temp_inner_rr"   };
    static const char* brakeKeys[]   = { "brake_temp_fl",        "brake_temp_fr",        "brake_temp_rl",        "brake_temp_rr"        };
    static const char* wearKeys[]    = { "tyre_wear_fl",         "tyre_wear_fr",         "tyre_wear_rl",         "tyre_wear_rr"         };
    static const char* blisterKeys[] = { "blisters_fl",          "blisters_fr",          "blisters_rl",          "blisters_rr"          };

    for (int i = 0; i < 4; ++i) {
        if (!telemetry.empty()) {
            int surf = telemetry.value(surfKeys[i], -1);
            if (surf >= 0) {
                surfaceTemp_[i]->setText(QString::number(surf) + "°C");
                surfaceTemp_[i]->setStyleSheet("color: " + tyreTempColor(surf).name() + "; font-weight: bold;");
            }
            int inner = telemetry.value(innerKeys[i], -1);
            if (inner >= 0) {
                innerTemp_[i]->setText(QString::number(inner) + "°C");
                innerTemp_[i]->setStyleSheet("color: " + tyreTempColor(inner).name() + "; font-weight: bold;");
            }
            int brk = telemetry.value(brakeKeys[i], -1);
            if (brk >= 0) {
                brakeTemp_[i]->setText(QString::number(brk) + "°C");
                brakeTemp_[i]->setStyleSheet("color: " + brakeTempColor(brk).name() + "; font-weight: bold;");
            }
        }

        if (!damage.empty()) {
            int wear = damage.value(wearKeys[i], -1);
            if (wear >= 0) {
                const QString wearCol = wearPctColor(wear).name();
                wearLabel_[i]->setText(QString::number(wear) + "%");
                wearLabel_[i]->setStyleSheet("color: " + wearCol + "; font-weight: bold;");
                wear_[i]->setValue(wear);
                wear_[i]->setStyleSheet(QString(
                    "QProgressBar { border: none; background: palette(mid); border-radius: 3px; }"
                    "QProgressBar::chunk { background: %1; border-radius: 3px; }"
                ).arg(wearCol));
            }
            int blisters = damage.value(blisterKeys[i], 0);
            if (blisters > 0) {
                blisters_[i]->setText(QString("· %1% blisters").arg(blisters));
                blisters_[i]->setVisible(true);
            } else {
                blisters_[i]->setVisible(false);
            }
        }
    }
}
