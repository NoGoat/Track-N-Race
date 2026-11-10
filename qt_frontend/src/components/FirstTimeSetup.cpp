#include "FirstTimeSetup.h"

#include "../MainWindow.h"
#include "ConnectionCheck.h"
#include "OverviewPage.h"
#include "PcAddressList.h"
#include "PageUiHelpers.h"
#include "PairingQrCode.h"
#include "SegmentedControl.h"
#include "SetupUi.h"
#include "analysis/AnalysisWidgets.h"

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEnterEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLinearGradient>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStyle>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>
#include <cmath>
#include <functional>

using namespace tnrsetup;

namespace {

constexpr int kColumnWidth = 560;
constexpr int kWideColumnWidth = 880;
constexpr int kSideGutter = 16;
constexpr qint64 kPairWindowMs = 2 * 60 * 1000;

struct StepInfo { const char* title; const char* description; };
const StepInfo kSteps[] = {
    {"Recordings", "Save sessions to replay and analyse later."},
    {"Density", "How tightly cards and tables are packed."},
    {"Connect the game", "Tell the game where to send telemetry."},
    {"Check connection", "Makes sure telemetry can reach this PC."},
    {"Paired mode", "Show this session on an Android phone or tablet."},
    {"Summary", "Here’s what you picked. Change anything before you start."},
};

// Recording scope ids for the segmented controls, in display order.
const char* const kScopeValues[] = {"all_drivers", "driver_only", "both", "ask"};
const char* const kScopeLabels[] = {"All Drivers", "Driver Only", "Both", "Ask"};
struct ScopeCategory { const char* key; const char* label; };
const ScopeCategory kScopeCategories[] = {
    {"practice", "Practice Sessions"},
    {"qualifying", "Qualifying Sessions"},
    {"race", "Race Sessions"},
    {"time_trial", "Time Trial Sessions"},
};

struct DensityOption { tnr::DensityMode mode; const char* label; };
const DensityOption kDensities[] = {
    {tnr::DensityMode::Compact, "Compact"},
    {tnr::DensityMode::Normal, "Normal"},
    {tnr::DensityMode::Spacious, "Spacious"},
};

QString densityLabel(std::optional<tnr::DensityMode> mode) {
    if (!mode) return QStringLiteral("Mixed");
    for (const DensityOption& option : kDensities)
        if (option.mode == *mode) return QString::fromLatin1(option.label);
    return QStringLiteral("Normal");
}

bool isIpv4(const QString& value) {
    static const QRegularExpression octet(QStringLiteral("^[0-9]{1,3}$"));
    const QStringList parts = value.trimmed().split('.', Qt::KeepEmptyParts);
    if (parts.size() != 4) return false;
    for (const QString& part : parts)
        if (!octet.match(part).hasMatch() || part.toInt() > 255) return false;
    return true;
}

QString formatFree(qint64 bytes) {
    const double gb = bytes / 1e9;
    if (gb >= 1) return QStringLiteral("%1 GB free").arg(gb >= 100 ? QString::number(std::lround(gb))
                                                                  : QString::number(gb, 'f', 1));
    return QStringLiteral("%1 MB free").arg(std::lround(bytes / 1e6));
}

// Free bytes on the volume holding `directory`, walking up to the nearest
// folder that exists; -1 when it can't be read.
qint64 freeBytes(const QString& directory) {
    QDir dir(directory);
    for (int guard = 0; guard < 64; ++guard) {
        if (dir.exists()) {
            const QStorageInfo storage(dir.absolutePath());
            return storage.isValid() ? storage.bytesAvailable() : -1;
        }
        if (!dir.cdUp()) return -1;
    }
    return -1;
}

QString driveOf(const QString& directory) {
    static const QRegularExpression drive(QStringLiteral("^([A-Za-z]:)[\\\\/]"));
    const auto match = drive.match(directory);
    return match.hasMatch() ? match.captured(1) : QString();
}

QLabel* groupLabel(const QString& text) {
    auto* label = new QLabel(text.toUpper());
    QFont font = scaledFont(label->font(), 0.78);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 2);
    label->setFont(font);
    label->setForegroundRole(QPalette::PlaceholderText);
    label->setContentsMargins(0, 0, 0, 4);
    return label;
}

QPushButton* linkButton(const QString& text) {
    auto* button = new QPushButton(text);
    button->setFlat(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setStyleSheet(QStringLiteral(
        "QPushButton { border: none; background: transparent; padding: 0; color: palette(highlight); }"
        "QPushButton:hover { text-decoration: underline; }"));
    button->setFont(scaledFont(button->font(), 0.9));
    return button;
}

// Settings-style row: title and description on the left, the control right.
QWidget* settingRow(const QString& title, const QString& description, QWidget* control,
                    QLabel** descriptionOut = nullptr) {
    auto* row = new QWidget;
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 8, 0, 8);
    layout->setSpacing(16);
    auto* text = new QVBoxLayout;
    text->setSpacing(2);
    text->addWidget(new QLabel(title));
    QLabel* detail = nullptr;
    if (!description.isEmpty() || descriptionOut) {
        detail = muted(description);
        text->addWidget(detail);
    }
    if (descriptionOut) *descriptionOut = detail;
    layout->addLayout(text, 1);
    layout->addWidget(control, 0, Qt::AlignVCenter);
    return row;
}

QToolButton* copyButton(std::function<QString()> value) {
    auto* button = new QToolButton;
    button->setAutoRaise(true);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setIcon(themedIcon(button, "edit-copy", QStyle::SP_FileDialogDetailedView));
    button->setIconSize(QSize(12, 12));
    button->setText(QStringLiteral("Copy"));
    QObject::connect(button, &QToolButton::clicked, button, [button, value] {
        QApplication::clipboard()->setText(value());
        button->setText(QStringLiteral("Copied"));
        QTimer::singleShot(1200, button, [button] { button->setText(QStringLiteral("Copy")); });
    });
    return button;
}

} // namespace

// Footer step dots: the current step is a wide blue pill; visited steps are
// darker than ones not reached yet. Clicking a dot jumps to that step.
class SetupStepDots : public QWidget {
public:
    using QWidget::QWidget;
    std::function<void(int)> onClicked;
    int count = 0;
    int current = 0;
    std::function<bool(int)> visited;

    QSize sizeHint() const override { return {count * 12 + 12, 14}; }

protected:
    QRect dotRect(int index) const {
        int x = 0;
        for (int i = 0; i < index; ++i) x += (i == current ? 18 : 6) + 6;
        const int w = index == current ? 18 : 6;
        return {x, (height() - 6) / 2, w, 6};
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        for (int i = 0; i < count; ++i) {
            QColor color = i == current ? palette().color(QPalette::Highlight)
                : (visited && visited(i)) ? palette().color(QPalette::PlaceholderText)
                : palette().color(QPalette::Mid);
            p.setBrush(color);
            p.drawRoundedRect(dotRect(i), 3, 3);
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        for (int i = 0; i < count; ++i) {
            if (dotRect(i).adjusted(-3, -5, 3, 5).contains(event->position().toPoint())) {
                if (i != current && onClicked) onClicked(i);
                return;
            }
        }
    }
};

namespace {

// ── Density previews ────────────────────────────────────────────────────────

struct SampleLap { int lap; int ms; int delta; };

// 24 laps, an out-lap and a pit lap, as Electron's SAMPLE_LAPS: the same
// seeded sequence, so both apps show the same rows.
const QVector<SampleLap>& sampleLaps() {
    static const QVector<SampleLap> laps = [] {
        long long seed = 11;
        auto random = [&seed] { seed = (seed * 9301 + 49297) % 233280; return seed / 233280.0; };
        QVector<SampleLap> out;
        int best = INT_MAX;
        for (int lap = 1; lap <= 24; ++lap) {
            const double seconds = 91.4 + random() * 1.2 - (lap > 14 ? 0.4 : 0) +
                                   (lap == 1 ? 4.8 : 0) + (lap == 14 ? 21.6 : 0);
            const int ms = static_cast<int>(std::lround(seconds * 1000));
            best = std::min(best, ms);
            out.push_back({lap, ms, 0});
        }
        for (SampleLap& lap : out) lap.delta = lap.ms - best;
        return out;
    }();
    return laps;
}

QColor fastestColor(const QPalette& palette) {
    return palette.color(QPalette::Window).lightness() > 128 ? QColor("#7C3BA6") : QColor("#BF5FFF");
}

class SampleLapTable : public QWidget {
public:
    explicit SampleLapTable(tnr::DensityMode density, QWidget* parent = nullptr)
        : QWidget(parent), density_(density) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    int rowHeight() const {
        return density_ == tnr::DensityMode::Compact ? 22 : density_ == tnr::DensityMode::Spacious ? 42 : 30;
    }
    QSize sizeHint() const override { return {200, rowHeight() * int(sampleLaps().size() + 1)}; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        const int rh = rowHeight();
        const int pad = density_ == tnr::DensityMode::Compact ? 8 : density_ == tnr::DensityMode::Spacious ? 14 : 10;
        const int cellPx = density_ == tnr::DensityMode::Compact ? 11 : density_ == tnr::DensityMode::Spacious ? 14 : 12;
        const int headPx = density_ == tnr::DensityMode::Compact ? 9 : density_ == tnr::DensityMode::Spacious ? 11 : 10;
        const int lapW = width() * 22 / 100;
        const int timeW = width() * 42 / 100;
        const QColor text = palette().color(QPalette::WindowText);
        const QColor mutedText = palette().color(QPalette::PlaceholderText);
        QColor line = palette().color(QPalette::Mid);
        line.setAlpha(110);
        const QColor fastest = fastestColor(palette());

        QFont head = font();
        head.setPixelSize(headPx);
        head.setBold(true);
        head.setLetterSpacing(QFont::PercentageSpacing, 110);
        p.setFont(head);
        p.setPen(mutedText);
        p.drawText(QRect(pad, 0, lapW, rh), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("LAP"));
        p.drawText(QRect(lapW + pad, 0, timeW, rh), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("TIME"));
        p.drawText(QRect(0, 0, width() - pad, rh), Qt::AlignVCenter | Qt::AlignRight, QString::fromUtf8("Δ"));
        p.setPen(palette().color(QPalette::Mid));
        p.drawLine(0, rh - 1, width(), rh - 1);

        QFont cell = font();
        cell.setPixelSize(cellPx);
        QFont cellBold = cell;
        cellBold.setBold(true);
        int y = rh;
        for (const SampleLap& lap : sampleLaps()) {
            const bool best = lap.delta == 0;
            p.setFont(cell);
            p.setPen(mutedText);
            p.drawText(QRect(pad, y, lapW, rh), Qt::AlignVCenter | Qt::AlignLeft, QString::number(lap.lap));
            p.setFont(cellBold);
            p.setPen(best ? fastest : text);
            p.drawText(QRect(lapW + pad, y, timeW, rh), Qt::AlignVCenter | Qt::AlignLeft,
                       analysis::formatLapTime(lap.ms));
            p.setFont(cell);
            p.setPen(best ? fastest : palette().color(QPalette::WindowText).darker(120));
            p.drawText(QRect(0, y, width() - pad, rh), Qt::AlignVCenter | Qt::AlignRight,
                       best ? QStringLiteral("Best") : analysis::formatDelta(lap.delta / 1000.0));
            p.setPen(line);
            p.drawLine(0, y + rh - 1, width(), y + rh - 1);
            y += rh;
        }
    }

private:
    tnr::DensityMode density_;
};

// The preview canvas: stat cards on top, the lap table below, clipped to a
// fixed height with a fade at the bottom. Paints its own border, blue and
// thicker when selected.
class DensityPreviewFrame : public QWidget {
public:
    DensityPreviewFrame(tnr::DensityMode density, QWidget* parent) : QWidget(parent) {
        setFixedHeight(320);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        stats_ = new QWidget(this);
        auto* row = new QHBoxLayout(stats_);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(0);
        row->addWidget(makeOverviewStatCardPreview(QStringLiteral("Speed"), QStringLiteral("287"),
                                                   QStringLiteral("kph"), {}, density));
        row->addWidget(tnrui::vline());
        row->addWidget(makeOverviewStatCardPreview(QStringLiteral("Gear"), QStringLiteral("7"), {}, {}, density));
        row->addWidget(tnrui::vline());
        row->addWidget(makeOverviewStatCardPreview(QStringLiteral("Pos"), QStringLiteral("P3"), {},
                                                   QStringLiteral("Lap 12"), density));
        divider_ = tnrui::hline();
        divider_->setParent(this);
        table_ = new SampleLapTable(density, this);
    }

    void setSelected(bool selected) { selected_ = selected; update(); }
    void setHovered(bool hovered) { hovered_ = hovered; update(); }

protected:
    void resizeEvent(QResizeEvent*) override {
        const int inset = 2;
        const int w = width() - inset * 2;
        const int statsH = stats_->sizeHint().height();
        stats_->setGeometry(inset, inset, w, statsH);
        const int dividerH = divider_->sizeHint().height();
        divider_->setGeometry(inset, inset + statsH, w, dividerH);
        table_->setGeometry(inset, inset + statsH + dividerH, w, table_->sizeHint().height());
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
        p.setPen(Qt::NoPen);
        p.setBrush(palette().color(QPalette::Base));
        p.drawRoundedRect(r, 8, 8);
    }

public:
    // The border and the fade, drawn above the children by OverlayPainter.
    void paintOverlay(QPainter& p, const QPoint& offset) const {
        p.save();
        p.translate(offset);
        p.setRenderHint(QPainter::Antialiasing);
        const QColor base = palette().color(QPalette::Base);
        QColor clear = base;
        clear.setAlpha(0);
        QLinearGradient fade(0, height() - 40, 0, height() - 2);
        fade.setColorAt(0, clear);
        fade.setColorAt(1, base);
        QPainterPath clip;
        clip.addRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 8, 8);
        p.setClipPath(clip);
        p.fillRect(QRect(0, height() - 40, width(), 40), fade);
        p.setClipping(false);
        const QColor border = selected_ ? palette().color(QPalette::Highlight)
            : hovered_ ? palette().color(QPalette::Dark) : palette().color(QPalette::Mid);
        p.setPen(QPen(border, selected_ ? 2 : 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 8, 8);
        p.restore();
    }

private:
    QWidget* stats_ = nullptr;
    QFrame* divider_ = nullptr;
    SampleLapTable* table_ = nullptr;
    bool selected_ = false;
    bool hovered_ = false;
};

// Draws the frame's border and fade above its children.
class OverlayPainter : public QWidget {
public:
    OverlayPainter(DensityPreviewFrame* frame) : QWidget(frame), frame_(frame) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        frame->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject*, QEvent* e) override {
        if (e->type() == QEvent::Resize || e->type() == QEvent::Show) {
            setGeometry(frame_->rect());
            raise();
        }
        return false;
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        frame_->paintOverlay(p, {});
    }

private:
    DensityPreviewFrame* frame_;
};

class DensityOptionButton : public QAbstractButton {
public:
    DensityOptionButton(const DensityOption& option, QWidget* parent) : QAbstractButton(parent) {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(10);
        frame_ = new DensityPreviewFrame(option.mode, this);
        overlay_ = new OverlayPainter(frame_);
        layout->addWidget(frame_);
        auto* labelRow = new QHBoxLayout;
        label_ = new QLabel(QString::fromLatin1(option.label));
        label_->setFont(scaledFont(label_->font(), 1.0, true));
        check_ = new QLabel(QString::fromUtf8("✓"));
        check_->setFont(scaledFont(check_->font(), 1.1, true));
        QPalette palette = check_->palette();
        palette.setColor(QPalette::WindowText, palette.color(QPalette::Highlight));
        check_->setPalette(palette);
        labelRow->addWidget(label_, 1);
        labelRow->addWidget(check_);
        layout->addLayout(labelRow);
        connect(this, &QAbstractButton::toggled, this, [this](bool) { sync(); });
        sync();
    }

protected:
    void paintEvent(QPaintEvent*) override {}
    void enterEvent(QEnterEvent* e) override { frame_->setHovered(true); overlay_->update(); QAbstractButton::enterEvent(e); }
    void leaveEvent(QEvent* e) override { frame_->setHovered(false); overlay_->update(); QAbstractButton::leaveEvent(e); }

private:
    void sync() {
        frame_->setSelected(isChecked());
        overlay_->update();
        check_->setVisible(isChecked());
        label_->setForegroundRole(isChecked() ? QPalette::WindowText : QPalette::PlaceholderText);
    }

    DensityPreviewFrame* frame_ = nullptr;
    OverlayPainter* overlay_ = nullptr;
    QLabel* label_ = nullptr;
    QLabel* check_ = nullptr;
};

} // namespace

// ── FirstTimeSetup ──────────────────────────────────────────────────────────

FirstTimeSetup::FirstTimeSetup(MainWindow* mainWindow, QWidget* parent)
    : QWidget(parent), mainWindow_(mainWindow)
{
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Window);
    setAttribute(Qt::WA_DeleteOnClose);
    statuses_.fill(Status::Todo);

    // Recordings: on unless the user already turned it off, saving to the
    // chosen folder or Documents/Track N Race/Recordings.
    const QSettings settings("TrackNRace", "NativeRecorder");
    recordEnabled_ = settings.contains("autoRecord") ? mainWindow_->autoRecordEnabled() : true;
    recordDirectory_ = mainWindow_->currentOutputDirectory();
    if (recordDirectory_.isEmpty())
        recordDirectory_ = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                               .filePath(QStringLiteral("Track N Race/Recordings"));
    recordDirectory_ = QDir::toNativeSeparators(recordDirectory_);
    for (int i = 0; i < 4; ++i)
        scopes_[i] = mainWindow_->recordingScope(QString::fromLatin1(kScopeCategories[i].key));

    density_ = mainWindow_->uniformDensity().value_or(tnr::DensityMode::Normal);
    port_ = mainWindow_->udpPort();
    bindAddress_ = mainWindow_->udpBindAddress();
    showAdvanced_ = !gameStepValid();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    scroll_ = new QScrollArea(this);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setWidgetResizable(true);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll_->viewport()->setAutoFillBackground(false);
    scroll_->viewport()->installEventFilter(this);
    layout->addWidget(scroll_, 1);

    layout->addWidget(tnrui::hline());
    auto* footer = new QWidget(this);
    auto* grid = new QGridLayout(footer);
    grid->setContentsMargins(16, 12, 16, 12);
    grid->setHorizontalSpacing(8);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(2, 1);

    backButton_ = new QPushButton(QStringLiteral("Back"), footer);
    connect(backButton_, &QPushButton::clicked, this, [this] { leaveStep(stepIndex_ - 1, Status::Done); });
    grid->addWidget(backButton_, 0, 0, Qt::AlignLeft);

    // The step count hangs off the right so the dots themselves sit at the
    // centre: an equally wide blank on the left balances it.
    auto* nav = new QWidget(footer);
    auto* navLayout = new QHBoxLayout(nav);
    navLayout->setContentsMargins(0, 0, 0, 0);
    navLayout->setSpacing(10);
    stepCount_ = muted(QString(), 0.85);
    stepCount_->setWordWrap(false);
    const int countWidth = stepCount_->fontMetrics().horizontalAdvance(QStringLiteral("6 of 6")) + 2;
    stepCount_->setFixedWidth(countWidth);
    auto* balance = new QWidget(nav);
    balance->setFixedWidth(countWidth);
    dots_ = new SetupStepDots(nav);
    dots_->count = StepCount;
    dots_->visited = [this](int index) { return statuses_[index] != Status::Todo; };
    dots_->onClicked = [this](int index) { leaveStep(index, Status::Done); };
    dots_->setFixedSize(StepCount * 12 + 12, 14);
    dots_->setCursor(Qt::PointingHandCursor);
    navLayout->addWidget(balance);
    navLayout->addWidget(dots_);
    navLayout->addWidget(stepCount_);
    grid->addWidget(nav, 0, 1, Qt::AlignCenter);

    auto* actions = new QHBoxLayout;
    actions->setSpacing(8);
    actions->addStretch(1);
    skipButton_ = new QPushButton(QStringLiteral("Skip"), footer);
    connect(skipButton_, &QPushButton::clicked, this, [this] { leaveStep(stepIndex_ + 1, Status::Skipped); });
    continueButton_ = new QPushButton(QStringLiteral("Continue"), footer);
    continueButton_->setDefault(true);
    continueButton_->setStyleSheet(QStringLiteral(
        "QPushButton { background: palette(highlight); color: palette(highlighted-text); border: none;"
        " border-radius: 4px; padding: 6px 16px; }"
        "QPushButton:disabled { background: palette(mid); color: palette(placeholder-text); }"));
    connect(continueButton_, &QPushButton::clicked, this, [this] {
        if (stepIndex_ == StepCount - 1) finish();
        else leaveStep(stepIndex_ + 1, Status::Done);
    });
    actions->addWidget(skipButton_);
    actions->addWidget(continueButton_);
    grid->addLayout(actions, 0, 2);
    layout->addWidget(footer);

    parent->installEventFilter(this);

    // Adapters are listed once, off the GUI thread.
    PcAddressList::load(this, [this](QVector<tnrp::NetworkInterfaceInfo> list) {
        interfaces_ = std::move(list);
        interfacesLoaded_ = true;
        if (selectedAddress_.isEmpty()) selectedAddress_ = PcAddressList::preferredAddress(interfaces_);
        if (stepIndex_ == Game || stepIndex_ == Check || stepIndex_ == Summary) showStep(stepIndex_);
    });
}

void FirstTimeSetup::open() {
    setGeometry(parentWidget()->rect());
    showStep(0);
    show();
    raise();
    setFocus();
}

bool FirstTimeSetup::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
        raise();
    } else if (watched == scroll_->viewport() && event->type() == QEvent::Resize && stepPage_) {
        // Centre the 560px column (880px for the wide previews), keeping a
        // 16px gutter on narrow windows.
        const int width = scroll_->viewport()->width();
        auto* page = stepPage_->layout();
        if (page && page->count() >= 2) {
            const int side = qMax(kSideGutter, (width - kColumnWidth) / 2);
            const int bodyWidth = stepIndex_ == Density ? kWideColumnWidth : kColumnWidth;
            const int bodySide = qMax(kSideGutter, (width - bodyWidth) / 2);
            if (auto* title = page->itemAt(0)->layout()) title->setContentsMargins(side, 72, side, 0);
            if (auto* body = page->itemAt(1)->layout()) body->setContentsMargins(bodySide, 0, bodySide, 24);
        }
    }
    return QWidget::eventFilter(watched, event);
}

bool FirstTimeSetup::gameStepValid() const {
    const QString bind = bindAddress_.trimmed();
    return port_ >= 1 && port_ <= 65535 && (bind.isEmpty() || isIpv4(bind));
}

void FirstTimeSetup::showStep(int index) {
    stepIndex_ = qBound(0, index, StepCount - 1);
    pairingBody_ = nullptr;
    freeSpaceLabel_ = nullptr;

    auto* page = new QWidget;
    page->setAutoFillBackground(false);
    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);

    auto* titleLayout = new QVBoxLayout;
    titleLayout->setSpacing(6);
    auto* title = new QLabel(QString::fromUtf8(kSteps[stepIndex_].title));
    title->setFont(scaledFont(title->font(), 1.6, true));
    titleLayout->addWidget(title);
    auto* description = muted(QString::fromUtf8(kSteps[stepIndex_].description), 0.95);
    description->setContentsMargins(0, 0, 0, 20);
    titleLayout->addWidget(description);
    pageLayout->addLayout(titleLayout);

    auto* bodyLayout = new QVBoxLayout;
    bodyLayout->setSpacing(0);
    QWidget* body = nullptr;
    switch (stepIndex_) {
        case Recordings: body = buildRecordingsStep(); break;
        case Density:    body = buildDensityStep(); break;
        case Game:       body = buildGameStep(); break;
        case Check:      body = buildCheckStep(); break;
        case Pairing:    body = buildPairingStep(); break;
        case Summary:    body = buildSummaryStep(); break;
        default:         body = new QWidget; break;
    }
    bodyLayout->addWidget(body);
    pageLayout->addLayout(bodyLayout);
    pageLayout->addStretch(1);

    QWidget* old = scroll_->takeWidget();
    stepPage_ = page;
    scroll_->setWidget(page);
    if (old) old->deleteLater();
    scroll_->verticalScrollBar()->setValue(0);
    // Apply the column margins for the current width.
    QResizeEvent resize(scroll_->viewport()->size(), scroll_->viewport()->size());
    eventFilter(scroll_->viewport(), &resize);
    refreshFooter();
}

void FirstTimeSetup::refreshFooter() {
    const bool last = stepIndex_ == StepCount - 1;
    backButton_->setVisible(stepIndex_ > 0);
    skipButton_->setVisible(!last);
    continueButton_->setText(last ? QStringLiteral("Start using Track N Race") : QStringLiteral("Continue"));
    continueButton_->setEnabled(stepIndex_ != Game || gameStepValid());
    dots_->current = stepIndex_;
    dots_->update();
    stepCount_->setText(QStringLiteral("%1 of %2").arg(stepIndex_ + 1).arg(int(StepCount)));
}

void FirstTimeSetup::commitStep(Step step) {
    switch (step) {
        case Recordings:
            if (recordEnabled_) QDir().mkpath(recordDirectory_);
            for (int i = 0; i < 4; ++i)
                mainWindow_->setRecordingScope(QString::fromLatin1(kScopeCategories[i].key), scopes_[i]);
            mainWindow_->setOutputDirectory(recordDirectory_);
            mainWindow_->setAutoRecord(recordEnabled_);
            break;
        case Density:
            if (mainWindow_->uniformDensity() != density_) mainWindow_->setAllDensity(density_);
            break;
        case Game: {
            const QString bind = bindAddress_.trimmed().isEmpty() ? QStringLiteral("0.0.0.0") : bindAddress_.trimmed();
            if (gameStepValid() && (port_ != mainWindow_->udpPort() || bind != mainWindow_->udpBindAddress()))
                mainWindow_->applyUdpConfiguration(port_, bind, mainWindow_->udpForwardingEnabled(),
                                                   mainWindow_->udpForwardTargets());
            break;
        }
        default:
            break;
    }
}

void FirstTimeSetup::leaveStep(int target, Status outcome) {
    target = qBound(0, target, StepCount - 1);
    if (outcome == Status::Done) commitStep(static_cast<Step>(stepIndex_));
    if (stepIndex_ != Summary) statuses_[stepIndex_] = outcome;
    showStep(target);
}

void FirstTimeSetup::finish() {
    QSettings("TrackNRace", "NativeRecorder").setValue("setup/completed", true);
    emit finished();
    close();
}

// ── Recordings ──────────────────────────────────────────────────────────────

void FirstTimeSetup::refreshFreeSpace() {
    if (!freeSpaceLabel_) return;
    freeSpaceLabel_->setText(QStringLiteral("Where recordings are saved."));
    const QString directory = recordDirectory_;
    runAsync<qint64>(freeSpaceLabel_.data(), [directory] { return freeBytes(directory); },
        [this, directory](qint64 bytes) {
            if (!freeSpaceLabel_ || directory != recordDirectory_ || bytes < 0) return;
            const QString drive = driveOf(directory);
            freeSpaceLabel_->setText(QStringLiteral("Where recordings are saved. %1%2")
                                         .arg(formatFree(bytes),
                                              drive.isEmpty() ? QString() : QStringLiteral(" on ") + drive));
        });
}

QWidget* FirstTimeSetup::buildRecordingsStep() {
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toggle = new QCheckBox;
    toggle->setChecked(recordEnabled_);
    layout->addWidget(settingRow(QStringLiteral("Record Session Data"),
                                 QStringLiteral("Save live telemetry as .tnrd files."), toggle));

    auto* dependent = new QWidget;
    auto* dependentLayout = new QVBoxLayout(dependent);
    dependentLayout->setContentsMargins(0, 0, 0, 0);
    dependentLayout->setSpacing(0);

    auto* directoryControl = new QWidget;
    auto* directoryLayout = new QHBoxLayout(directoryControl);
    directoryLayout->setContentsMargins(0, 0, 0, 0);
    directoryLayout->setSpacing(8);
    auto* directoryEdit = new QLineEdit(recordDirectory_);
    directoryEdit->setReadOnly(true);
    directoryEdit->setToolTip(recordDirectory_);
    directoryEdit->setFixedWidth(200);
    directoryEdit->setCursorPosition(0);
    auto* browse = new QPushButton(QStringLiteral("Browse"));
    browse->setIcon(themedIcon(browse, "document-open-folder", QStyle::SP_DirOpenIcon));
    directoryLayout->addWidget(directoryEdit);
    directoryLayout->addWidget(browse);
    QLabel* freeLabel = nullptr;
    dependentLayout->addWidget(settingRow(QStringLiteral("Output Directory"),
                                          QStringLiteral("Where recordings are saved."),
                                          directoryControl, &freeLabel));
    freeSpaceLabel_ = freeLabel;
    refreshFreeSpace();
    connect(browse, &QPushButton::clicked, this, [this, directoryEdit] {
        const QString selected = QFileDialog::getExistingDirectory(this, QStringLiteral("Select Output Directory"),
                                                                   recordDirectory_);
        if (selected.isEmpty()) return;
        mainWindow_->rememberDialogDirectory(selected, true);
        recordDirectory_ = QDir::toNativeSeparators(selected);
        directoryEdit->setText(recordDirectory_);
        directoryEdit->setToolTip(recordDirectory_);
        directoryEdit->setCursorPosition(0);
        refreshFreeSpace();
    });

    for (int i = 0; i < 4; ++i) {
        QButtonGroup* group = nullptr;
        QWidget* control = tnrui::segmented(group, {{kScopeLabels[0], 0}, {kScopeLabels[1], 1},
                                                    {kScopeLabels[2], 2}, {kScopeLabels[3], 3}});
        int current = 0;
        for (int v = 0; v < 4; ++v)
            if (scopes_[i] == QLatin1String(kScopeValues[v])) current = v;
        group->button(current)->setChecked(true);
        connect(group, &QButtonGroup::idClicked, this, [this, i](int id) {
            scopes_[i] = QString::fromLatin1(kScopeValues[id]);
        });
        dependentLayout->addWidget(settingRow(QString::fromLatin1(kScopeCategories[i].label), {}, control));
    }
    dependent->setEnabled(recordEnabled_);
    layout->addWidget(dependent);

    connect(toggle, &QCheckBox::toggled, this, [this, dependent](bool on) {
        recordEnabled_ = on;
        dependent->setEnabled(on);
    });
    return body;
}

// ── Density ─────────────────────────────────────────────────────────────────

QWidget* FirstTimeSetup::buildDensityStep() {
    auto* body = new QWidget;
    auto* layout = new QHBoxLayout(body);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(16);
    auto* group = new QButtonGroup(body);
    group->setExclusive(true);
    for (const DensityOption& option : kDensities) {
        auto* button = new DensityOptionButton(option, body);
        button->setChecked(option.mode == density_);
        group->addButton(button, static_cast<int>(option.mode));
        layout->addWidget(button, 1);
    }
    connect(group, &QButtonGroup::idClicked, this, [this](int id) {
        density_ = static_cast<tnr::DensityMode>(id);
    });
    return body;
}

// ── Connect the game ────────────────────────────────────────────────────────

QWidget* FirstTimeSetup::buildGameStep() {
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    layout->addWidget(groupLabel(QStringLiteral("This PC’s address")));
    auto* addresses = new PcAddressList(false);
    if (interfacesLoaded_) addresses->setInterfaces(interfaces_);
    addresses->setSelectedAddress(selectedAddress_);
    layout->addWidget(addresses);
    layout->addSpacing(24);

    // The game decides which UDP Format years it offers; Track N Race reads
    // 2024, 2025 and 2026, so recommend the newest each game has that we support.
    QButtonGroup* gameGroup = nullptr;
    QWidget* gameControl = tnrui::segmented(gameGroup, {{"F1 25 + 2026 Season Pack", 0}, {"F1 24", 1}});
    gameGroup->button(gameIsF1_24_ ? 1 : 0)->setChecked(true);
    connect(gameGroup, &QButtonGroup::idClicked, this, [this](int id) { gameIsF1_24_ = id == 1; showStep(Game); });
    layout->addWidget(settingRow(QStringLiteral("Your game"), QStringLiteral("Picks the UDP Format to use."), gameControl));
    layout->addSpacing(8);

    layout->addWidget(groupLabel(QStringLiteral("In the game ▸ Settings ▸ Telemetry Settings")));
    struct Setting { QString label; QString value; bool copyable; QString note; };
    const QVector<Setting> settings = {
        {QStringLiteral("UDP Telemetry"), QStringLiteral("On"), false, {}},
        {QStringLiteral("UDP Broadcast Mode"), QStringLiteral("Off"), false, {}},
        {QStringLiteral("UDP IP Address"), selectedAddress_.isEmpty() ? QString::fromUtf8("—") : selectedAddress_, true, {}},
        {QStringLiteral("UDP Port"), QString::number(port_), true, {}},
        {QStringLiteral("UDP Send Rate"), QStringLiteral("60 Hz"), false, {}},
        {QStringLiteral("UDP Format"), gameIsF1_24_ ? QStringLiteral("2024") : QStringLiteral("2026"), false,
         gameIsF1_24_ ? QStringLiteral("2022 and 2023 aren’t supported") : QStringLiteral("2024 and 2025 also work")},
    };
    auto* list = new QVBoxLayout;
    list->setSpacing(0);
    for (const Setting& setting : settings) {
        auto* row = new QWidget;
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 6, 0, 6);
        rowLayout->setSpacing(16);
        auto* label = new QLabel(setting.label);
        label->setForegroundRole(QPalette::PlaceholderText);
        rowLayout->addWidget(label, 1);
        auto* valueColumn = new QVBoxLayout;
        valueColumn->setSpacing(1);
        auto* value = new QLabel(setting.value);
        if (setting.label == QLatin1String("UDP IP Address")) ipValue_ = value;
        value->setFont(scaledFont(value->font(), 1.0, true));
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        if (setting.copyable) {
            QPalette palette = value->palette();
            palette.setColor(QPalette::WindowText, palette.color(QPalette::Highlight));
            value->setPalette(palette);
        }
        valueColumn->addWidget(value);
        if (!setting.note.isEmpty()) {
            auto* note = muted(setting.note, 0.85);
            note->setAlignment(Qt::AlignRight);
            valueColumn->addWidget(note);
        }
        rowLayout->addLayout(valueColumn);
        QWidget* copy = nullptr;
        if (setting.label == QLatin1String("UDP IP Address"))
            copy = copyButton([this] { return selectedAddress_; });
        else if (setting.copyable)
            copy = copyButton([value = setting.value] { return value; });
        else
            copy = new QWidget;
        copy->setFixedWidth(72);
        rowLayout->addWidget(copy);
        addSeparated(list, row);
    }
    layout->addLayout(list);
    layout->addSpacing(10);

    connect(addresses, &PcAddressList::addressSelected, this, [this](const QString& address) {
        selectedAddress_ = address;
        if (ipValue_) ipValue_->setText(address);
    });

    auto* advanced = linkButton(showAdvanced_ ? QStringLiteral("Hide advanced") : QStringLiteral("Advanced"));
    auto* advancedRow = new QHBoxLayout;
    advancedRow->addWidget(advanced);
    advancedRow->addStretch(1);
    layout->addLayout(advancedRow);
    auto* advancedBox = new QWidget;
    auto* advancedLayout = new QVBoxLayout(advancedBox);
    advancedLayout->setContentsMargins(0, 0, 0, 0);
    advancedLayout->setSpacing(0);
    auto* portSpin = new QSpinBox;
    portSpin->setRange(1, 65535);
    portSpin->setGroupSeparatorShown(false);
    portSpin->setValue(port_);
    portSpin->setFixedWidth(110);
    advancedLayout->addWidget(settingRow(QStringLiteral("UDP Port"), QStringLiteral("Must match the game."), portSpin));
    auto* bindEdit = new QLineEdit(bindAddress_);
    bindEdit->setPlaceholderText(QStringLiteral("0.0.0.0"));
    bindEdit->setFixedWidth(140);
    advancedLayout->addWidget(settingRow(QStringLiteral("Bind Address"),
                                         QStringLiteral("0.0.0.0 listens on every network."), bindEdit));
    advancedBox->setVisible(showAdvanced_);
    layout->addWidget(advancedBox);

    connect(advanced, &QPushButton::clicked, this, [this, advanced, advancedBox] {
        showAdvanced_ = !showAdvanced_;
        advancedBox->setVisible(showAdvanced_);
        advanced->setText(showAdvanced_ ? QStringLiteral("Hide advanced") : QStringLiteral("Advanced"));
    });
    connect(portSpin, &QSpinBox::editingFinished, this, [this, portSpin] {
        if (portSpin->value() == port_) return;
        port_ = portSpin->value();
        showStep(Game);   // the settings list shows the port
    });
    connect(bindEdit, &QLineEdit::textChanged, this, [this, bindEdit](const QString& text) {
        bindAddress_ = text;
        const bool valid = gameStepValid();
        bindEdit->setStyleSheet(valid ? QString() : QStringLiteral("border: 1px solid #c62828;"));
        continueButton_->setEnabled(valid);
    });
    if (!gameStepValid()) bindEdit->setStyleSheet(QStringLiteral("border: 1px solid #c62828;"));
    return body;
}

// ── Check connection ────────────────────────────────────────────────────────

QWidget* FirstTimeSetup::buildCheckStep() {
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* check = new ConnectionCheck(mainWindow_, body);
    if (interfacesLoaded_) check->setInterfaces(interfaces_);
    check->setSelectedAddress(selectedAddress_);
    check->setChangePortHandler([this] {
        showAdvanced_ = true;
        leaveStep(Game, Status::Done);
    });
    layout->addWidget(check);
    auto* hintLabel = muted(QStringLiteral("You can continue without a signal. The header shows the connection status later too."));
    hintLabel->setContentsMargins(0, 14, 0, 0);
    layout->addWidget(hintLabel);
    return body;
}

// ── Paired mode ─────────────────────────────────────────────────────────────

QWidget* FirstTimeSetup::buildPairingStep() {
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toggle = new QCheckBox;
    toggle->setChecked(mainWindow_->pairServiceState().enabled);
    layout->addWidget(settingRow(QStringLiteral("Enable Paired Mode"),
                                 QStringLiteral("Let Android displays find this PC on the local network."), toggle));
    connect(toggle, &QCheckBox::toggled, this, [this](bool on) { mainWindow_->setPairServiceEnabled(on); });

    auto* pairingBody = new QVBoxLayout;
    pairingBody->setSpacing(0);
    layout->addLayout(pairingBody);
    pairingBody_ = pairingBody;

    connect(mainWindow_, &MainWindow::pairServiceStateChanged, body, [this, toggle] {
        QSignalBlocker block(toggle);
        toggle->setChecked(mainWindow_->pairServiceState().enabled);
        refreshPairingBody();
    });
    refreshPairingBody();
    return body;
}

void FirstTimeSetup::refreshPairingBody() {
    if (!pairingBody_) return;
    clearLayout(pairingBody_);
    const PairServiceState& state = mainWindow_->pairServiceState();

    if (state.enabled && !state.pendingDeviceId.isEmpty()) {
        auto* heading = groupLabel(QStringLiteral("Pair a device"));
        heading->setContentsMargins(0, 14, 0, 4);
        pairingBody_->addWidget(heading);
        auto* row = new QWidget;
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 8, 0, 8);
        rowLayout->setSpacing(16);
        auto* text = new QVBoxLayout;
        auto* title = new QLabel(QStringLiteral("Allow %1?").arg(state.pendingDeviceName));
        title->setFont(scaledFont(title->font(), 1.0, true));
        text->addWidget(title);
        text->addWidget(muted(QStringLiteral("This phone entered the pairing code. Only allow a phone you recognise.")));
        rowLayout->addLayout(text, 1);
        auto* deny = new QPushButton(QStringLiteral("Deny"));
        auto* allow = new QPushButton(QStringLiteral("Allow"));
        connect(deny, &QPushButton::clicked, this, [this] { mainWindow_->respondToPairing(false); });
        connect(allow, &QPushButton::clicked, this, [this] { mainWindow_->respondToPairing(true); });
        rowLayout->addWidget(deny);
        rowLayout->addWidget(allow);
        pairingBody_->addWidget(row);
    } else if (state.enabled && state.pairingOpen) {
        auto* heading = groupLabel(QStringLiteral("Pair a device"));
        heading->setContentsMargins(0, 14, 0, 4);
        pairingBody_->addWidget(heading);
        auto* row = new QWidget;
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 6, 0, 0);
        rowLayout->setSpacing(28);
        auto* qr = new QLabel;
        qr->setFixedSize(132, 132);
        qr->setAlignment(Qt::AlignCenter);
        qr->setStyleSheet(QStringLiteral("background: white; border-radius: 4px; padding: 6px;"));
        const QImage image = state.qrPayload.isEmpty() ? QImage() : pairingQrCodeImage(state.qrPayload);
        if (!image.isNull())
            qr->setPixmap(QPixmap::fromImage(image).scaled(120, 120, Qt::KeepAspectRatio, Qt::FastTransformation));
        rowLayout->addWidget(qr, 0, Qt::AlignTop);

        auto* column = new QVBoxLayout;
        column->setSpacing(2);
        column->addWidget(muted(QStringLiteral("Matching code"), 0.85));
        auto* code = new QLabel(state.matchingCode.size() == 8
            ? state.matchingCode.left(4) + QLatin1Char(' ') + state.matchingCode.mid(4) : state.matchingCode);
        QFont codeFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        codeFont.setPointSize(24);
        codeFont.setBold(true);
        codeFont.setLetterSpacing(QFont::AbsoluteSpacing, 4);
        code->setFont(codeFont);
        code->setTextInteractionFlags(Qt::TextSelectableByMouse);
        column->addWidget(code);
        auto* instructions = muted(QStringLiteral("Scan the QR in Android Settings, or pick this PC on the phone and enter the code."));
        instructions->setContentsMargins(0, 8, 0, 12);
        column->addWidget(instructions);

        auto* countdown = new QProgressBar;
        countdown->setTextVisible(false);
        countdown->setFixedHeight(3);
        countdown->setRange(0, static_cast<int>(kPairWindowMs / 1000));
        auto* remainingLabel = muted(QString(), 0.85);
        const qint64 expiresAt = state.pairingExpiresAt;
        auto tick = [countdown, remainingLabel, expiresAt] {
            const qint64 remaining = qMax<qint64>(0, expiresAt - QDateTime::currentMSecsSinceEpoch());
            const int seconds = static_cast<int>((remaining + 999) / 1000);
            countdown->setValue(qMin(seconds, countdown->maximum()));
            remainingLabel->setText(QStringLiteral("Open for %1:%2").arg(seconds / 60)
                                        .arg(seconds % 60, 2, 10, QLatin1Char('0')));
        };
        tick();
        auto* timer = new QTimer(countdown);
        connect(timer, &QTimer::timeout, countdown, tick);
        timer->start(1000);
        column->addWidget(countdown);
        column->addWidget(remainingLabel);
        auto* cancel = linkButton(QStringLiteral("Cancel pairing"));
        connect(cancel, &QPushButton::clicked, this, [this] { mainWindow_->closePairingWindow(); });
        column->addWidget(cancel, 0, Qt::AlignRight);
        rowLayout->addLayout(column, 1);
        pairingBody_->addWidget(row);
    } else if (state.enabled) {
        auto* heading = groupLabel(QStringLiteral("Devices"));
        heading->setContentsMargins(0, 14, 0, 4);
        pairingBody_->addWidget(heading);
        auto* row = new QWidget;
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 6, 0, 6);
        rowLayout->setSpacing(16);
        QStringList names;
        for (const PairDeviceState& device : state.devices)
            names << (device.name.isEmpty() ? QStringLiteral("Android display") : device.name);
        QLabel* devices = names.isEmpty() ? muted(QStringLiteral("No devices paired yet."))
                                          : new QLabel(names.join(QStringLiteral(", ")));
        devices->setWordWrap(false);
        rowLayout->addWidget(devices, 1);
        auto* pair = new QPushButton(QStringLiteral("Pair a device"));
        pair->setIcon(themedIcon(pair, "smartphone", QStyle::SP_ComputerIcon));
        connect(pair, &QPushButton::clicked, this, [this] { mainWindow_->openPairingWindow(); });
        rowLayout->addWidget(pair);
        pairingBody_->addWidget(row);
        auto* hintLabel = muted(QStringLiteral("Opens discovery, QR, and matching-code pairing for two minutes."));
        hintLabel->setContentsMargins(0, 6, 0, 0);
        pairingBody_->addWidget(hintLabel);
    } else {
        auto* hintLabel = muted(QStringLiteral("Optional. You can pair later in Settings ▸ Paired Devices."));
        hintLabel->setContentsMargins(0, 4, 0, 0);
        pairingBody_->addWidget(hintLabel);
    }

    if (!state.error.isEmpty()) {
        auto* error = new QLabel(state.error);
        error->setWordWrap(true);
        error->setStyleSheet(QStringLiteral("color: #f87171;"));
        error->setContentsMargins(0, 12, 0, 0);
        pairingBody_->addWidget(error);
    }
}

// ── Summary ─────────────────────────────────────────────────────────────────

QWidget* FirstTimeSetup::buildSummaryStep() {
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    const bool recordingsSkipped = statuses_[Recordings] == Status::Skipped;
    const bool densitySkipped = statuses_[Density] == Status::Skipped;
    const bool gameSkipped = statuses_[Game] == Status::Skipped;

    struct Row { Step step; QString label; QString value; QString detail; };
    QVector<Row> rows;
    if (recordingsSkipped) {
        const bool saved = mainWindow_->autoRecordEnabled();
        rows.push_back({Recordings, QStringLiteral("Recordings"), saved ? QStringLiteral("On") : QStringLiteral("Off"),
                        saved ? QDir::toNativeSeparators(mainWindow_->currentOutputDirectory()) : QString()});
    } else {
        rows.push_back({Recordings, QStringLiteral("Recordings"), recordEnabled_ ? QStringLiteral("On") : QStringLiteral("Off"),
                        recordEnabled_ ? recordDirectory_ : QStringLiteral("Sessions won’t be saved")});
    }
    rows.push_back({Density, QStringLiteral("Density"),
                    densityLabel(densitySkipped ? mainWindow_->uniformDensity() : std::optional(density_)),
                    densitySkipped ? QString() : QStringLiteral("Every section")});
    rows.push_back({Game, QStringLiteral("Game address"),
                    QStringLiteral("%1 : %2").arg(selectedAddress_.isEmpty() ? QString::fromUtf8("—") : selectedAddress_)
                        .arg(gameSkipped ? mainWindow_->udpPort() : port_), {}});
    rows.push_back({Pairing, QStringLiteral("Paired mode"),
                    mainWindow_->pairServiceState().enabled ? QStringLiteral("On") : QStringLiteral("Off"), {}});

    for (int position = 0; position < rows.size(); ++position) {
        const Row& row = rows[position];
        const bool skipped = statuses_[row.step] == Status::Skipped;
        auto* line = new QWidget;
        auto* lineLayout = new QHBoxLayout(line);
        lineLayout->setContentsMargins(0, 10, 0, 10);
        lineLayout->setSpacing(14);
        auto* number = muted(QStringLiteral("%1").arg(position + 1, 2, 10, QLatin1Char('0')), 0.85);
        number->setWordWrap(false);
        number->setFixedWidth(26);
        lineLayout->addWidget(number);
        auto* label = new QLabel(row.label);
        label->setForegroundRole(QPalette::PlaceholderText);
        label->setFixedWidth(150);
        lineLayout->addWidget(label);
        auto* value = new QLabel;
        value->setTextFormat(Qt::RichText);
        value->setToolTip(row.detail);
        if (skipped) {
            value->setText(QStringLiteral("Skipped · kept %1").arg(row.value.toHtmlEscaped()));
            value->setForegroundRole(QPalette::PlaceholderText);
        } else {
            QString html = QStringLiteral("<b>%1</b>").arg(row.value.toHtmlEscaped());
            if (!row.detail.isEmpty()) {
                const QString mutedColor = palette().color(QPalette::PlaceholderText).name();
                html += QStringLiteral("&nbsp;&nbsp;<span style='color:%1'><small>%2</small></span>")
                            .arg(mutedColor, row.detail.toHtmlEscaped());
            }
            value->setText(html);
        }
        value->setMinimumWidth(0);
        value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        lineLayout->addWidget(value, 1);
        auto* change = linkButton(QStringLiteral("Change"));
        const Step target = row.step;
        connect(change, &QPushButton::clicked, this, [this, target] { showStep(target); });
        lineLayout->addWidget(change);
        addSeparated(layout, line);
    }
    return body;
}
