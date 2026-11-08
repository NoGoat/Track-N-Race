#include "PcAddressList.h"

#include "../IconUtils.h"

#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QStyle>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace {

QFont scaled(const QFont& base, double scale) {
    QFont font = base;
    font.setPointSizeF(qMax(7.0, base.pointSizeF() * scale));
    return font;
}

// One adapter: check mark, name over what it is for, and the address.
class AdapterRow : public QAbstractButton {
public:
    AdapterRow(const QString& title, const QString& detail, const QString& address, QWidget* parent = nullptr)
        : QAbstractButton(parent), title_(title), detail_(detail), address_(address) {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAccessibleName(QStringLiteral("%1 %2").arg(title, address));
    }

    QSize sizeHint() const override {
        return {300, fontMetrics().height() + QFontMetrics(scaled(font(), 0.85)).height() + 16};
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (underMouse()) {
            QColor hover = palette().color(QPalette::Highlight);
            hover.setAlpha(28);
            p.setPen(Qt::NoPen);
            p.setBrush(hover);
            p.drawRoundedRect(rect(), 4, 4);
        }
        const QColor accent = palette().color(QPalette::Highlight);
        const QColor text = isChecked() || underMouse() ? palette().color(QPalette::WindowText)
                                                        : palette().color(QPalette::WindowText).darker(115);
        const QFontMetrics fm(font());
        const int left = 10;
        if (isChecked()) {
            QFont check = font();
            check.setBold(true);
            p.setFont(check);
            p.setPen(accent);
            p.drawText(QRect(left, 0, 14, height()), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("✓"));
        }
        QFont addressFont = font();
        addressFont.setBold(isChecked());
        const int addressW = QFontMetrics(addressFont).horizontalAdvance(address_);
        const int textX = left + 26;
        const int textW = qMax(0, width() - textX - addressW - 20);

        p.setFont(font());
        p.setPen(text);
        p.drawText(QRect(textX, 6, textW, fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(title_, Qt::ElideRight, textW));
        const QFont detailFont = scaled(font(), 0.85);
        p.setFont(detailFont);
        p.setPen(palette().color(QPalette::PlaceholderText));
        p.drawText(QRect(textX, 6 + fm.height(), textW, QFontMetrics(detailFont).height() + 2),
                   Qt::AlignLeft | Qt::AlignVCenter, detail_);
        p.setFont(addressFont);
        p.setPen(isChecked() ? accent : text);
        p.drawText(QRect(0, 0, width() - 10, height()), Qt::AlignVCenter | Qt::AlignRight, address_);
    }

private:
    QString title_, detail_, address_;
};

QToolButton* copyButton(const QString& value) {
    auto* button = new QToolButton;
    button->setAutoRaise(true);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setIcon(adaptThemeIcon(QIcon::fromTheme(QStringLiteral("edit-copy")),
                                   button->palette().color(QPalette::WindowText),
                                   button->style()->standardIcon(QStyle::SP_FileDialogDetailedView)));
    button->setIconSize(QSize(12, 12));
    button->setText(QStringLiteral("Copy"));
    button->setToolTip(QStringLiteral("Copy %1").arg(value));
    QObject::connect(button, &QToolButton::clicked, button, [button, value] {
        QApplication::clipboard()->setText(value);
        button->setText(QStringLiteral("Copied"));
        QTimer::singleShot(1200, button, [button] { button->setText(QStringLiteral("Copy")); });
    });
    return button;
}

} // namespace

PcAddressList::PcAddressList(bool copyable, QWidget* parent)
    : QWidget(parent), copyable_(copyable)
{
    rows_ = new QVBoxLayout(this);
    rows_->setContentsMargins(0, 0, 0, 0);
    rows_->setSpacing(0);
    rebuild();
}

void PcAddressList::setInterfaces(const QVector<tnrp::NetworkInterfaceInfo>& interfaces) {
    interfaces_ = interfaces;
    loaded_ = true;
    rebuild();
}

void PcAddressList::setSelectedAddress(const QString& address) {
    if (address == selected_) return;
    selected_ = address;
    rebuild();
}

QString PcAddressList::preferredAddress(const QVector<tnrp::NetworkInterfaceInfo>& interfaces) {
    auto preferred = std::find_if(interfaces.cbegin(), interfaces.cend(), [](const auto& item) {
        return item.kind != "virtual" && item.kind != "loopback";
    });
    if (preferred == interfaces.cend())
        preferred = std::find_if(interfaces.cbegin(), interfaces.cend(),
                                 [](const auto& item) { return item.kind == "loopback"; });
    return preferred != interfaces.cend() ? QString::fromStdString(preferred->address)
                                          : QStringLiteral("127.0.0.1");
}

void PcAddressList::load(QObject* context, std::function<void(QVector<tnrp::NetworkInterfaceInfo>)> done) {
    QPointer<QObject> guard(context);
    QThreadPool::globalInstance()->start([guard, done = std::move(done)] {
        const std::vector<tnrp::NetworkInterfaceInfo> list = tnrp::listNetworkInterfaces();
        QVector<tnrp::NetworkInterfaceInfo> result(list.begin(), list.end());
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard, done, result] {
            if (guard) done(result);
        }, Qt::QueuedConnection);
    });
}

void PcAddressList::rebuild() {
    while (QLayoutItem* item = rows_->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }

    if (!loaded_ || interfaces_.isEmpty()) {
        auto* looking = new QLabel(loaded_ ? QStringLiteral("No network adapters found.")
                                           : QStringLiteral("Looking for network adapters…"));
        looking->setForegroundRole(QPalette::PlaceholderText);
        looking->setContentsMargins(10, 6, 10, 6);
        rows_->addWidget(looking);
        return;
    }

    int virtualCount = 0;
    for (const tnrp::NetworkInterfaceInfo& item : interfaces_) {
        const QString address = QString::fromStdString(item.address);
        if (item.kind == "virtual") ++virtualCount;
        if (item.kind == "virtual" && !showVirtual_ && address != selected_) continue;
        QString title = QString::fromStdString(item.name);
        QString detail = QStringLiteral("Console or another PC");
        if (item.kind == "loopback") {
            title = QStringLiteral("This PC only");
            detail = QStringLiteral("Game runs on this computer");
        } else if (item.kind == "virtual") {
            detail = QStringLiteral("Virtual adapter");
        }

        auto* line = new QWidget(this);
        auto* lineLayout = new QHBoxLayout(line);
        lineLayout->setContentsMargins(0, 0, 0, 0);
        lineLayout->setSpacing(4);
        auto* row = new AdapterRow(title, detail, address, line);
        row->setChecked(address == selected_);
        connect(row, &AdapterRow::clicked, this, [this, address] {
            if (address == selected_) return;
            selected_ = address;
            // Rebuilt after the click finishes, since this row is replaced.
            QTimer::singleShot(0, this, [this] { rebuild(); });
            emit addressSelected(address);
        });
        lineLayout->addWidget(row, 1);
        if (copyable_) {
            QWidget* copy = address == selected_ ? static_cast<QWidget*>(copyButton(address)) : new QWidget;
            copy->setFixedWidth(72);
            lineLayout->addWidget(copy);
        }
        rows_->addWidget(line);
    }

    if (virtualCount > 0) {
        auto* toggle = new QPushButton(
            showVirtual_ ? QStringLiteral("Hide virtual adapters")
                         : QStringLiteral("%1 virtual adapter%2 hidden. Show")
                               .arg(virtualCount).arg(virtualCount == 1 ? "" : "s"), this);
        toggle->setFlat(true);
        toggle->setCursor(Qt::PointingHandCursor);
        toggle->setStyleSheet(QStringLiteral(
            "QPushButton { border: none; background: transparent; padding: 6px 10px; text-align: left;"
            " color: palette(placeholder-text); }"
            "QPushButton:hover { color: palette(window-text); }"));
        toggle->setFont(scaled(toggle->font(), 0.9));
        connect(toggle, &QPushButton::clicked, this, [this] {
            showVirtual_ = !showVirtual_;
            QTimer::singleShot(0, this, [this] { rebuild(); });
        });
        auto* toggleRow = new QHBoxLayout;
        toggleRow->setContentsMargins(0, 0, 0, 0);
        toggleRow->addWidget(toggle);
        toggleRow->addStretch(1);
        auto* holder = new QWidget(this);
        holder->setLayout(toggleRow);
        rows_->addWidget(holder);
    }
}
