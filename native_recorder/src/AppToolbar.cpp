#include "AppToolbar.h"
#include "IconUtils.h"

#include <QApplication>
#include <QAction>
#include <QButtonGroup>
#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QTimer>
#include <QToolButton>

// Chart window-size options, shown as a segmented toolbar control.
static const struct { const char* label; float secs; } kWindowOptions[] = {
    {"15s", 15}, {"30s", 30}, {"1m", 60},
    {"2m", 120}, {"5m", 300}, {"10m", 600}
};
static constexpr int kWindowOptionCount = 6;

static constexpr int kToolbarHeight = 44;

namespace {

// Segmented-control button for the toolbar's window-size picker. When checked it
// paints itself as the active QStyle's *default button* — the same blue outline
// the Edit-Layout dialog's on-toggles wear (see ToggleButton in
// EditOverviewLayoutDialog.cpp). QToolButton can't reuse that trick directly:
// the DefaultButton look lives on QStyleOptionButton, which only QPushButton
// feeds the style, so for the checked state we draw a default QPushButton bevel
// + label ourselves (same CE_PushButton + DefaultButton code path the dialog
// hits). Unchecked segments fall through to the normal flat auto-raised look.
class SegmentButton : public QToolButton {
public:
    using QToolButton::QToolButton;

protected:
    void paintEvent(QPaintEvent* e) override {
        if (!isChecked()) { QToolButton::paintEvent(e); return; }
        QStylePainter p(this);
        QStyleOptionButton opt;
        opt.initFrom(this);
        opt.rect = rect();
        opt.text = text();
        opt.features = QStyleOptionButton::DefaultButton;
        opt.state |= QStyle::State_Raised;
        opt.state &= ~(QStyle::State_On | QStyle::State_Sunken);
        p.drawControl(QStyle::CE_PushButton, opt);
    }
};

// No bundled SVG for these — prefer the OS/desktop theme icon (tinted to the
// toolbar foreground on Windows so the monochrome Breeze icons stay visible in
// dark mode), and fall back to Qt's own built-in standard-pixmap icon.
QIcon openRecordingIcon(QWidget* w) {
    return adaptThemeIcon(QIcon::fromTheme(QIcon::ThemeIcon::DocumentOpen),
        w->palette().color(QPalette::WindowText),
        w->style()->standardIcon(QStyle::SP_DialogOpenButton));
}

QIcon editLayoutIcon(QWidget* w) {
    return adaptThemeIcon(QIcon::fromTheme("document-edit"),
        w->palette().color(QPalette::WindowText),
        w->style()->standardIcon(QStyle::SP_FileDialogDetailedView));
}

QIcon settingsIcon(QWidget* w) {
    return adaptThemeIcon(
        QIcon::fromTheme("configure", QIcon::fromTheme("preferences-system")),
        w->palette().color(QPalette::WindowText),
        w->style()->standardIcon(QStyle::SP_FileDialogListView));
}

// "⋯" overflow button — same theme-icon-with-fallback pattern as the icons
// above, ending at the style's horizontal-extension glyph (what Qt's own overflow
// button would use) so it always renders even when the theme lacks an overflow icon.
QIcon overflowIcon(QWidget* w) {
    return adaptThemeIcon(
        QIcon::fromTheme("overflow-menu-symbolic"),
        w->palette().color(QPalette::WindowText),
        w->style()->standardIcon(QStyle::SP_ToolBarHorizontalExtensionButton));
}

} // namespace

AppToolbar::AppToolbar(const QStringList& pageNames, bool showLabels, QWidget* parent)
    : QToolBar(parent)
{
    setMovable(false);
    setFloatable(false);
    setToolButtonStyle(showLabels ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
    setContentsMargins(0, 0, 0, 0);
    if (layout()) layout()->setContentsMargins(0, 0, 0, 0);
    if (layout()) layout()->setSpacing(4);
    // Breeze (and other styles) draw the QToolBar's own 1px bottom border across
    // its full width regardless of what the tab bar does — left alone, it shows
    // up as a second line stacked right under our accent underline. QToolBar also
    // reserves its own internal padding/margin around every item regardless of
    // a widget's own size policy — zeroing it here too, since that inset (not
    // anything in the page buttons themselves) was the source of the remaining
    // gap between the active-page underline and the toolbar's true bottom edge.
    setFixedHeight(kToolbarHeight);
    // Sets the toolbar stylesheet (border/margins, + the palette Button shade
    // background — see the method).
    updateColorScheme();

    // Page switcher: plain checkable QToolButtons in an exclusive group, same
    // approach as the window-size segmented control below. QTabBar was tried
    // first, but it has a hardcoded internal paint call (PE_FrameTabBarBase)
    // for the base line under inactive tabs that's supposed to be suppressed by
    // documentMode(true) and isn't, in this style/Qt-version combination — and
    // that line isn't reachable through any stylesheet rule. Plain QToolButtons
    // have no such native "tab base" painting path, so there's nothing for an
    // unselected button to draw beyond what its own (empty) stylesheet says.
    QWidget* pageTabsWidget = new QWidget;
    pageTabsWidget->setFixedHeight(kToolbarHeight);
    QHBoxLayout* pageTabsLay = new QHBoxLayout(pageTabsWidget);
    pageTabsLay->setContentsMargins(0, 0, 0, 0);
    pageTabsLay->setSpacing(4);
    pageGroup_ = new QButtonGroup(this);
    pageGroup_->setExclusive(true);
    static constexpr int kUnderlineWidth = 2;
    const QString accent = QApplication::palette().color(QPalette::Highlight).name();
    // Every button — checked or not — reserves the same border-bottom width
    // (transparent unless checked), so switching pages only changes its color,
    // never shifts the text within the button's fixed height.
    const QString pageBtnStyle = QString(
        "QToolButton { padding: 0px 14px; border: none; background: transparent;"
        " border-bottom: %1px solid transparent; }"
        "QToolButton:checked { border-bottom: %1px solid %2; }"
    ).arg(kUnderlineWidth).arg(accent);
    for (int i = 0; i < pageNames.size(); ++i) {
        QToolButton* b = new QToolButton;
        b->setText(pageNames[i]);
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setFixedHeight(kToolbarHeight - 2);
        b->setStyleSheet(pageBtnStyle);
        // Pin each tab to its natural width so it can't compress: when space runs
        // out the only way the strip shrinks is by *hiding* a whole tab (handled by
        // relayout). Otherwise the buttons and the toolbar both try to shrink
        // at once and the overflow measurement never settles — that was the flicker.
        b->ensurePolished();
        b->setFixedWidth(b->sizeHint().width());
        pageGroup_->addButton(b, i);
        pageTabsLay->addWidget(b);
        pageButtons_.push_back(b);
    }
    static_cast<QToolButton*>(pageGroup_->button(0))->setChecked(true);   // default: first page
    addWidget(pageTabsWidget);

    connect(pageGroup_, &QButtonGroup::idClicked, this, [this](int i) {
        currentPage_ = i;
        relayout();      // the active tab is kept inline — re-evaluate overflow
        emit pageSelected(i);
    });

    // Expanding spacer that pushes the right-hand group over. The session timer
    // rides at the right edge of this spacer (just left of the window segment) so
    // it stays clear of the overflow logic — it's not a standalone toolbar item, so
    // the relayout arithmetic below is untouched and the timer is always visible.
    QWidget* spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    QHBoxLayout* spacerLay = new QHBoxLayout(spacer);
    spacerLay->setContentsMargins(0, 0, 0, 0);
    spacerLay->setSpacing(0);
    spacerLay->addStretch(1);
    timerLabel_ = new QLabel;
    timerLabel_->setObjectName("sessionTimer");
    timerLabel_->setContentsMargins(8, 0, 8, 0);
    timerLabel_->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
    QFont timerFont = timerLabel_->font();
    timerFont.setBold(true);
    timerLabel_->setFont(timerFont);
    timerLabel_->setToolTip("Session time");
    timerLabel_->hide();   // shown once the first session_time arrives
    spacerLay->addWidget(timerLabel_);
    addWidget(spacer);

    // Window-size segmented control: an exclusive row of checkable QToolButtons
    // drawn edge-to-edge, styled by the active QStyle (so it renders native to
    // whatever platform/theme is running, not a fixed look).
    windowSeg_ = new QWidget;
    QHBoxLayout* segLay = new QHBoxLayout(windowSeg_);
    segLay->setContentsMargins(0, 0, 0, 0);
    segLay->setSpacing(0);
    windowGroup_ = new QButtonGroup(this);
    windowGroup_->setExclusive(true);
    for (int i = 0; i < kWindowOptionCount; ++i) {
        SegmentButton* b = new SegmentButton;
        b->setText(kWindowOptions[i].label);
        b->setCheckable(true);
        b->setAutoRaise(true);
        windowGroup_->addButton(b, i);
        segLay->addWidget(b);
    }
    static_cast<QToolButton*>(windowGroup_->button(1))->setChecked(true);   // default 30s
    windowAct_ = addWidget(windowSeg_);
    connect(windowGroup_, &QButtonGroup::idClicked, this, [this](int idx) { applyChartWindow(idx); });

    openAct_ = addAction(openRecordingIcon(this), "Open Recording");
    connect(openAct_, &QAction::triggered, this, &AppToolbar::openRecordingRequested);
    editLayoutAct_ = addAction(editLayoutIcon(this), "Edit Layout");
    editLayoutAct_->setEnabled(true); // Default is the Overview page (0)
    connect(editLayoutAct_, &QAction::triggered, this, &AppToolbar::editLayoutRequested);
    settingsAct_ = addAction(settingsIcon(this), "Settings");
    connect(settingsAct_, &QAction::triggered, this, &AppToolbar::settingsRequested);

    // Custom overflow: the toolbar is built from composite custom widgets (page
    // tabs, window-size segment), which Qt's native QToolBarExtension can't reparent
    // into its popup. Instead we manage it ourselves — relayout() collapses
    // low-priority items into this "⋯" button's menu when the window is too narrow,
    // so the broken native extension never appears.
    overflowMenu_ = new QMenu(this);
    overflowBtn_  = new QToolButton;
    overflowBtn_->setAutoRaise(true);
    overflowBtn_->setIcon(overflowIcon(this));
    overflowBtn_->setPopupMode(QToolButton::InstantPopup);
    overflowBtn_->setMenu(overflowMenu_);
    overflowBtn_->setToolTip("More");
    // Control visibility via the toolbar action so its slot is fully removed when
    // hidden (toggling just the widget leaves a reserved empty slot).
    overflowAct_ = addWidget(overflowBtn_);
    overflowAct_->setVisible(false);
    // Disable Qt's own overflow: its extension button (objectName "qt_toolbar_ext_button")
    // would otherwise flash in/out as items reflow, fighting our ⋯ menu. Keep it
    // permanently hidden via the event filter below.
    extButton_ = findChild<QWidget*>("qt_toolbar_ext_button");
    if (extButton_) {
        extButton_->hide();
        extButton_->installEventFilter(this);
    }
    // Initial pass once the toolbar has a real width (after the window is shown).
    QTimer::singleShot(0, this, [this] { relayout(); });
}

void AppToolbar::setEditLayoutEnabled(bool on) {
    if (editLayoutAct_) editLayoutAct_->setEnabled(on);
}

void AppToolbar::setShowLabels(bool on) {
    setToolButtonStyle(on ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
    relayout();   // text-beside-icon changes the icon-action widths
}

void AppToolbar::updateColorScheme() {
    // The toolbar sits on the palette's Button shade (a touch lighter than the
    // window) with a hairline bottom border separating it from the page content
    // below — the same divider style as the tab-bar/pane seam in the Settings
    // window. The border colour is ~30% of the way from the window colour toward
    // the text colour so it stays visible in both light and dark themes; it's
    // recomputed here so it re-themes when the colour scheme changes.
    // (background: palette(button) already neutralises Breeze's OS-scheme "tools
    // area" tint unconditionally, so no style/mode-specific branch is needed.)
    const QColor win = QApplication::palette().color(QPalette::Window);
    const QColor txt = QApplication::palette().color(QPalette::WindowText);
    const QColor borderCol((win.red()   * 7 + txt.red()   * 3) / 10,
                           (win.green() * 7 + txt.green() * 3) / 10,
                           (win.blue()  * 7 + txt.blue()  * 3) / 10);
    setStyleSheet(QString(
        "QToolBar { border: none; border-bottom: 1px solid %1;"
        " margin: 0px; padding: 0px; background: palette(button); }").arg(borderCol.name()));
}

// Toolbar session timer. `sessionTime` is the header session_time carried on
// every packet (live UDP and playback): elapsed seconds since session start.
// Formatted M:SS to mirror the Electron titlebar timer. Guarded on the whole
// second so a burst of same-frame packets doesn't re-set the label needlessly.
void AppToolbar::updateSessionTimer(float sessionTime) {
    if (!timerLabel_ || sessionTime < 0.0f) return;
    const int total = (int)sessionTime;
    if (total == timerSec_ && timerLabel_->isVisible()) return;
    timerSec_ = total;
    timerLabel_->setText(QString("%1:%2")
                             .arg(total / 60)
                             .arg(total % 60, 2, 10, QLatin1Char('0')));
    if (!timerLabel_->isVisible()) timerLabel_->show();
    // The label's footprint changed if it just appeared or grew a digit
    // (e.g. 9:59 → 10:00). Re-run the responsive layout so the reserved timer
    // width stays correct and the toolbar never overflows.
    const int w = timerLabel_->sizeHint().width();
    if (w != timerW_) { timerW_ = w; relayout(); }
}

void AppToolbar::resetSessionTimer() {
    timerSec_ = -1;
    timerW_   = 0;
    if (timerLabel_) {
        timerLabel_->clear();
        timerLabel_->hide();
        relayout();   // reclaim the freed width for the inline items
    }
}

void AppToolbar::resizeEvent(QResizeEvent* e) {
    QToolBar::resizeEvent(e);
    relayout();   // collapse/expand items for the new width
}

void AppToolbar::changeEvent(QEvent* e) {
    QToolBar::changeEvent(e);
    if (e->type() == QEvent::ApplicationPaletteChange || e->type() == QEvent::PaletteChange)
        refreshThemedIcons();
}

bool AppToolbar::eventFilter(QObject* obj, QEvent* e) {
    // Qt's QToolBarLayout re-shows its extension button whenever it thinks items
    // overflow; hide it again every time so only our ⋯ overflow is ever seen.
    if (obj == extButton_ &&
        (e->type() == QEvent::Show || e->type() == QEvent::ShowToParent))
        extButton_->hide();
    return QToolBar::eventFilter(obj, e);
}

void AppToolbar::refreshThemedIcons() {
    // The theme icons are tinted to the palette foreground (on Windows), so
    // rebuild them whenever the palette changes to re-tint for the new mode.
    if (openAct_)       openAct_->setIcon(openRecordingIcon(this));
    if (editLayoutAct_) editLayoutAct_->setIcon(editLayoutIcon(this));
    if (settingsAct_)   settingsAct_->setIcon(settingsIcon(this));
    if (overflowBtn_)   overflowBtn_->setIcon(overflowIcon(this));
}

void AppToolbar::applyChartWindow(int idx) {
    if (idx < 0 || idx >= kWindowOptionCount) return;
    windowIdx_ = idx;
    emit chartWindowChanged(kWindowOptions[idx].secs);
    // Keep the inline segment in sync when the choice came from the overflow menu
    // (a programmatic setChecked emits idToggled, not idClicked, so no recursion).
    if (windowGroup_)
        if (auto* b = windowGroup_->button(idx))
            if (!b->isChecked()) b->setChecked(true);
}

// Collapse low-priority toolbar items into the "⋯" menu when the window is too
// narrow to fit everything, expanding them back as it widens. Order of collapse:
// icon actions → window-size segment → page tabs (right-to-left, active tab kept).
void AppToolbar::relayout() {
    if (!overflowAct_ || pageButtons_.empty()) return;
    const int avail = width();
    if (avail <= 0) return;
    const int spacing = layout() ? layout()->spacing() : 4;
    const int n = (int)pageButtons_.size();
    // Deliberate slack so we always collapse a little EARLY rather than ever let the
    // toolbar genuinely overflow. A real overflow makes Qt's (suppressed) extension
    // button fight our event filter — show/hide/show — which is the flicker. Showing
    // the ⋯ a few px sooner is harmless; oscillation is not.
    constexpr int kSlack = 40;

    auto actW = [&](QAction* a) -> int {
        QWidget* w = a ? widgetForAction(a) : nullptr;
        return w ? w->sizeHint().width() : 0;
    };
    auto setIconsVisible = [&](bool v) {
        if (openAct_)       openAct_->setVisible(v);
        if (editLayoutAct_) editLayoutAct_->setVisible(v);
        if (settingsAct_)   settingsAct_->setVisible(v);
    };

    // Pure arithmetic from stable sizeHints (tabs are fixed-width; the rest are
    // content-sized and don't depend on the live layout), so the decision is
    // deterministic — identical for a given width every call, no oscillation.
    std::vector<int> tabW(n);
    int sumTabs = 0;
    for (int i = 0; i < n; ++i) { tabW[i] = pageButtons_[i]->sizeHint().width(); sumTabs += tabW[i]; }
    const int wSeg   = windowSeg_->sizeHint().width();
    const int wIcons = actW(openAct_) + actW(editLayoutAct_) + actW(settingsAct_);
    const int wOver  = overflowBtn_->sizeHint().width();
    // The session timer lives inside the (otherwise collapsible) spacer, so the
    // spacer can no longer shrink to 0 — it must always reserve the timer's width.
    // The timer is persistent: it never collapses into the overflow menu, so this
    // width stays in the inline budget throughout and is never subtracted off.
    const int wTimer = (timerLabel_ && timerLabel_->isVisible())
                           ? timerLabel_->sizeHint().width() : 0;
    // Inter-item gaps: 6 toolbar items (tab strip, spacer, seg, 3 icons) → 5 gaps,
    // plus the tab strip's own gaps between its n buttons. The timer adds no gap of
    // its own (it rides inside the spacer).
    const int needAll = sumTabs + wSeg + wIcons + wTimer + spacing * 5 + spacing * (n - 1);

    if (avail >= needAll + kSlack) {              // comfortably fits — everything inline
        if (windowAct_) windowAct_->setVisible(true);
        setIconsVisible(true);
        for (auto* b : pageButtons_) b->setVisible(true);
        overflowAct_->setVisible(false);
        return;
    }

    // Need overflow. Collapse units (icons → window segment → tabs R→L, active tab
    // kept) until inline content + ⋯ leaves at least kSlack of headroom.
    overflowAct_->setVisible(true);
    const int budget = avail - wOver - spacing - kSlack;
    int inlineW = needAll;
    bool segIn = true, iconsIn = true;
    std::vector<bool> tabIn(n, true);
    if (inlineW > budget && iconsIn) { iconsIn = false; inlineW -= wIcons + spacing; }
    if (inlineW > budget && segIn)   { segIn   = false; inlineW -= wSeg + spacing; }
    for (int i = n - 1; i >= 0 && inlineW > budget; --i) {
        if (i == currentPage_) continue;          // always keep the active tab inline
        tabIn[i] = false;
        inlineW -= tabW[i] + spacing;
    }

    if (windowAct_) windowAct_->setVisible(segIn);
    setIconsVisible(iconsIn);
    for (int i = 0; i < n; ++i) pageButtons_[i]->setVisible(tabIn[i]);

    // Rebuild the overflow menu from whatever collapsed.
    overflowMenu_->clear();
    for (int i = 0; i < n; ++i) {
        if (tabIn[i]) continue;
        // Tabs are navigation, not state — render as plain buttons (no checkbox).
        QAction* a = overflowMenu_->addAction(pageButtons_[i]->text());
        connect(a, &QAction::triggered, this, [this, i] {
            if (pageGroup_) if (auto* b = pageGroup_->button(i)) b->click();
        });
    }
    if (!segIn) {
        overflowMenu_->addSection("Chart Window");
        for (int i = 0; i < kWindowOptionCount; ++i) {
            QAction* a = overflowMenu_->addAction(kWindowOptions[i].label);
            a->setCheckable(true);
            a->setChecked(i == windowIdx_);
            connect(a, &QAction::triggered, this, [this, i] { applyChartWindow(i); });
        }
    }
    if (!iconsIn) {
        overflowMenu_->addSeparator();
        QAction* mo = overflowMenu_->addAction(openRecordingIcon(this), "Open Recording");
        connect(mo, &QAction::triggered, openAct_, &QAction::trigger);
        QAction* me = overflowMenu_->addAction(editLayoutIcon(this), "Edit Layout");
        me->setEnabled(editLayoutAct_ && editLayoutAct_->isEnabled());
        connect(me, &QAction::triggered, editLayoutAct_, &QAction::trigger);
        QAction* ms = overflowMenu_->addAction(settingsIcon(this), "Settings");
        connect(ms, &QAction::triggered, settingsAct_, &QAction::trigger);
    }
}
