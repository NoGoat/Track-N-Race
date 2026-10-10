// Website screenshots: F7 single capture, Shift+F7 window sizing, F8 tour.
//
// The tour reproduces the Electron frontend's website screenshots from the
// same recording: each shot selects a page, sets up its state, seeks to the
// session time read off the Electron screenshot, waits for the seek's history
// to be installed and the page to settle, then captures the window.

#include "MainWindow.h"

#include "AppToolbar.h"
#include "PlaybackController.h"
#include "components/ChartView.h"
#include "components/EditOverviewLayoutDialog.h"
#include "components/DriverLapsDialog.h"
#include "components/StandingsPage.h"
#include "components/Toast.h"
#include "components/TyresPage.h"
#include "components/analysis/AnalysisPage.h"

#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPainter>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>

#include <functional>
#include <limits>
#include <memory>
#include <utility>

namespace {

constexpr qreal kCaptureScale = 2.0;   // the Electron screenshots are macOS 2x captures
const QSize kWindowSize(1200, 700);    // their whole window, title bar included

// Session times shown in the Electron screenshots' transport (m:ss.mmm).
constexpr float t(int minutes, double seconds) { return float(minutes * 60 + seconds); }

}  // namespace

// ── Single capture ──────────────────────────────────────────────────────────

QPixmap MainWindow::grabForScreenshot() {
    // Toasts are drawn inside the window; a stale one would land in the shot.
    for (Toast* toast : findChildren<Toast*>()) toast->hide();

    // X11: read the frame from the screen, so KWin's title bar is included
    // and dialogs open over the window appear as they do on screen. The
    // pixels are the display's own; at 200% scaling the frame is 2x.
    if (QGuiApplication::platformName() == QLatin1String("xcb")) {
        if (QScreen* scr = screen()) {
            const QRect frame = frameGeometry().translated(-scr->geometry().topLeft());
            QPixmap shot = scr->grabWindow(0, frame.x(), frame.y(), frame.width(), frame.height());
            if (!shot.isNull()) return shot;
        }
    }

    // Elsewhere (Wayland cannot read the screen): the window renders itself
    // at 2x, GPU charts included. No title bar; open dialogs are drawn over
    // it where they sit.
    ChartView::setCaptureScale(kCaptureScale);
    QPixmap shot((QSizeF(size()) * kCaptureScale).toSize());
    shot.setDevicePixelRatio(kCaptureScale);
    shot.fill(Qt::transparent);
    render(&shot);
    {
        QPainter painter(&shot);
        for (QDialog* dialog : findChildren<QDialog*>()) {
            if (!dialog->isVisible() || !dialog->isWindow()) continue;
            dialog->render(&painter, dialog->geometry().topLeft() - geometry().topLeft());
        }
    }
    ChartView::setCaptureScale(0);
    return shot;
}

void MainWindow::showScreenshotNotice(const QString& text, int msecs) {
    // Reported in the title bar: a toast would land inside the next capture.
    // Repeated notices keep the original title to restore, and a title the
    // app set meanwhile (session/playback change) is left alone.
    if (screenshotNotice_.isEmpty() || windowTitle() != screenshotNotice_)
        titleBeforeScreenshot_ = windowTitle();
    screenshotNotice_ = text;
    setWindowTitle(text);
    if (msecs <= 0) return;
    QTimer::singleShot(msecs, this, [this, notice = text] {
        if (screenshotNotice_ != notice) return;   // a newer notice owns the title
        if (windowTitle() == notice) setWindowTitle(titleBeforeScreenshot_);
        screenshotNotice_.clear();
    });
}

void MainWindow::captureScreenshot() {
    if (screenshotTour_) return;   // the tour owns the window while it runs
    const QPixmap shot = grabForScreenshot();
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
                        + "/Track N Race Screenshots";
    QDir().mkpath(dir);
    const QString name = "tnr-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz") + ".png";
    const bool ok = shot.save(dir + "/" + name);
    qInfo().noquote() << "[screenshot]" << (ok ? "saved" : "FAILED to save") << dir + "/" + name
                      << QString("%1x%2").arg(shot.width()).arg(shot.height());
    showScreenshotNotice(ok ? QString("Screenshot: saved %1 (%2x%3)").arg(name).arg(shot.width()).arg(shot.height())
                            : QString("Screenshot: could not write to %1").arg(dir));
}

void MainWindow::sizeForScreenshot() {
    // 1200x700 logical (2400x1400 at 2x) is the whole window in every Electron
    // screenshot, its title bar included (macOS draws it inside the window).
    // Size the frame, not the contents, to match: the contents shrink by the
    // decoration, so the minimum size is relaxed for the rest of this run.
    showNormal();
    const QSize decoration = frameGeometry().size() - geometry().size();
    setMinimumSize(kWindowSize - decoration);
    resize(kWindowSize - decoration);
}

// ── Tour ────────────────────────────────────────────────────────────────────

class ScreenshotTour : public QObject {
public:
    struct Shot {
        QString file;                         // name of the matching Electron screenshot
        MainWindow::Page page;
        float time = -1;                      // transport time to seek to; < 0 = no seek
        std::function<QString()> before;      // set up state; returns an error to skip
        std::function<void()> beforeCapture;  // after settling (dialogs, cursors)
        std::function<void()> after;          // undo the state
        int settleMs = 1500;
    };

    ScreenshotTour(MainWindow* w, const QString& recording, const QString& outDir)
        : QObject(w), w_(w), recording_(recording), outDir_(outDir) {
        connect(w_->playback_, &PlaybackController::seekStarted, this,
                [this](uint64_t id) { lastSeekStarted_ = id; });
        connect(w_->playback_, &PlaybackController::historyInstalled, this, [this](uint64_t id) {
            if (waitingForSeek_ && id >= awaitedSeek_) seekDone();
        });
        timer_.setSingleShot(true);
        buildShots();
    }

    void start() {
        QDir().mkpath(outDir_);
        w_->sizeForScreenshot();
        w_->raise();
        w_->activateWindow();
        // Keep the pointer off the window: hover cursors and tooltips would be captured.
        QCursor::setPos(w_->frameGeometry().topRight() + QPoint(80, 0));

        if (QFileInfo(w_->playback_->loadedPath()) == QFileInfo(recording_)) {
            step("using the open recording", 500, [this] { runShot(0); });
            return;
        }
        status(QStringLiteral("opening %1").arg(QFileInfo(recording_).fileName()));
        entered_ = connect(w_->playback_, &PlaybackController::entered, this, [this] {
            disconnect(entered_);
            w_->playback_->pause();
            // The lap catalog and participants arrive just after the header.
            step("recording opened", 4000, [this] { runShot(0); });
        });
        failed_ = connect(w_->playback_, &PlaybackController::loadFailed, this,
                          [this](const QString& reason) { finish("could not open the recording: " + reason); });
        w_->playback_->load(recording_);
    }

    void cancel() { finish(QStringLiteral("cancelled")); }

private:
    MainWindow* w_;
    QString recording_;
    QString outDir_;
    QVector<Shot> shots_;
    int saved_ = 0;
    QStringList skipped_;
    QTimer timer_;
    QMetaObject::Connection entered_, failed_, timerConn_;
    uint64_t lastSeekStarted_ = 0;
    uint64_t awaitedSeek_ = 0;
    bool waitingForSeek_ = false;
    std::function<void()> afterSeek_;

    void status(const QString& text) {
        qInfo().noquote() << "[screenshot-tour]" << text;
        w_->showScreenshotNotice("Screenshot tour: " + text, 0);
    }

    // Runs `next` after `ms`, reporting `what`.
    void step(const QString& what, int ms, std::function<void()> next) {
        status(what);
        disconnect(timerConn_);
        timerConn_ = connect(&timer_, &QTimer::timeout, this, std::move(next));
        timer_.start(ms);
    }

    void seekThen(float seconds, std::function<void()> next) {
        afterSeek_ = std::move(next);
        waitingForSeek_ = true;
        // Nothing matches until this seek's id is known; seekStarted is
        // emitted synchronously by the seek call below.
        awaitedSeek_ = std::numeric_limits<uint64_t>::max();
        w_->playback_->pause();
        w_->playback_->seekToDisplayTime(seconds);
        awaitedSeek_ = lastSeekStarted_;
        // A seek whose history never arrives must not stall the tour.
        step(QStringLiteral("seeking to %1").arg(seconds, 0, 'f', 3), 20000, [this] {
            qWarning("[screenshot-tour] seek did not report completion; capturing anyway");
            seekDone();
        });
    }

    void seekDone() {
        waitingForSeek_ = false;
        timer_.stop();
        if (auto next = std::exchange(afterSeek_, nullptr)) next();
    }

    void runShot(int index) {
        if (index >= shots_.size()) {
            finish(QStringLiteral("%1 saved to %2%3").arg(saved_).arg(outDir_,
                   skipped_.isEmpty() ? QString() : "; skipped " + skipped_.join(", ")));
            return;
        }
        const Shot& shot = shots_[index];
        w_->toolbar_->selectPage(shot.page);
        if (shot.before) {
            const QString error = shot.before();
            if (!error.isEmpty()) {
                qWarning().noquote() << "[screenshot-tour] skipping" << shot.file << "-" << error;
                skipped_ << shot.file;
                if (shot.after) shot.after();
                runShot(index + 1);
                return;
            }
        }
        auto settle = [this, index] {
            const Shot& s = shots_[index];
            step(QStringLiteral("%1 (%2/%3)").arg(s.file).arg(index + 1).arg(shots_.size()),
                 s.settleMs, [this, index] {
                const Shot& s = shots_[index];
                if (!s.beforeCapture) { capture(index); return; }
                s.beforeCapture();
                step(s.file, 800, [this, index] { capture(index); });
            });
        };
        if (shot.time >= 0) seekThen(shot.time, settle);
        else settle();
    }

    void capture(int index) {
        const Shot& shot = shots_[index];
        const QPixmap pixmap = w_->grabForScreenshot();
        const QString path = QDir(outDir_).filePath(shot.file);
        if (pixmap.save(path)) {
            ++saved_;
            qInfo().noquote() << "[screenshot-tour] saved" << path
                              << QString("%1x%2").arg(pixmap.width()).arg(pixmap.height())
                              << "at" << w_->playback_->displayTime();
            if (pixmap.size() != kWindowSize * kCaptureScale)
                qWarning().noquote() << "[screenshot-tour]" << shot.file << "is not 2400x1400";
        } else {
            qWarning().noquote() << "[screenshot-tour] could not write" << path;
            skipped_ << shot.file;
        }
        if (shot.after) shot.after();
        runShot(index + 1);
    }

    void finish(const QString& result) {
        timer_.stop();
        disconnect(entered_);
        disconnect(failed_);
        waitingForSeek_ = false;
        qInfo().noquote() << "[screenshot-tour]" << result;
        w_->showScreenshotNotice("Screenshot tour: " + result, 8000);
        deleteLater();
    }

    // ── Helpers for the shot list ──
    int carIndex(const QString& name) const {
        if (!w_->lastParticipantsData) return -1;
        for (const auto& driver : w_->lastParticipantsData->drivers)
            if (QString::fromStdString(driver.name).compare(name, Qt::CaseInsensitive) == 0)
                return driver.idx;
        return -1;
    }
    QString selectDriver(const QString& name) {
        const int idx = carIndex(name);
        if (idx < 0) return name + " is not in the recording";
        if (w_->playback_->tnrdVersion() != QStringLiteral("TNRD_V6"))
            return QStringLiteral("switching drivers needs a TNRD V6 recording");
        emit w_->toolbar_->playbackDriverChanged(idx);
        return {};
    }
    void restoreDriver() {
        const int original = w_->playback_->originalPlaybackDriverIndex();
        if (original >= 0) emit w_->toolbar_->playbackDriverChanged(original);
    }

    void buildShots() {
        using P = MainWindow::Page;
        const float common = t(31, 45.225);   // most Electron shots

        shots_ << Shot{"overview.png", P::Overview, t(31, 44.009)};

        shots_ << Shot{"other_drivers.png", P::Overview, t(31, 45.225),
                       [this] { return selectDriver("ALBON"); }, {},
                       [this] { restoreDriver(); }, 3000};

        auto tableWas = std::make_shared<bool>(false);
        shots_ << Shot{"overview_table.png", P::Overview, t(34, 5.532),
                       [this, tableWas] {
                           *tableWas = w_->graphView(tnr::GraphSection::OverviewTelemetry);
                           w_->dispatchGraphView(tnr::GraphSection::OverviewTelemetry, true);
                           return QString();
                       }, {},
                       [this, tableWas] {
                           w_->dispatchGraphView(tnr::GraphSection::OverviewTelemetry, *tableWas);
                       }};

        shots_ << Shot{"editor.png", P::Overview, common, {},
                       [this] { emit w_->toolbar_->editLayoutRequested(); },
                       [this] {
                           for (auto* dialog : w_->findChildren<EditOverviewLayoutDialog*>())
                               dialog->close();
                       }};

        shots_ << Shot{"session.png", P::Session, t(31, 44.090)};
        shots_ << Shot{"standings.png", P::Standings, common};

        shots_ << Shot{"previous_lap_times.png", P::Standings, common, {},
                       [this] {
                           const int idx = carIndex("ANTONELLI");
                           if (idx >= 0) emit w_->standingsPage_->driverLapsRequested(idx);
                       },
                       [this] { if (w_->lapsDialog_) w_->lapsDialog_->close(); }};

        auto graphsWere = std::make_shared<bool>(false);
        shots_ << Shot{"tyres.png", P::Tyres, common,
                       [this, graphsWere] {
                           *graphsWere = w_->tyresPage_->graphsShown();
                           w_->tyresPage_->setGraphsShown(false);
                           return QString();
                       }};
        shots_ << Shot{"tyres_charts.png", P::Tyres, common,
                       [this] { w_->tyresPage_->setGraphsShown(true); return QString(); }, {},
                       [this, graphsWere] { w_->tyresPage_->setGraphsShown(*graphsWere); }};

        shots_ << Shot{"strategy.png", P::Strategy, common};
        shots_ << Shot{"inputs.png", P::Input, common};
        shots_ << Shot{"power.png", P::Power, common};
        shots_ << Shot{"misc.png", P::Misc, common};
        shots_ << Shot{"damage.png", P::Damage, common};
        shots_ << Shot{"trends.png", P::Trends, common};

        // Electron: Antonelli lap 4 vs Leclerc lap 3, Delta/Throttle/Speed/ERS/
        // Brake, split view, the map cursor at 0:54.500 into the lap.
        shots_ << Shot{"analyze.png", P::Analyze, -1,
                       [this] {
                           return w_->analyzePage_->stageComparison(
                               "ANTONELLI", 4, "LECLERC", 3,
                               {"delta", "throttle", "speed", "ers", "brake"});
                       },
                       [this] { w_->analyzePage_->focusMapElapsed(54.5); },
                       {}, 5000};

        // Leave the window on the Overview page.
        shots_.last().after = [this] { w_->toolbar_->selectPage(MainWindow::Page::Overview); };
    }
};

void MainWindow::toggleScreenshotTour() {
    if (screenshotTour_) {
        screenshotTour_->cancel();
        return;
    }
    // Next to the AppImage when run as one, otherwise next to the executable.
    const QByteArray appImage = qgetenv("APPIMAGE");
    const QDir dir = appImage.isEmpty() ? QDir(QCoreApplication::applicationDirPath())
                                        : QFileInfo(QString::fromLocal8Bit(appImage)).dir();
    const QString recording = dir.filePath("test.tnrd");
    if (!QFileInfo::exists(recording)) {
        showScreenshotNotice("Screenshot tour: no test.tnrd in " + dir.absolutePath(), 8000);
        return;
    }
    screenshotTour_ = new ScreenshotTour(this, recording, dir.filePath("screenshots"));
    screenshotTour_->start();
}
