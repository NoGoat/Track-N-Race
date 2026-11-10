#pragma once

#include <QDialog>

#include <memory>

namespace tnrp { struct TrackMapData; }

class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSlider;
class TrackMapWidget;

// Edit Map: a map's names and rotation, with a live preview. A built-in map's
// edit is saved as a copy for its track (tnrp::TrackMapLibrary::edit).
class EditMapDialog : public QDialog {
    Q_OBJECT
public:
    explicit EditMapDialog(const QString& mapId, QWidget* parent = nullptr);

private:
    void setRotation(double degrees);
    void save();

    QString mapId_;
    std::shared_ptr<const tnrp::TrackMapData> base_;
    TrackMapWidget* preview_ = nullptr;
    QLineEdit* name_ = nullptr;
    QLineEdit* trackName_ = nullptr;
    QLineEdit* circuitName_ = nullptr;
    QSlider* rotationSlider_ = nullptr;
    QDoubleSpinBox* rotationSpin_ = nullptr;
    QLabel* error_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
    double rotation_ = 0;
};
