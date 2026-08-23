#pragma once

#include <QDialog>
#include "SessionLayout.h"

class SessionPage;
class QPushButton;

class EditSessionLayoutDialog : public QDialog {
    Q_OBJECT

public:
    explicit EditSessionLayoutDialog(SessionPage* page, QWidget* parent = nullptr);

private:
    void toggleGpName(bool on);
    void toggleMarshalZones(bool on);
    void toggleTimeLeft(bool on);
    void toggleCard(int idx, bool on);
    void toggleMap(bool on);
    void toggleProximity(bool on);
    void toggleEvents(bool on);
    void toggleWeather(bool on);

    SessionPage*  page_;
    SessionLayout layout_;
    QPushButton*  gpNameBtn_ = nullptr;
    QPushButton*  zonesBtn_ = nullptr;
    QPushButton*  timeLeftBtn_ = nullptr;
    QPushButton*  cardBtns_[SessionLayout::StatCardCount] = {};
    QPushButton*  mapBtn_ = nullptr;
    QPushButton*  proxBtn_ = nullptr;
    QPushButton*  eventsBtn_ = nullptr;
    QPushButton*  weatherBtn_ = nullptr;
};
