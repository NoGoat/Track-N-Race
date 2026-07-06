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

static const char* kCornerNames[]  = { "FRONT LEFT", "FRONT RIGHT", "REAR LEFT", "REAR RIGHT" };
static const char* kCornerAbbrev[] = { "FL", "FR", "RL", "RR" };

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
        compactCorner_[i].clear();
    }
    for (int d = 0; d < 3; ++d) dividers_[d] = nullptr;
    compactGrid_ = nullptr;

    if (level_ == Compact)        { buildCompactGrid(outer, /*showHeading*/ true);  return; }
    if (level_ == UltraCompact1)  { buildCompactGrid(outer, /*showHeading*/ false); return; }
    if (level_ == UltraCompact2)  { buildOneLine(outer, /*abbrev*/ false, /*showLabels*/ false); return; }
    if (level_ == UltraCompact3)  { buildOneLine(outer, /*abbrev*/ true,  /*showLabels*/ true);  return; }

    for (int i = 0; i < 4; ++i) {
        QWidget* card = new QWidget;
        card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        QVBoxLayout* cv = new QVBoxLayout(card);

        QLabel* title = new QLabel(kCornerNames[i]);
        QFont tf; tf.setPointSize(7); tf.setBold(true);
        title->setFont(tf);
        title->setForegroundRole(QPalette::PlaceholderText);

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

// Compact (Overview) layout. Built as ONE grid of 8 half-width columns (0,2,…,14,
// stretch 1) + 7 divider columns (1,3,…,13, a vline each) — structurally identical
// to the damage row's 8 cards + 7 dividers. Because both are 8 equal stretch units
// separated by 7 same-width dividers, Qt computes the exact same column boundaries
// for each, so every tyre separator lands pixel-perfect on a damage-card boundary.
//
// Each corner owns 2 half-columns (left = Surface/Inner, right = Brake/Wear); the
// divider column between them is the Inner/Brake separator (= the corner's midpoint
// = a damage boundary). The divider column after a corner is the between-corner
// separator (full height). The Surface/Inner and Brake/Wear splits live nested
// inside a half-column, so they don't shift the shared column boundaries.
//
// showHeading draws the corner-name title + rule above the value cells (Compact);
// UltraCompact1 passes false to drop that heading row, leaving just the value row.
void TyreCardsWidget::buildCompactGrid(QBoxLayout* outer, bool showHeading) {
    QWidget* host = new QWidget;
    QGridLayout* g = new QGridLayout(host);
    compactGrid_ = g;
    g->setContentsMargins(0, 0, 0, 0);
    g->setHorizontalSpacing(0);
    g->setVerticalSpacing(0);
    const int cellRow = showHeading ? 2 : 0;         // value cells sit below the heading, or at the top
    g->setRowStretch(cellRow, 1);                    // body row fills the height
    for (int c = 0; c <= 14; c += 2) g->setColumnStretch(c, 1);   // 8 half-width columns

    auto vline = [] {
        QFrame* f = new QFrame; f->setFrameShape(QFrame::VLine); f->setFrameShadow(QFrame::Sunken); return f;
    };
    // One-line cell: uppercase label on the left, value pinned right (matching the
    // other compact cards).
    auto subCol = [](const char* text, QLabel*& out) {
        QWidget* w = new QWidget;
        QHBoxLayout* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0); h->setSpacing(4);
        QLabel* l = new QLabel(QString(text).toUpper());
        QFont lf; lf.setPointSize(7); l->setFont(lf);
        l->setForegroundRole(QPalette::PlaceholderText);
        out = new QLabel("—");
        QFont vf; vf.setPointSize(9); vf.setBold(true); out->setFont(vf);
        out->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        h->addWidget(l);
        h->addStretch();
        h->addWidget(out);
        return w;
    };
    // A half-column widget: two sub-columns split by their own (non-aligning) line.
    auto halfCell = [&](const char* t0, QLabel*& v0, const char* t1, QLabel*& v1) {
        QWidget* w = new QWidget;
        QHBoxLayout* h = new QHBoxLayout(w);
        h->setContentsMargins(8, 0, 8, 0); h->setSpacing(8);
        h->addWidget(subCol(t0, v0), 1);
        h->addWidget(vline());
        h->addWidget(subCol(t1, v1), 1);
        return w;
    };

    for (int k = 0; k < 4; ++k) {
        const int c0 = 4 * k;        // left half-column
        const int c1 = 4 * k + 2;    // right half-column

        QLabel* title = nullptr;
        QFrame* rule  = nullptr;
        if (showHeading) {
            title = new QLabel(kCornerNames[k]);
            QFont tf; tf.setPointSize(7); tf.setBold(true); title->setFont(tf);
            title->setForegroundRole(QPalette::PlaceholderText);
            title->setAlignment(Qt::AlignHCenter);
            title->setContentsMargins(0, 4, 0, 3);
            g->addWidget(title, 0, c0, 1, 3);        // spans the corner's 2 halves + mid divider

            rule = new QFrame; rule->setFrameShape(QFrame::HLine); rule->setFrameShadow(QFrame::Sunken);
            g->addWidget(rule, 1, c0, 1, 3);
        }

        QWidget* left  = halfCell("Surface", surfaceTemp_[k], "Inner", innerTemp_[k]);
        QWidget* right = halfCell("Brake",   brakeTemp_[k],   "Wear",  wearLabel_[k]);
        g->addWidget(left,  cellRow, c0);
        g->addWidget(right, cellRow, c1);

        // Inner/Brake separator: divider column between the halves, body row only
        // (starts at the rule, runs to the bottom edge).
        QFrame* mid = vline();
        g->addWidget(mid, cellRow, c0 + 1);

        compactCorner_[k] = { title, rule, left, right, mid };

        if (k < 3) {
            QFrame* bc = vline();                    // between-corner separator, spans all built rows
            g->addWidget(bc, 0, c1 + 1, cellRow + 1, 1);
            dividers_[k] = bc;
        }
    }

    outer->addWidget(host, 1);
    for (int k = 0; k < 4; ++k) {
        for (QWidget* w : compactCorner_[k]) if (w) w->setVisible(cornerVisible_[k]);
        // Collapse a hidden corner's two half-columns so the visible corners grow.
        g->setColumnStretch(4 * k,     cornerVisible_[k] ? 1 : 0);
        g->setColumnStretch(4 * k + 2, cornerVisible_[k] ? 1 : 0);
    }
    updateDividers();
}

// One line per corner: the corner name on the left, then the four values pinned to
// the right. No dividers inside a corner — just the between-corner separators (as in
// the other layouts). abbrev shortens the name (FL/FR/…); showLabels prefixes each
// value with its metric (Surface/Inner/Brake/Wear).
//   UltraCompact2 — full name, bare values
//   UltraCompact3 — abbreviated name, labelled values
void TyreCardsWidget::buildOneLine(QBoxLayout* outer, bool abbrev, bool showLabels) {
    for (int k = 0; k < 4; ++k) {
        QWidget* cell = new QWidget;
        QHBoxLayout* h = new QHBoxLayout(cell);
        h->setContentsMargins(10, 0, 10, 0);
        h->setSpacing(showLabels ? 6 : 10);

        QLabel* name = new QLabel(abbrev ? kCornerAbbrev[k] : kCornerNames[k]);
        QFont tf; tf.setPointSize(7); tf.setBold(true); name->setFont(tf);
        name->setForegroundRole(QPalette::PlaceholderText);
        h->addWidget(name);
        h->addStretch();

        // Optional metric label + value; labelled values get extra left padding so
        // the four groups read as Surface 80°C  Inner 89°C  … rather than running on.
        auto valueLabel = [&](const char* metric, QLabel*& out) {
            if (showLabels) {
                QLabel* l = new QLabel(metric);
                QFont lf; lf.setPointSize(8); l->setFont(lf);
                l->setForegroundRole(QPalette::PlaceholderText);
                l->setContentsMargins(8, 0, 0, 0);
                h->addWidget(l);
            }
            out = new QLabel("—");
            QFont vf; vf.setPointSize(9); vf.setBold(true); out->setFont(vf);
            out->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            h->addWidget(out);
        };
        valueLabel("Surface", surfaceTemp_[k]);
        valueLabel("Inner",   innerTemp_[k]);
        valueLabel("Brake",   brakeTemp_[k]);
        valueLabel("Wear",    wearLabel_[k]);

        compactCorner_[k] = { cell };
        cell->setVisible(cornerVisible_[k]);
        outer->addWidget(cell, 1);

        if (k < 3) {
            QFrame* div = new QFrame;
            div->setFrameShape(orientation_ == Qt::Horizontal ? QFrame::VLine : QFrame::HLine);
            div->setFrameShadow(QFrame::Sunken);
            dividers_[k] = div;
            outer->addWidget(div);
        }
    }
    updateDividers();
}

// Live density switch (Overview page only). Rebuilds the cards at the new level;
// the Overview page re-applies corner visibility and re-feeds update().
void TyreCardsWidget::setLevel(Level level) {
    if (level_ == level) return;
    level_ = level;
    buildCards();
}

void TyreCardsWidget::setCornerVisible(int i, bool on)
{
    if (i < 0 || i >= 4) return;
    cornerVisible_[i] = on;
    if (cards_[i]) cards_[i]->setVisible(on);                 // non-compact: single card widget
    for (QWidget* w : compactCorner_[i]) if (w) w->setVisible(on);   // compact grid: the corner's cells
    if (compactGrid_) {
        // Collapse the hidden corner's half-columns so the visible corners expand.
        compactGrid_->setColumnStretch(4 * i,     on ? 1 : 0);
        compactGrid_->setColumnStretch(4 * i + 2, on ? 1 : 0);
    }
    updateDividers();
}

void TyreCardsWidget::updateDividers()
{
    // Use the logical visibility we track, NOT cards_[]->isVisible(): this runs from
    // applyLayout during construction (before the page is ever shown), when every
    // card's realized isVisible() is false — which would otherwise hide every
    // divider permanently.
    //
    // dividers_[d] sits between corner d and d+1, i.e. it precedes corner d+1. Show
    // it only when corner d+1 is visible AND some earlier corner is visible — so
    // there's exactly one separator between consecutive visible corners (no leading
    // separator, and none stranded next to a hidden corner).
    bool anyEarlier = false;
    for (int k = 0; k < 4; ++k) {
        if (k > 0 && dividers_[k - 1])
            dividers_[k - 1]->setVisible(cornerVisible_[k] && anyEarlier);
        anyEarlier = anyEarlier || cornerVisible_[k];
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
