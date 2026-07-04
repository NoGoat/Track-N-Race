#pragma once

#include <QWidget>

#include <nlohmann/json.hpp>

class QTableWidget;
class TyreCardsWidget;

// Tyres tab — dry/wet tyre-set tables plus a vertical strip of per-corner
// tyre cards. Self-contained: MainWindow feeds it the latest cached rows via
// updateTyreCards()/updateTyreSets() from the coalesced refresh.
class TyresPage : public QWidget {
    Q_OBJECT

public:
    explicit TyresPage(QWidget* parent = nullptr);

    // Refresh the per-corner cards from the latest telemetry + damage rows.
    void updateTyreCards(const nlohmann::json& telemetry, const nlohmann::json& damage);

    // Rebuild the dry/wet set tables from the latest tyre_sets row.
    void updateTyreSets(const nlohmann::json& tyreSets);

private:
    TyreCardsWidget* tyreCards_    = nullptr;
    QTableWidget*    drySetsTable_ = nullptr;
    QTableWidget*    wetSetsTable_ = nullptr;
};
