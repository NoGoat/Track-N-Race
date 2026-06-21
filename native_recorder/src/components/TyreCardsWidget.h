#pragma once

#include <QWidget>
#include <QLabel>
#include <QProgressBar>

#include <nlohmann/json.hpp>

class TyreCardsWidget : public QWidget {
    Q_OBJECT
public:
    // Horizontal: 4 cards side by side (Overview page)
    // Vertical:   4 cards stacked in a column (Tyres page)
    explicit TyreCardsWidget(Qt::Orientation orientation = Qt::Horizontal,
                             QWidget* parent = nullptr);

    void update(const nlohmann::json& telemetry, const nlohmann::json& damage);

private:
    QLabel*       surfaceTemp_[4] = {};
    QLabel*       innerTemp_[4]   = {};
    QLabel*       brakeTemp_[4]   = {};
    QLabel*       wearLabel_[4]   = {};
    QProgressBar* wear_[4]        = {};
    QLabel*       blisters_[4]    = {};
};
