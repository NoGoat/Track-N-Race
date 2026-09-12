#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>

#include <cstdint>
#include <memory>

class QComboBox;
class QFrame;
class QLabel;
class QPushButton;
class QSlider;
class QWidget;
class SessionModel;
class TnrdPlayer;
struct EngineSeekFlush;
namespace tnrp { class Engine; struct HeaderRow; }

// Owns the .tnrd playback controls: the engine-backed TnrdPlayer facade and the bottom transport
// bar (seek buttons, play/pause, scrub slider, time label, lap/speed combos,
// close button). Handles the bar↔player sync internally — slider/label/lap
// tracking, scrub throttling, model load/clear on open/close — and raises the
// signals below for the app-level reactions (playback-mode fan-out, engine
// logging, window title, loading overlay).
class PlaybackController : public QObject {
    Q_OBJECT

public:
    PlaybackController(SessionModel* model, tnrp::Engine* engine, QWidget* barParent);
    ~PlaybackController() override;

    QWidget* bar() const { return bar_; }         // insert into the central vbox
    QFrame*  separator() const { return sep_; }   // hairline above the bar

    void load(const QString& path);               // open a recording asynchronously
    float currentTime() const;                    // absolute session_time playhead
    bool handleControlRow(const QByteArray& json);
    void handleSeekFlush(const std::shared_ptr<EngineSeekFlush>& flush);
    void setDataRequirements(uint32_t streamMask, uint32_t historyMask,
                             float windowSeconds);
    void requestLapData(int lapNum, uint32_t rowTypeMask);
    void setEngine(tnrp::Engine* engine);
    void quiesce();
    void shutdown();

    // Path of the currently-loaded .tnrd (empty when none). Used by the Export-to-
    // Excel action, whose exporter opens its own reader on this file.
    QString loadedPath() const { return loadedPath_; }

    // Follows the toolbar's "Show button labels" option: icon-only vs a labelled
    // "Close File" button (kept in sync from MainWindow::setToolbarLabels).
    void setShowLabels(bool on);

signals:
    void loadingStarted();                         // show the loading overlay
    void loadFailed(const QString& reason);        // hide overlay + warn
    // Recording opened: model loaded, bar shown. `header` is the .tnrd header
    // (protocol/track_name/session_name), `currentTime` the initial playhead.
    void entered(const tnrp::HeaderRow& header, float currentTime);
    void exited();                                 // recording closed, bar hidden
    void seeked();                                 // user seek (suppress SC toast once)
    void seekStarted(uint64_t requestId);          // freeze old-timeline presentation
    void historyInstalled(uint64_t requestId);     // authoritative model swap completed
    void lapCatalogInstalled();                    // lap-relative requests can now be resolved
    void activeLapChanged(int lapNum);             // refresh Current/Previous lap data
    void timeChanged(float absoluteTime);          // per playback tick / scrub
    void exportRequested();                        // Export-to-Excel button clicked

private:
    SessionModel* model_  = nullptr;
    TnrdPlayer*   player_ = nullptr;

    QString loadedPath_;               // the .tnrd currently open (for the export action)

    bool    seekerUpdating_ = false;
    qint64  lastSeekMs_     = 0;       // leading-edge throttle for scrub-bar seeks
    bool    lastPlaying_    = false;   // only swap the play/pause icon on change
    qint64  lastTransportUiMs_ = 0;    // progress cosmetics are capped to 10 Hz
    int     lastActiveLapNum_ = -1;    // request lap-relative data only on transitions

    QWidget*     bar_         = nullptr;
    QFrame*      sep_         = nullptr;
    QPushButton* seekBackBtn_ = nullptr;
    QPushButton* playBtn_     = nullptr;
    QPushButton* seekFwdBtn_  = nullptr;
    QSlider*     slider_      = nullptr;
    QLabel*      timeLabel_   = nullptr;
    QComboBox*   speedCombo_  = nullptr;
    QComboBox*   lapCombo_    = nullptr;
    QPushButton* exportBtn_   = nullptr;
    QPushButton* closeRecBtn_ = nullptr;
};
