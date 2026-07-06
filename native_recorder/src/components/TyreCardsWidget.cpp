#include "TyreCardsWidget.h"
#include "TyreHelpers.h"

#include <QBoxLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QLayout>
#include <QFrame>
#include <QFont>
#include <QPalette>
#include <QSizePolicy>
#include <algorithm>

static const char* kCornerNames[] = { "FRONT LEFT", "FRONT RIGHT", "REAR LEFT", "REAR RIGHT" };

// Remove and delete every item in a layout so the cards can be rebuilt in place
// when compact mode toggles at runtime.
static void clearLayout(QLayout* lay) {
    if (!lay) return;
    while (QLayoutItem* item = lay->takeAt(0)) {
        if (QWidget* w = item->widget()) delete w;
        else if (QLayout* child = item->layout()) clearLayout(child);
        delete item;
    }
}

TyreCardsWidget::TyreCardsWidget(Qt::Orientation orientation, QWidget* parent)
    : QWidget(parent), orientation_(orientation)
{
    QBoxLayout* outer = (orientation == Qt::Horizontal)
        ? static_cast<QBoxLayout*>(new QHBoxLayout(this))
        : static_cast<QBoxLayout*>(new QVBoxLayout(this));
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    buildCards();
}

// Build (or rebuild in place) the four corner cards + dividers. The default (full)
// card stacks Surface/Inner/Brake/Wear label-value rows over a wear bar. Compact —
// used only by the Overview page (see OverviewPage) — centres the corner name over
// a Surface/Inner/Brake/Wear header row and a matching value row, dropping the wear
// bar and blister line. The value labels are recreated, so update() repaints them.
void TyreCardsWidget::buildCards() {
    QBoxLayout* outer = qobject_cast<QBoxLayout*>(layout());
    clearLayout(outer);
    for (int i = 0; i < 4; ++i) {
        cards_[i] = nullptr;
        surfaceTemp_[i] = innerTemp_[i] = brakeTemp_[i] = wearLabel_[i] = nullptr;
        wear_[i] = nullptr;
        blisters_[i] = nullptr;
    }
    for (int d = 0; d < 3; ++d) dividers_[d] = nullptr;

    for (int i = 0; i < 4; ++i) {
        QWidget* card = new QWidget;
        card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        QVBoxLayout* cv = new QVBoxLayout(card);

        QLabel* title = new QLabel(kCornerNames[i]);
        QFont tf; tf.setPointSize(7); tf.setBold(true);
        title->setFont(tf);
        title->setForegroundRole(QPalette::PlaceholderText);

        if (compact_) {
            // No card margin/spacing so the under-title rule reaches the card edges
            // (meeting the between-card dividers) and the grid runs flush to the
            // card's top rule and bottom edge.
            cv->setContentsMargins(0, 0, 0, 0);
            cv->setSpacing(0);
            title->setAlignment(Qt::AlignHCenter);
            title->setContentsMargins(0, 4, 0, 3);   // padding lives on the title, not the grid
            cv->addWidget(title);

            // Horizontal rule between the title and the columns.
            QFrame* titleRule = new QFrame;
            titleRule->setFrameShape(QFrame::HLine);
            titleRule->setFrameShadow(QFrame::Sunken);
            cv->addWidget(titleRule);

            // Surface/Inner/Brake/Wear columns with a vertical separator between
            // each. Data columns live at grid columns 0/2/4/6, separators at 1/3/5.
            // The grid has NO top/bottom margin and rows 0 & 3 are stretch spacers
            // that centre the label/value pair; the separators span all four rows so
            // they run edge-to-edge — touching the rule above and the section line
            // below — with no gap from container padding.
            QGridLayout* grid = new QGridLayout;
            grid->setContentsMargins(10, 0, 10, 0);
            grid->setHorizontalSpacing(8);
            grid->setVerticalSpacing(1);
            grid->setRowStretch(0, 1);   // top spacer
            grid->setRowStretch(3, 1);   // bottom spacer
            const char* subLabels[4] = { "Surface", "Inner", "Brake", "Wear" };
            QLabel** vals[4] = { &surfaceTemp_[i], &innerTemp_[i], &brakeTemp_[i], &wearLabel_[i] };
            for (int c = 0; c < 4; ++c) {
                const int gc = c * 2;
                QLabel* lbl = new QLabel(subLabels[c]);
                QFont lf; lf.setPointSize(7); lbl->setFont(lf);
                lbl->setForegroundRole(QPalette::PlaceholderText);
                lbl->setAlignment(Qt::AlignCenter);
                QLabel* val = new QLabel("—");
                QFont vf; vf.setPointSize(9); vf.setBold(true); val->setFont(vf);
                val->setAlignment(Qt::AlignCenter);
                *vals[c] = val;
                grid->addWidget(lbl, 1, gc);
                grid->addWidget(val, 2, gc);
                grid->setColumnStretch(gc, 1);
                if (c < 3) {
                    QFrame* sep = new QFrame;
                    sep->setFrameShape(QFrame::VLine);
                    sep->setFrameShadow(QFrame::Sunken);
                    grid->addWidget(sep, 0, gc + 1, 4, 1);   // span all rows: rule above → line below
                }
            }
            cv->addLayout(grid, 1);   // grid fills the card so the separators reach the bottom edge
            // wear_ / blisters_ intentionally left null: no bar, no blister row.
        } else {
            cv->setContentsMargins(10, 8, 10, 8);
            cv->setSpacing(2);
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
        }

        cards_[i] = card;
        card->setVisible(cornerVisible_[i]);
        outer->addWidget(card, 1);

        if (i < 3) {
            QFrame* div = new QFrame;
            div->setFrameShape(orientation_ == Qt::Horizontal ? QFrame::VLine : QFrame::HLine);
            div->setFrameShadow(QFrame::Sunken);
            dividers_[i] = div;
            outer->addWidget(div);
        }
    }
    updateDividers();
}

// Live compact-mode toggle (Overview page only). Rebuilds the cards at the new
// density; the Overview page re-applies corner visibility and re-feeds update().
void TyreCardsWidget::setCompactMode(bool on) {
    if (compact_ == on) return;
    compact_ = on;
    buildCards();
}

void TyreCardsWidget::setCornerVisible(int i, bool on)
{
    if (i < 0 || i >= 4) return;
    cornerVisible_[i] = on;
    if (cards_[i]) cards_[i]->setVisible(on);
    updateDividers();
}

void TyreCardsWidget::updateDividers()
{
    // Use the logical visibility we track, NOT cards_[]->isVisible(): this runs from
    // applyLayout during construction (before the page is ever shown), when every
    // card's realized isVisible() is false — which would otherwise hide every
    // divider permanently.
    for (int d = 0; d < 3; ++d) {
        if (dividers_[d])
            dividers_[d]->setVisible(cornerVisible_[d] && cornerVisible_[d + 1]);
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
                if (wear_[i]) {   // no wear bar in compact mode
                    wear_[i]->setValue(wear);
                    wear_[i]->setStyleSheet(QString(
                        "QProgressBar { border: none; background: palette(mid); border-radius: 3px; }"
                        "QProgressBar::chunk { background: %1; border-radius: 3px; }"
                    ).arg(wearCol));
                }
            }
            if (blisters_[i]) {   // no blister line in compact mode
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
}
