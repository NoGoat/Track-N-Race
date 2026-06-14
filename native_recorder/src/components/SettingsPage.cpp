#include "../MainWindow.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QCheckBox>
#include <QRadioButton>
#include <QPushButton>
#include <QLabel>
#include <QButtonGroup>
#include <QSizePolicy>
#include <QApplication>
#include <QStyleHints>

QWidget* MainWindow::buildSettingsTab() {
    QWidget* w = new QWidget;
    QVBoxLayout* vbox = new QVBoxLayout(w);
    vbox->setContentsMargins(16, 16, 16, 16);
    vbox->setSpacing(12);

    // ── Recording group ──────────────────────────────────────────
    QGroupBox* recGroup = new QGroupBox("Recording");
    QVBoxLayout* rv = new QVBoxLayout(recGroup);
    rv->setSpacing(8);

    recordCheck = new QCheckBox("Auto-record when a session starts");
    recordCheck->setChecked(wantRecord);
    rv->addWidget(recordCheck);

    QHBoxLayout* dirRow = new QHBoxLayout;
    dirLabel = new QLabel(outputDirectory.isEmpty() ? "No directory selected." : outputDirectory);
    dirLabel->setWordWrap(true);
    dirLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    QPushButton* browseBtn = new QPushButton("Browse…");
    browseBtn->setFixedWidth(90);
    dirRow->addWidget(dirLabel);
    dirRow->addWidget(browseBtn);
    rv->addLayout(dirRow);

    vbox->addWidget(recGroup);

    // ── Appearance group ─────────────────────────────────────────
    QGroupBox* appGroup = new QGroupBox("Appearance");
    QVBoxLayout* av = new QVBoxLayout(appGroup);
    av->setSpacing(6);

    themeSystem = new QRadioButton("System default");
    themeLight  = new QRadioButton("Light");
    themeDark   = new QRadioButton("Dark");

    const QString theme = settings.value("theme", "system").toString();
    if (theme == "light")     themeLight->setChecked(true);
    else if (theme == "dark") themeDark->setChecked(true);
    else                      themeSystem->setChecked(true);

    QButtonGroup* bg = new QButtonGroup(this);
    bg->addButton(themeSystem);
    bg->addButton(themeLight);
    bg->addButton(themeDark);

    av->addWidget(themeSystem);
    av->addWidget(themeLight);
    av->addWidget(themeDark);

    vbox->addWidget(appGroup);
    vbox->addStretch();

    connect(browseBtn,   &QPushButton::clicked,  this, &MainWindow::onBrowseDirectory);
    connect(recordCheck, &QCheckBox::toggled,     this, &MainWindow::onAutoRecordToggled);
    connect(themeSystem, &QRadioButton::toggled,  this, [this](bool) { onThemeChanged(); });
    connect(themeLight,  &QRadioButton::toggled,  this, [this](bool) { onThemeChanged(); });
    connect(themeDark,   &QRadioButton::toggled,  this, [this](bool) { onThemeChanged(); });

    return w;
}
