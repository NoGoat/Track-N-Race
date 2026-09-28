#pragma once

#include <QDialog>

#include <tnrp/control_rows.h>

#include <functional>

class QLabel;
class QTableWidget;

// One driver's completed lap times, opened by double-clicking a Standings row.
// The owner subscribes the engine to this car while the dialog is open; the
// engine pushes driver_lap_history rows when they change and the owner hands
// them to apply(). The dialog never asks for data itself.
class DriverLapsDialog : public QDialog {
    Q_OBJECT

public:
    // Whether this driver holds the session's fastest lap, as Standings shows
    // it. Called on the GUI thread.
    using HoldsFastestLap = std::function<bool()>;

    DriverLapsDialog(const QString& driverName, int carIdx,
                     HoldsFastestLap holdsFastestLap, QWidget* parent = nullptr);

    int carIdx() const { return carIdx_; }
    // A pushed row; rows for any other car are ignored.
    void apply(const tnrp::DriverLapHistoryRow& history);

private:
    int carIdx_;
    HoldsFastestLap holdsFastestLap_;
    QTableWidget* table_ = nullptr;
    QLabel* empty_ = nullptr;
    QWidget* loading_ = nullptr;
};
