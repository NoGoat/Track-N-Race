#include "Attributions.h"
#include "MinimalController.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QIcon>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStyle>
#include <QTableWidget>
#include <QTextBrowser>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QtGlobal>

#include <memory>
#include <array>
#include <cstdio>
#include <string>

namespace {

QString text(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

// AppImage qt.conf restricts plugin lookup to the bundled runtime. Add the
// host's Qt plugin directory before QApplication is constructed so installed
// desktop styles (Breeze, Kvantum, Adwaita, etc.) remain available.
void addHostQtPluginPath() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QString qtpaths = QStandardPaths::findExecutable("qtpaths6");
    if (qtpaths.isEmpty()) qtpaths = QStandardPaths::findExecutable("qtpaths");
#else
    QString qtpaths = QStandardPaths::findExecutable("qtpaths-qt5");
    if (qtpaths.isEmpty()) qtpaths = QStandardPaths::findExecutable("qtpaths");
#endif
    if (qtpaths.isEmpty()) return;

    QString escaped = qtpaths;
    escaped.replace(QLatin1String("'"), QLatin1String("'\\''"));
    const QString command = QLatin1Char('\'') + escaped
        + QLatin1String("' -query QT_INSTALL_PLUGINS 2>/dev/null");

    FILE* pipe = popen(command.toLocal8Bit().constData(), "r");
    if (!pipe) return;
    std::array<char, 512> buffer{};
    QString output;
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
        output += QString::fromLocal8Bit(buffer.data());
    pclose(pipe);

    const QString hostPlugins = output.trimmed();
    if (hostPlugins.isEmpty() || !QDir(hostPlugins).exists()) return;
    const QByteArray existing = qgetenv("QT_PLUGIN_PATH");
    qputenv("QT_PLUGIN_PATH", existing.isEmpty()
        ? hostPlugins.toLocal8Bit()
        : hostPlugins.toLocal8Bit() + ':' + existing);
}

AppSettings loadSettings(QSettings& settings) {
    AppSettings result;
    result.outputFolder = settings.value("output-folder").toString().toStdString();
    result.bindAddress = settings.value("bind-address", "0.0.0.0").toString().toStdString();
    const uint port = settings.value("port", 20777).toUInt();
    result.port = port >= 1 && port <= 65535 ? static_cast<uint16_t>(port) : 20777;
    result.protocol = tnrp::overrideFromString(
        settings.value("protocol", "auto").toString().toStdString());
    return result;
}

QFrame* horizontalSeparator() {
    auto* separator = new QFrame;
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    return separator;
}

QLabel* subHeading(const QString& title) {
    auto* label = new QLabel(title);
    QFont font = label->font();
    font.setBold(true);
    font.setPointSizeF(font.pointSizeF() + 1.0);
    label->setFont(font);
    return label;
}

void showLicenseText(QWidget* parent, const Attribution& item) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QString("%1 — %2").arg(text(item.name), text(item.license)));
    dialog.setWindowFlags(Qt::Dialog);
    dialog.setWindowModality(Qt::ApplicationModal);
    dialog.setFixedSize(660, 560);

    auto* layout = new QVBoxLayout(&dialog);
    auto* browser = new QTextBrowser;
    browser->setOpenExternalLinks(true);
    browser->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    browser->setPlainText(text(item.licenseText));
    layout->addWidget(browser);

    auto* buttonRow = new QHBoxLayout;
    auto* copy = new QPushButton("Copy");
    copy->setIcon(QIcon::fromTheme(
        "edit-copy-symbolic", dialog.style()->standardIcon(QStyle::SP_FileDialogContentsView)));
    QObject::connect(copy, &QPushButton::clicked, browser, [browser] {
        QApplication::clipboard()->setText(browser->toPlainText());
    });
    buttonRow->addWidget(copy);
    buttonRow->addStretch(1);

    auto* close = new QPushButton("Close");
    close->setDefault(true);
    close->setIcon(QIcon::fromTheme(
        "dialog-close-symbolic", dialog.style()->standardIcon(QStyle::SP_DialogCloseButton)));
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    buttonRow->addWidget(close);
    layout->addLayout(buttonRow);
    dialog.exec();
}

void showAttributions(QWidget* parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle("About Track N Race Minimal Recorder");
    dialog.setWindowIcon(QIcon::fromTheme("track-n-race-minimal"));
    dialog.setWindowFlags(Qt::Dialog);
    dialog.setWindowModality(Qt::ApplicationModal);

    auto* outer = new QVBoxLayout(&dialog);
    outer->setSizeConstraint(QLayout::SetFixedSize);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 12, 8, 8);
    layout->setSpacing(8);

    const auto items = minimalAppAttributions();
    if (items.empty()) return;
    const Attribution& project = items.front();

    auto* name = new QLabel("Track N Race Minimal Recorder");
    QFont nameFont = name->font();
    nameFont.setBold(true);
    nameFont.setPointSizeF(nameFont.pointSizeF() + 3.0);
    name->setFont(nameFont);
    layout->addWidget(name);
    layout->addWidget(new QLabel("Version " + text(project.version)));

    auto* description = new QLabel(
        "Lightweight background telemetry recorder for F1 sim racing.");
    description->setWordWrap(true);
    layout->addWidget(description);
    layout->addWidget(new QLabel("© 2026 Track N Race"));

    auto* repository = new QLabel(
        "<a href=\"https://github.com/nogoat/track-n-race\">github.com/nogoat/track-n-race</a>");
    repository->setOpenExternalLinks(true);
    layout->addWidget(repository);

    auto* projectLicenseRow = new QHBoxLayout;
    projectLicenseRow->addWidget(new QLabel("Licensed under the GNU General Public License v3."));
    auto* viewProjectLicense = new QPushButton("View license");
    QObject::connect(viewProjectLicense, &QPushButton::clicked, &dialog,
                     [&dialog, &project] { showLicenseText(&dialog, project); });
    projectLicenseRow->addWidget(viewProjectLicense);
    projectLicenseRow->addStretch(1);
    layout->addLayout(projectLicenseRow);

    layout->addWidget(horizontalSeparator());
    layout->addWidget(subHeading("Third-party software"));

    const int libraryCount = static_cast<int>(items.size()) - 1;
    auto* table = new QTableWidget(libraryCount, 6);
    table->setHorizontalHeaderLabels(
        {"Library", "Version", "License", "Copyright", "Website", "View License"});
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setFocusPolicy(Qt::NoFocus);
    table->setShowGrid(false);
    table->setAlternatingRowColors(true);
    table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table->setMinimumWidth(860);

    auto* header = table->horizontalHeader();
    header->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 6; ++column)
        header->setSectionResizeMode(column, QHeaderView::ResizeToContents);

    for (int row = 0; row < libraryCount; ++row) {
        const Attribution& item = items[static_cast<size_t>(row + 1)];
        auto* library = new QTableWidgetItem(text(item.name));
        QFont font = library->font();
        font.setBold(true);
        library->setFont(font);
        table->setItem(row, 0, library);
        table->setItem(row, 1, new QTableWidgetItem(text(item.version)));
        table->setItem(row, 2, new QTableWidgetItem(text(item.license)));
        table->setItem(row, 3, new QTableWidgetItem(text(item.copyright)));

        const QString url = text(item.website);
        auto* link = new QLabel(QString("<a href=\"%1\">%2</a>")
                                    .arg(url.toHtmlEscaped(), QUrl(url).host()));
        link->setOpenExternalLinks(true);
        link->setContentsMargins(4, 0, 24, 0);
        link->setMinimumWidth(link->fontMetrics().horizontalAdvance(QUrl(url).host()) + 34);
        table->setCellWidget(row, 4, link);

        auto* view = new QToolButton;
        view->setAutoRaise(true);
        view->setCursor(Qt::PointingHandCursor);
        view->setToolTip("View license");
        view->setIcon(QIcon::fromTheme(
            "quickview-symbolic", table->style()->standardIcon(QStyle::SP_FileDialogContentsView)));
        const Attribution* attribution = &item;
        QObject::connect(view, &QToolButton::clicked, &dialog,
                         [&dialog, attribution] { showLicenseText(&dialog, *attribution); });
        auto* viewCell = new QWidget;
        auto* viewLayout = new QHBoxLayout(viewCell);
        viewLayout->setContentsMargins(0, 0, 0, 0);
        viewLayout->addStretch(1);
        viewLayout->addWidget(view);
        viewLayout->addStretch(1);
        table->setCellWidget(row, 5, viewCell);
    }

    table->resizeRowsToContents();
    int rowHeight = 0;
    for (int row = 0; row < libraryCount; ++row)
        rowHeight = qMax(rowHeight, table->rowHeight(row));
    for (int row = 0; row < libraryCount; ++row)
        table->setRowHeight(row, rowHeight);
    table->setFixedHeight(table->horizontalHeader()->height() + 2 * table->frameWidth()
                          + libraryCount * rowHeight);
    layout->addWidget(table);
    layout->addStretch(1);
    outer->addWidget(page);

    outer->addWidget(horizontalSeparator());
    auto* bottom = new QHBoxLayout;
    bottom->setContentsMargins(12, 8, 12, 12);
    bottom->addStretch(1);
    auto* close = new QPushButton("Close");
    close->setDefault(true);
    close->setIcon(QIcon::fromTheme(
        "dialog-close-symbolic", dialog.style()->standardIcon(QStyle::SP_DialogCloseButton)));
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    bottom->addWidget(close);
    outer->addLayout(bottom);
    dialog.exec();
}

class RecorderWindow final : public QWidget {
public:
    RecorderWindow(QSettings& settings, std::unique_ptr<MinimalController>& controller)
        : settings_(settings), controller_(controller) {
        setWindowTitle("Track N Race Minimal Recorder");
        setWindowIcon(QIcon::fromTheme("track-n-race-minimal"));
        resize(620, 330);

        auto* grid = new QGridLayout(this);
        grid->setContentsMargins(20, 20, 20, 20);
        grid->setHorizontalSpacing(12);
        grid->setVerticalSpacing(12);

        folderEntry_ = new QLineEdit;
        folderEntry_->setReadOnly(true);
        auto* browse = new QPushButton("Browse...");
        addRow(grid, 0, "Recording folder", folderEntry_, browse);

        protocol_ = new QComboBox;
        protocol_->addItems({"Auto", "F1 24", "F1 25", "F1 26"});
        addRow(grid, 1, "Protocol override", protocol_);

        addressEntry_ = new QLineEdit;
        addRow(grid, 2, "IPv4 bind address", addressEntry_);

        port_ = new QSpinBox;
        port_->setRange(1, 65535);
        auto* apply = new QPushButton("Apply network");
        addRow(grid, 3, "UDP port", port_, apply);

        auto* separator = new QFrame;
        separator->setFrameShape(QFrame::HLine);
        grid->addWidget(separator, 4, 0, 1, 3);

        circuit_ = new QLabel("Unavailable");
        addRow(grid, 5, "Circuit name", circuit_);
        session_ = new QLabel("Unavailable");
        auto* attributions = new QPushButton("Attributions...");
        addRow(grid, 6, "Session", session_, attributions);

        const AppSettings& current = controller_->settings();
        folderEntry_->setText(QString::fromStdString(current.outputFolder));
        addressEntry_->setText(QString::fromStdString(current.bindAddress));
        port_->setValue(current.port);
        protocol_->setCurrentIndex(current.protocol == tnrp::Override::F1_24 ? 1
                                 : current.protocol == tnrp::Override::F1_25 ? 2
                                 : current.protocol == tnrp::Override::F1_26 ? 3 : 0);

        connect(browse, &QPushButton::clicked, this, [this] { chooseFolder(); });
        connect(apply, &QPushButton::clicked, this, [this] { applyNetwork(); });
        connect(attributions, &QPushButton::clicked, this, [this] { showAttributions(this); });
        connect(protocol_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int index) {
            const tnrp::Override value = index == 1 ? tnrp::Override::F1_24
                                       : index == 2 ? tnrp::Override::F1_25
                                       : index == 3 ? tnrp::Override::F1_26
                                                    : tnrp::Override::Auto;
            controller_->setProtocol(value);
            settings_.setValue("protocol", QString::fromLatin1(tnrp::toString(value)));
        });

        controller_->setSessionCallback([this](std::string circuit, std::string session) {
            const QString circuitText = QString::fromStdString(circuit);
            const QString sessionText = QString::fromStdString(session);
            QMetaObject::invokeMethod(this, [this, circuitText, sessionText] {
                circuit_->setText(circuitText);
                session_->setText(sessionText);
            }, Qt::QueuedConnection);
        });

        std::string error;
        if (!controller_->start(error)) showError(error);
    }

private:
    static void addRow(QGridLayout* grid, int row, const QString& title,
                       QWidget* value, QWidget* action = nullptr) {
        grid->addWidget(new QLabel(title), row, 0);
        grid->addWidget(value, row, 1, 1, action ? 1 : 2);
        if (action) grid->addWidget(action, row, 2);
    }

    void showError(const std::string& error) {
        QMessageBox::critical(this, "Track N Race Minimal Recorder",
                              QString::fromStdString(error));
    }

    void chooseFolder() {
        const QString folder = QFileDialog::getExistingDirectory(
            this, "Select recording folder", folderEntry_->text());
        if (folder.isEmpty()) return;
        std::string error;
        if (!controller_->setOutputFolder(folder.toStdString(), error)) {
            showError(error);
            return;
        }
        folderEntry_->setText(folder);
        settings_.setValue("output-folder", folder);
    }

    void applyNetwork() {
        std::string error;
        if (!controller_->applyNetwork(addressEntry_->text().toStdString(),
                                       static_cast<uint16_t>(port_->value()), error)) {
            showError(error);
            const AppSettings& current = controller_->settings();
            addressEntry_->setText(QString::fromStdString(current.bindAddress));
            port_->setValue(current.port);
            return;
        }
        settings_.setValue("bind-address", addressEntry_->text());
        settings_.setValue("port", port_->value());
    }

    QSettings& settings_;
    std::unique_ptr<MinimalController>& controller_;
    QLineEdit* folderEntry_{};
    QComboBox* protocol_{};
    QLineEdit* addressEntry_{};
    QSpinBox* port_{};
    QLabel* circuit_{};
    QLabel* session_{};
};

} // namespace

int main(int argc, char** argv) {
    // The bundled portal platform theme follows the host desktop and supplies
    // native file dialogs on both X11 and Wayland. It gracefully falls back
    // when the desktop portal service is unavailable.
    if (qgetenv("QT_QPA_PLATFORMTHEME").isEmpty())
        qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
    addHostQtPluginPath();

    QApplication application(argc, argv);
    QApplication::setOrganizationName("TrackNRace");
    QApplication::setApplicationName("MinimalRecorder");
    QApplication::setApplicationDisplayName("Track N Race Minimal Recorder");

    QSettings settings;
    auto controller = std::make_unique<MinimalController>(loadSettings(settings));
    RecorderWindow window(settings, controller);
    window.show();
    const int result = application.exec();

    controller->setSessionCallback({});
    controller.reset();
    settings.sync();
    return result;
}
