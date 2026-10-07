#pragma once

#include <QColor>
#include <QFrame>
#include <QString>

#include "../CompactSettings.h"

class QLabel;

// Electron's Overview StatCard as the Trends and Damage pages use it: an
// uppercase label, the value with its unit, and — in Spacious only — a sub
// line under the value. Compact collapses it to one line. The unit hides
// while the value is missing ("—"), and the card's tooltip is its hover title.
class SummaryCard : public QFrame {
    Q_OBJECT

public:
    static const QString kMissing;   // "—"

    SummaryCard(const QString& label, const QString& unit, tnr::DensityMode density,
                QWidget* parent = nullptr);

    void setLabel(const QString& label);
    // An invalid colour keeps the default text colour.
    void setValue(const QString& text, const QColor& color = QColor());
    // Shown in Spacious only, as Electron passes `sub` only there.
    void setSub(const QString& text);

private:
    tnr::DensityMode density_;
    QLabel* label_ = nullptr;
    QLabel* value_ = nullptr;
    QLabel* unit_ = nullptr;
    QLabel* sub_ = nullptr;
    QColor valueColor_;
};
