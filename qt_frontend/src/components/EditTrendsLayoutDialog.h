#pragma once

#include <QDialog>
#include "TrendsLayout.h"

class TrendsPage;

// Electron's Trends layout editor: the summary cards, then the charts of the
// chart layout in use — the Separate layout's four, or the single combined
// or bar chart — applied and saved as each toggle changes.
class EditTrendsLayoutDialog : public QDialog {
    Q_OBJECT

public:
    explicit EditTrendsLayoutDialog(TrendsPage* page, QWidget* parent = nullptr);

private:
    TrendsPage*  page_;
    TrendsLayout layout_;
};
