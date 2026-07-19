#pragma once

#include <QToolBar>
#include <QStringList>

#include <vector>

class QAction;
class QButtonGroup;
class QComboBox;
class QLabel;
class QMenu;
class QToolButton;

// The main window's top toolbar: page tabs, session timer, chart window-size
// segmented control, the Open/Edit Layout/Settings actions, and the custom "⋯"
// overflow that collapses low-priority items when the window is too narrow.
// Self-contained: owns its widgets, overflow relayout, themed-icon refresh and
// the session-timer label. The owner reacts to the signals below.
class AppToolbar : public QToolBar {
    Q_OBJECT

public:
    // pageNames must match the owner's page-stack order (its Page enum).
    AppToolbar(const QStringList& pageNames, bool showLabels, QWidget* parent = nullptr);

    void setEditLayoutEnabled(bool on);
    void setAnalyzeControlsVisible(bool on);
    void setAnalyzeControlsEnabled(bool on);
    void setShowLabels(bool on);        // icon-only ↔ text-beside-icon
    // Forces the toolbar to the palette's Button shade with a hairline bottom
    // border; recomputed on theme/style changes so it re-themes correctly.
    void updateColorScheme();

    // Session timer label (header session_time, formatted M:SS). Lives inside
    // the expanding spacer so it never joins the overflow arithmetic.
    void updateSessionTimer(float sessionTime);
    void resetSessionTimer();

signals:
    void pageSelected(int index);
    void chartWindowChanged(float seconds);
    void openRecordingRequested();
    void editLayoutRequested();
    void settingsRequested();
    void analyzeZoomInRequested();
    void analyzeZoomOutRequested();
    void analyzePanLeftRequested();
    void analyzePanRightRequested();
    void analyzeResetZoomRequested();

protected:
    void resizeEvent(QResizeEvent* e) override;   // collapse/expand for the new width
    // Rebuilds the tinted theme icons when the application palette changes
    // (e.g. Light↔Dark), so they re-tint to the new foreground colour.
    void changeEvent(QEvent* e) override;
    // Keeps Qt's native toolbar extension button (extButton_) hidden — we do
    // our own overflow via the ⋯ menu.
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void relayout();                   // collapse/expand into the ⋯ menu
    void applyChartWindow(int idx);    // sync inline/menu state + emit chartWindowChanged
    void refreshThemedIcons();

    QButtonGroup* pageGroup_    = nullptr;   // exclusive page tabs
    QComboBox*    windowBtn_    = nullptr;   // window-size dropdown (frameless combo box)
    QAction*      windowAct_    = nullptr;   // its toolbar action (hide to free space)
    QAction*      analyzeAct_   = nullptr;   // Analyze-only zoom/pan controls
    QWidget*      analyzeControls_ = nullptr;
    QToolButton*  analyzeZoomIn_ = nullptr;
    QToolButton*  analyzeZoomOut_ = nullptr;
    QToolButton*  analyzePanLeft_ = nullptr;
    QToolButton*  analyzePanRight_ = nullptr;
    QToolButton*  analyzeReset_ = nullptr;
    QToolButton*  overflowBtn_  = nullptr;   // "⋯" menu button (hidden until needed)
    QAction*      overflowAct_  = nullptr;   // its toolbar action (toggle visibility)
    QMenu*        overflowMenu_ = nullptr;
    QWidget*      extButton_    = nullptr;   // Qt's native QToolBar extension — kept hidden
    std::vector<QToolButton*> pageButtons_;  // the page-tab buttons
    int           windowIdx_    = 1;         // selected window-size option (default 30s)
    int           currentPage_  = 0;         // active tab — kept inline during overflow
    bool          analyzeVisible_ = false;

    QAction*  openAct_       = nullptr;
    QAction*  editLayoutAct_ = nullptr;
    QAction*  settingsAct_   = nullptr;

    QLabel*   timerLabel_ = nullptr;
    int       timerSec_   = -1;        // last shown whole second (skip redundant sets)
    int       timerW_     = 0;         // last reserved label width (re-layout on change)
};
