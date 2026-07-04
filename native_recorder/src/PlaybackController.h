#pragma once

#include <QObject>

#include <nlohmann/json.hpp>

class QComboBox;
class QFrame;
class QLabel;
class QPushButton;
class QSlider;
class QWidget;
class SessionModel;
class TnrdPlayer;

// Owns the .tnrd playback machinery: the TnrdPlayer and the bottom transport
// bar (seek buttons, play/pause, scrub slider, time label, lap/speed combos,
// close button). Handles the bar↔player sync internally — slider/label/lap
// tracking, scrub throttling, model load/clear on open/close — and raises the
// signals below for the app-level reactions (playback-mode fan-out, engine
// logging, window title, loading overlay).
class PlaybackController : public QObject {
    Q_OBJECT

public:
    PlaybackController(SessionModel* model, QWidget* barParent);

    QWidget* bar() const { return bar_; }         // insert into the central vbox
    QFrame*  separator() const { return sep_; }   // hairline above the bar

    void load(const QString& path);               // open a recording (async scan)
    float currentTime() const;                    // absolute session_time playhead

signals:
    void loadingStarted();                         // show the loading overlay
    void loadFailed();                             // hide overlay + warn
    // Recording opened: model loaded, bar shown. `header` is the .tnrd header
    // (protocol/track_name/session_name), `currentTime` the initial playhead.
    void entered(const nlohmann::json& header, float currentTime);
    void exited();                                 // recording closed, bar hidden
    void rowReady(const nlohmann::json& row);      // replayed packet (→ emitLiveData)
    void seeked();                                 // user seek (suppress SC toast once)
    void timeChanged(float absoluteTime);          // per playback tick / scrub

private:
    SessionModel* model_  = nullptr;
    TnrdPlayer*   player_ = nullptr;

    bool    seekerUpdating_ = false;
    qint64  lastSeekMs_     = 0;       // leading-edge throttle for scrub-bar seeks
    bool    lastPlaying_    = false;   // only swap the play/pause icon on change

    QWidget*     bar_         = nullptr;
    QFrame*      sep_         = nullptr;
    QPushButton* seekBackBtn_ = nullptr;
    QPushButton* playBtn_     = nullptr;
    QPushButton* seekFwdBtn_  = nullptr;
    QSlider*     slider_      = nullptr;
    QLabel*      timeLabel_   = nullptr;
    QComboBox*   speedCombo_  = nullptr;
    QComboBox*   lapCombo_    = nullptr;
};
