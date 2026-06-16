#include "SettingsDialog.h"
#include "../MainWindow.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QCheckBox>
#include <QRadioButton>
#include <QPushButton>
#include <QFileDialog>
#include <QSizePolicy>
#include <QFont>

// Bold label spanning both form columns, used as a section header row —
// QFormLayout::addRow(QWidget*) puts a single widget across the full width.
static QLabel* sectionHeading(const QString& text) {
    QLabel* l = new QLabel(text);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

static QFrame* horizontalSeparator() {
    QFrame* f = new QFrame;
    f->setFrameShape(QFrame::HLine);
    f->setFrameShadow(QFrame::Sunken);
    return f;
}

SettingsDialog::SettingsDialog(MainWindow* mainWindow, QWidget* parent)
    : QDialog(parent), mainWindow_(mainWindow)
{
    setWindowTitle("Settings");
    // Qt::Dialog gives the plain dialog frame (no minimize/maximize buttons)
    // without the bitwise flag-stripping the old code needed for that. Modal +
    // parented to MainWindow (see the call site) is what actually keeps this
    // above MainWindow and blocks it — the window-type flag alone doesn't.
    setWindowFlags(Qt::Dialog);
    setWindowModality(Qt::ApplicationModal);

    QVBoxLayout* main = new QVBoxLayout(this);
    main->setSizeConstraint(QLayout::SetFixedSize);
    main->setContentsMargins(20, 20, 20, 16);
    main->setSpacing(16);

    QFormLayout* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(10);

    addRecordingSection(form);
    form->addRow(horizontalSeparator());
    addAppearanceSection(form);

    main->addLayout(form);

    // ── Close ────────────────────────────────────────────────────
    QHBoxLayout* bottom = new QHBoxLayout;
    bottom->addStretch(1);
    QPushButton* closeBtn = new QPushButton("Close");
    closeBtn->setDefault(true);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    bottom->addWidget(closeBtn);
    main->addLayout(bottom);
}

void SettingsDialog::addRecordingSection(QFormLayout* form) {
    form->addRow(sectionHeading("Recording"));

    recordCheck_ = new QCheckBox("Auto-record when a session starts");
    recordCheck_->setChecked(mainWindow_->autoRecordEnabled());
    form->addRow(QString(), recordCheck_);

    QWidget* dirRow = new QWidget;
    QHBoxLayout* dirLay = new QHBoxLayout(dirRow);
    dirLay->setContentsMargins(0, 0, 0, 0);
    const QString dir = mainWindow_->currentOutputDirectory();
    dirLabel_ = new QLabel(dir.isEmpty() ? "No directory selected." : dir);
    dirLabel_->setWordWrap(true);
    dirLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    dirLabel_->setMaximumWidth(280);
    QPushButton* browseBtn = new QPushButton("Browse…");
    browseBtn->setFixedWidth(90);
    dirLay->addWidget(dirLabel_);
    dirLay->addWidget(browseBtn);
    form->addRow("Save to:", dirRow);

    connect(recordCheck_, &QCheckBox::toggled, this, [this](bool on) {
        mainWindow_->setAutoRecord(on);
    });
    connect(browseBtn, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(
            this, "Select Output Directory", mainWindow_->currentOutputDirectory());
        if (!dir.isEmpty()) {
            mainWindow_->setOutputDirectory(dir);
            dirLabel_->setText(dir);
        }
    });
}

void SettingsDialog::addAppearanceSection(QFormLayout* form) {
    form->addRow(sectionHeading("Appearance"));

    QWidget* themeRow = new QWidget;
    QHBoxLayout* themeLay = new QHBoxLayout(themeRow);
    themeLay->setContentsMargins(0, 0, 0, 0);
    themeLay->setSpacing(14);
    themeSystem_ = new QRadioButton("System default");
    themeLight_  = new QRadioButton("Light");
    themeDark_   = new QRadioButton("Dark");
    themeLay->addWidget(themeSystem_);
    themeLay->addWidget(themeLight_);
    themeLay->addWidget(themeDark_);

    const QString theme = mainWindow_->currentTheme();
    if (theme == "light")     themeLight_->setChecked(true);
    else if (theme == "dark") themeDark_->setChecked(true);
    else                      themeSystem_->setChecked(true);

    form->addRow("Theme:", themeRow);

    toolbarLabelsCheck_ = new QCheckBox("Show button labels in toolbar");
    toolbarLabelsCheck_->setChecked(mainWindow_->toolbarLabelsEnabled());
    form->addRow("Toolbar:", toolbarLabelsCheck_);

    auto applyTheme = [this](bool) {
        QString val = "system";
        if (themeLight_->isChecked())     val = "light";
        else if (themeDark_->isChecked()) val = "dark";
        mainWindow_->setTheme(val);
    };
    connect(themeSystem_, &QRadioButton::toggled, this, applyTheme);
    connect(themeLight_,  &QRadioButton::toggled, this, applyTheme);
    connect(themeDark_,   &QRadioButton::toggled, this, applyTheme);
    connect(toolbarLabelsCheck_, &QCheckBox::toggled, this, [this](bool on) {
        mainWindow_->setToolbarLabels(on);
    });
}
