#pragma once

#include <QObject>

class QNetworkAccessManager;
class QWidget;

// Startup-only GitHub release discovery. It deliberately offers discovery and
// an external download link only; installation remains outside the application.
class UpdateChecker final : public QObject {
    Q_OBJECT
public:
    explicit UpdateChecker(QWidget* dialogParent);
    void checkOnStartup();

private:
    QWidget* dialogParent_ = nullptr;
    QNetworkAccessManager* network_ = nullptr;
};
