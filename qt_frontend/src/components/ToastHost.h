#pragma once

#include <QObject>
#include <QSettings>

class QWidget;
class Toast;
struct ToastSpec;

// Event toast notifications (vendored qt-toast). show() builds and shows one
// transient popup, themed + gated by the toast settings; a single persistent
// slot (SC/VSC/FL) is tracked so a newer persistent/ending event evicts it.
// The safety-car *decision* logic stays with the row routing in MainWindow —
// this only owns presentation.
class ToastHost : public QObject {
    Q_OBJECT

public:
    // `container` is the central content widget toasts render inside (a child
    // overlay, not a window — required for correct positioning on Wayland).
    explicit ToastHost(QWidget* container);

    void show(const ToastSpec& spec);
    void dismissPersistent();   // SC returned to 0 with no ending event

private:
    QWidget* container_   = nullptr;
    Toast*   persistent_  = nullptr;   // single persistent slot; null when empty

    QSettings settings_{ "TrackNRace", "NativeRecorder" };
};
