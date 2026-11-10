#pragma once

#include <QAbstractButton>
#include <QMainWindow>
#include <QPointer>
#include <QScrollArea>
#include <QSplitter>
#include <QSplitterHandle>
#include <QToolButton>
#include <QVector>

#include <functional>

class QDockWidget;
class QVBoxLayout;

// Every resizable boundary on the Analysis page is drawn the same way: a
// visible line, in the highlight colour while it is hovered or dragged; the
// whole strip around it can be grabbed. Horizontal when `rect` is wider than tall.
void analysisPaintGrip(QPainter& painter, const QRect& rect, const QPalette& palette, bool hot);
// The selected row of an Analysis list, drawn like the segmented controls'
// selected segment (the style's default-button outline) instead of a solid
// highlight. `rect` is the whole row.
void analysisPaintSelectedRow(QPainter* painter, const QRect& rect, const QPalette& palette,
                              bool enabled, const QWidget* widget);
// The width of such a boundary, wide enough to grab.
constexpr int kAnalysisGripExtent = 7;

// A splitter whose handles are drawn as grips.
class AnalysisGripSplitter : public QSplitter {
    Q_OBJECT
public:
    explicit AnalysisGripSplitter(Qt::Orientation orientation, QWidget* parent = nullptr);

protected:
    QSplitterHandle* createHandle() override;
};

class AnalysisGripHandle : public QSplitterHandle {
    Q_OBJECT
public:
    AnalysisGripHandle(Qt::Orientation orientation, QSplitter* parent);

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    bool hovered_ = false;
    bool pressed_ = false;
};

// A tool button whose checked state is drawn like the segmented controls'
// (tnrui::SegmentButton, e.g. Follow Playback | Fixed Laps): the style's
// default-button outline, with the button's own icon and label on top.
// Unchecked it is an ordinary auto-raised tool button.
class AnalysisToggleButton : public QToolButton {
    Q_OBJECT
public:
    using QToolButton::QToolButton;

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
};

// A pane's vertical tool strip: each action as an icon with its short label
// (QAction::iconText) under it, top to bottom. Split view is short of width,
// not height, so a column never runs out of room; on a short window it
// scrolls instead of hiding tools behind an overflow button.
class AnalysisToolStrip : public QScrollArea {
    Q_OBJECT
public:
    explicit AnalysisToolStrip(const QString& accessibleName, QWidget* parent = nullptr);

    void addTool(QAction* action);
    // Two icon-only tools sharing one row.
    void addToolPair(QAction* first, QAction* second);
    void addSeparator();
    void addCaption(const QString& text);

private:
    QWidget* body_ = nullptr;
    QVBoxLayout* layout_ = nullptr;
};

// The Analysis page's docks live in a QMainWindow used as a plain widget, so
// they get Qt's own title bars (float, close), drag-to-dock and saveState.
// Right-clicking a dock title shows the page's Panels menu. The separators
// between docks and panes are drawn here as lines (Qt's are transparent).
class AnalysisWorkspace : public QMainWindow {
    Q_OBJECT
public:
    explicit AnalysisWorkspace(QWidget* parent = nullptr);

    // Fills the menu shown on right-click; the Panels tool button uses the same.
    std::function<void(QMenu*)> fillPanelsMenu;

    QMenu* createPopupMenu() override;

protected:
    bool event(QEvent* event) override;

private:
    // The gaps between docked panels and the central panes: Qt's separators.
    QVector<QRect> separators() const;
    QRect hovered_;
};

// A closed panel's tab on the window edge, its title written sideways.
class AnalysisRailButton : public QAbstractButton {
    Q_OBJECT
public:
    AnalysisRailButton(const QString& text, bool rightSide, QWidget* parent = nullptr);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    bool rightSide_ = false;
    bool hovered_ = false;
};

// The window edge's tabs for its closed panels; hidden while it has none.
// Clicking a tab shows its panel again where it was.
class AnalysisDockRail : public QWidget {
    Q_OBJECT
public:
    explicit AnalysisDockRail(bool rightSide, QWidget* parent = nullptr);

    void setDocks(const QVector<QDockWidget*>& closed);

private:
    bool rightSide_ = false;
    QVBoxLayout* layout_ = nullptr;
    QVector<QPointer<AnalysisRailButton>> buttons_;
};
