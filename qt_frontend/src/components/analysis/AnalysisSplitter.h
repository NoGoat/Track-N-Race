#pragma once

#include <QSplitter>
#include <QSplitterHandle>

class AnalysisSplitBadge;

// Split view's divider between the graphs and the map. Unlike a plain
// QSplitter it keeps a *ratio* (the first pane's share of the width), so the
// split survives window resizes as a percentage, as in the Electron app.
// While the handle is hovered or dragged a small badge shows the split as
// "graphs / map" percentages; double-clicking the handle restores 50 / 50.
class AnalysisSplitter : public QSplitter {
    Q_OBJECT
public:
    static constexpr double kDefaultRatio = 0.5;
    static constexpr double kMinRatio = 0.2;
    static constexpr double kMaxRatio = 0.8;

    explicit AnalysisSplitter(QWidget* parent = nullptr);
    ~AnalysisSplitter() override;

    double ratio() const { return ratio_; }
    // Stores and applies a ratio (clamped); does not emit ratioChanged.
    void setRatio(double ratio);
    // Re-lays the panes to the stored ratio, e.g. after one was shown again.
    void applyRatio();

signals:
    // The user moved the divider or reset it.
    void ratioChanged(double ratio);

protected:
    QSplitterHandle* createHandle() override;
    void resizeEvent(QResizeEvent* event) override;

private:
    friend class AnalysisSplitterHandle;

    double ratio_ = kDefaultRatio;
    bool hovered_ = false;
    bool dragging_ = false;
    AnalysisSplitBadge* badge_ = nullptr;   // on the window; created on first hover

    bool bothVisible() const;
    void onMoved();
    void resetToDefault();
    void setHovered(bool hovered);
    void setDragging(bool dragging);
    void refreshBadge();
};

class AnalysisSplitterHandle : public QSplitterHandle {
    Q_OBJECT
public:
    AnalysisSplitterHandle(Qt::Orientation orientation, AnalysisSplitter* parent);

protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    AnalysisSplitter* owner_ = nullptr;
};
