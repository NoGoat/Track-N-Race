#include "AnalysisMetricPicker.h"
#include "AnalysisSeriesModel.h"

#include "../../IconUtils.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kPopupWidth = 330;
constexpr int kContentMargin = 10;

// Wrapping row layout for the toggle chips (after Qt's flow-layout example).
// It wraps at a fixed width and reports the resulting height as its size hint,
// so the scroll area around it always knows the full content height.
class FlowLayout : public QLayout {
public:
    FlowLayout(QWidget* parent, int spacing, int wrapWidth)
        : QLayout(parent), spacing_(spacing), wrapWidth_(wrapWidth) {
        setContentsMargins(0, 0, 0, 0);
    }
    ~FlowLayout() override {
        while (QLayoutItem* item = takeAt(0)) delete item;
    }

    void addItem(QLayoutItem* item) override { items_.push_back(item); }
    int count() const override { return static_cast<int>(items_.size()); }
    QLayoutItem* itemAt(int index) const override {
        return index >= 0 && index < items_.size() ? items_[index] : nullptr;
    }
    QLayoutItem* takeAt(int index) override {
        return index >= 0 && index < items_.size() ? items_.takeAt(index) : nullptr;
    }
    Qt::Orientations expandingDirections() const override { return {}; }
    void setGeometry(const QRect& rect) override {
        QLayout::setGeometry(rect);
        arrange(rect, false);
    }
    QSize sizeHint() const override {
        return {wrapWidth_, arrange(QRect(0, 0, wrapWidth_, 0), true)};
    }
    QSize minimumSize() const override { return sizeHint(); }

private:
    QVector<QLayoutItem*> items_;
    int spacing_;
    int wrapWidth_;

    int arrange(const QRect& rect, bool measureOnly) const {
        int x = rect.x();
        int y = rect.y();
        int lineHeight = 0;
        for (QLayoutItem* item : items_) {
            if (item->isEmpty()) continue;   // hidden by the filter
            const QSize hint = item->sizeHint();
            if (x + hint.width() > rect.x() + rect.width() && lineHeight > 0) {
                x = rect.x();
                y += lineHeight + spacing_;
                lineHeight = 0;
            }
            if (!measureOnly) item->setGeometry(QRect(QPoint(x, y), hint));
            x += hint.width() + spacing_;
            lineHeight = qMax(lineHeight, hint.height());
        }
        return y + lineHeight - rect.y();
    }
};

QFrame* separator(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    return line;
}

// Every token must appear somewhere, so "rear ride" and "ride rear" both find
// Rear Ride Height.
bool matches(const QString& query, const QString& haystack) {
    const QStringList tokens = query.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    return std::all_of(tokens.cbegin(), tokens.cend(),
                       [&](const QString& token) { return haystack.contains(token); });
}

} // namespace

AnalysisMetricPicker::AnalysisMetricPicker(AnalysisSeriesModel* model, QWidget* parent)
    : QFrame(parent, Qt::Popup), model_(model) {
    setFrameShape(QFrame::StyledPanel);
    setAttribute(Qt::WA_WindowPropagation);
    setAccessibleName(QStringLiteral("Add Metrics"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* filterBox = new QWidget(this);
    auto* filterLayout = new QVBoxLayout(filterBox);
    filterLayout->setContentsMargins(8, 8, 8, 8);
    filter_ = new QLineEdit(filterBox);
    filter_->setPlaceholderText(QStringLiteral("Filter metrics"));
    filter_->setClearButtonEnabled(true);
    filter_->setAccessibleName(QStringLiteral("Filter metrics"));
    filter_->setToolTip(QStringLiteral("Type to filter. Enter adds the first match not yet charted."));
    filter_->addAction(adaptThemeIcon(QIcon::fromTheme(QStringLiteral("edit-find")),
                                      palette().color(QPalette::WindowText),
                                      style()->standardIcon(QStyle::SP_FileDialogContentsView)),
                       QLineEdit::LeadingPosition);
    filterLayout->addWidget(filter_);
    root->addWidget(filterBox);

    auto* line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    root->addWidget(line);

    scroll_ = new QScrollArea(this);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setWidgetResizable(true);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    content_ = new QWidget(scroll_);
    auto* sections = new QVBoxLayout(content_);
    sections->setContentsMargins(kContentMargin, 8, kContentMargin, 10);
    sections->setSpacing(10);
    // Chips wrap at the content width left beside a vertical scroll bar.
    wrapWidth_ = kPopupWidth - 2 * frameWidth() - 2 * kContentMargin -
                 style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, this);

    // Chip blocks per group; per-corner tyre metrics go in the matrix instead.
    for (const QString& group : analyzeMetricGroups()) {
        QStringList ids;
        for (const AnalyzeMetric& metric : analyzeMetrics())
            if (metric.group == group && !analyzeCornerRow(metric.id)) ids << metric.id;
        if (group == QLatin1String("Tyres")) {
            sections->addWidget(buildTyreMatrix());
            if (!ids.isEmpty()) sections->addWidget(buildChipSection(QStringLiteral("Tyre Averages"), ids));
        } else if (!ids.isEmpty()) {
            sections->addWidget(buildChipSection(group, ids));
        }
    }
    empty_ = new QLabel(content_);
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setForegroundRole(QPalette::PlaceholderText);
    empty_->hide();
    sections->addWidget(empty_);
    sections->addStretch(1);
    scroll_->setWidget(content_);
    root->addWidget(scroll_, 1);

    connect(filter_, &QLineEdit::textChanged, this, &AnalysisMetricPicker::applyFilter);
    connect(filter_, &QLineEdit::returnPressed, this, &AnalysisMetricPicker::addFirstMatch);
    connect(model_, &AnalysisSeriesModel::seriesEdited, this, &AnalysisMetricPicker::refreshStates);
    connect(model_, &QAbstractItemModel::modelReset, this, &AnalysisMetricPicker::refreshStates);
    refreshStates();
}

QWidget* AnalysisMetricPicker::buildChipSection(const QString& title, const QStringList& metricIds) {
    auto* section = new QWidget(content_);
    section->setAccessibleName(title);
    auto* layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    Section entry;
    entry.widget = section;
    entry.separator = separator(section);
    layout->addWidget(entry.separator);
    auto* flowHost = new QWidget(section);
    auto* flow = new FlowLayout(flowHost, 4, wrapWidth_);
    for (const QString& id : metricIds) {
        const AnalyzeMetric* metric = analyzeMetric(id);
        if (!metric) continue;
        auto* chip = new QToolButton(flowHost);
        chip->setText(metric->label);
        chip->setCheckable(true);
        chip->setToolButtonStyle(Qt::ToolButtonTextOnly);
        chip->setFocusPolicy(Qt::StrongFocus);
        flow->addWidget(chip);
        connect(chip, &QToolButton::clicked, this, [this, id](bool on) {
            model_->setMetricsSelected({id}, on);
        });
        entry.chips.push_back(static_cast<int>(chips_.size()));
        chips_.push_back({id, (metric->group + QLatin1Char(' ') + metric->label + QLatin1Char(' ') +
                               metric->unit).toLower(),
                          chip});
    }
    layout->addWidget(flowHost);
    sections_.push_back(entry);
    return section;
}

QWidget* AnalysisMetricPicker::buildTyreMatrix() {
    auto* section = new QWidget(content_);
    section->setAccessibleName(QStringLiteral("Tyres by corner"));
    auto* layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    QFrame* rule = separator(section);
    layout->addWidget(rule);

    auto* matrix = new QWidget(section);
    auto* grid = new QGridLayout(matrix);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(4);
    grid->setColumnStretch(0, 1);

    auto header = [&](const QString& text, int column, const QString& tip) {
        auto* label = new QLabel(text, matrix);
        QFont font = label->font();
        font.setPointSizeF(qMax(7.0, font.pointSizeF() * 0.8));
        label->setFont(font);
        label->setForegroundRole(QPalette::PlaceholderText);
        label->setAlignment(Qt::AlignCenter);
        label->setToolTip(tip);
        grid->addWidget(label, 0, column, Qt::AlignHCenter);
    };
    header(QStringLiteral("COM"), 1, QStringLiteral("Draw the picked corners in one series"));
    const auto& corners = analyzeTyreCorners();
    for (int corner = 0; corner < corners.size(); ++corner)
        header(corners[corner].label, 2 + corner, QString());
    header(QStringLiteral("ALL"), 2 + static_cast<int>(corners.size()),
           QStringLiteral("Pick or clear all four corners"));

    QStringList cornerTerms;
    for (const AnalyzeTyreCorner& corner : corners) cornerTerms << corner.label;
    const auto& rows = analyzeTyreRows();
    for (int index = 0; index < rows.size(); ++index) {
        const AnalyzeTyreRow& row = rows[index];
        const int gridRow = index + 1;
        TyreRow entry;
        entry.idPrefix = row.idPrefix;
        entry.haystack = (QStringLiteral("tyres tires ") + row.label +
                          QStringLiteral(" all com combined ") + cornerTerms.join(QLatin1Char(' ')))
                             .toLower();
        entry.label = new QLabel(row.label, matrix);
        grid->addWidget(entry.label, gridRow, 0);
        entry.widgets << entry.label;

        auto box = [&](int column, const QString& tip) {
            auto* check = new QCheckBox(matrix);
            check->setToolTip(tip);
            check->setAccessibleName(tip);
            grid->addWidget(check, gridRow, column, Qt::AlignHCenter);
            entry.widgets << check;
            return check;
        };
        const QString prefix = row.idPrefix;
        entry.combined = box(1, QStringLiteral("Show %1 corners together in one graph").arg(row.label));
        connect(entry.combined, &QCheckBox::clicked, this,
                [this, prefix] { model_->toggleTyreCombined(prefix); });
        for (int corner = 0; corner < corners.size(); ++corner) {
            const QString key = corners[corner].key;
            QCheckBox* check = box(2 + corner, QStringLiteral("%1 %2").arg(row.label, corners[corner].label));
            connect(check, &QCheckBox::clicked, this,
                    [this, prefix, key] { model_->toggleTyreCorner(prefix, key); });
            entry.corners << check;
        }
        entry.all = box(2 + static_cast<int>(corners.size()),
                        QStringLiteral("All %1 corners").arg(row.label));
        entry.all->setTristate(true);   // partly checked when some corners are picked
        connect(entry.all, &QCheckBox::clicked, this,
                [this, prefix] { model_->toggleTyreAllCorners(prefix); });
        tyreRows_.push_back(entry);
    }
    layout->addWidget(matrix);

    Section sectionEntry;
    sectionEntry.widget = section;
    sectionEntry.separator = rule;
    sectionEntry.tyreMatrix = true;
    sections_.push_back(sectionEntry);
    return section;
}

void AnalysisMetricPicker::popup(const QWidget* anchor) {
    {
        QSignalBlocker guard(filter_);
        filter_->clear();
    }
    applyFilter();
    refreshStates();

    // Below the anchor when it fits, otherwise above it; never off screen.
    const QRect anchorRect(anchor->mapToGlobal(QPoint(0, 0)), anchor->size());
    const QScreen* screen = anchor->screen();
    const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
    const int width = qMin(kPopupWidth, available.width() - 16);
    content_->ensurePolished();
    const int chrome = filter_->sizeHint().height() + 16 + 2 + 2 * frameWidth();
    const int wanted = chrome + content_->sizeHint().height();
    const int below = available.bottom() - anchorRect.bottom() - 4;
    const int above = anchorRect.top() - available.top() - 4;
    const bool openBelow = below >= qMin(wanted, 360) || below >= above;
    const int height = qMax(200, qMin(wanted, openBelow ? below : above));
    int x = anchorRect.left();
    x = qBound(available.left() + 8, x, available.right() - width - 8);
    const int y = openBelow ? anchorRect.bottom() + 4 : anchorRect.top() - 4 - height;
    setGeometry(x, y, width, height);
    show();
    filter_->setFocus(Qt::PopupFocusReason);
}

void AnalysisMetricPicker::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        close();
        return;
    }
    QFrame::keyPressEvent(event);
}

void AnalysisMetricPicker::applyFilter() {
    const QString query = filter_->text().trimmed().toLower();
    bool anyVisible = false;
    for (const Section& section : sections_) {
        bool sectionVisible = false;
        if (section.tyreMatrix) {
            for (const TyreRow& row : tyreRows_) {
                const bool shown = matches(query, row.haystack);
                for (QWidget* widget : row.widgets) widget->setVisible(shown);
                sectionVisible |= shown;
            }
        } else {
            for (int index : section.chips) {
                const Chip& chip = chips_[index];
                const bool shown = matches(query, chip.haystack);
                chip.button->setVisible(shown);
                sectionVisible |= shown;
            }
        }
        section.widget->setVisible(sectionVisible);
        section.separator->setVisible(anyVisible);   // no rule above the first block
        anyVisible |= sectionVisible;
    }
    empty_->setText(QStringLiteral("No metrics match “%1”").arg(filter_->text().trimmed()));
    empty_->setVisible(!anyVisible);
}

void AnalysisMetricPicker::refreshStates() {
    const QSet<QString> selected = model_->selectedIds();
    for (const Chip& chip : chips_) {
        const bool on = selected.contains(chip.metricId);
        QSignalBlocker guard(chip.button);
        chip.button->setChecked(on);
        chip.button->setToolTip(QStringLiteral("%1 %2").arg(on ? QStringLiteral("Remove")
                                                               : QStringLiteral("Add"),
                                                            chip.button->text()));
    }
    const auto& corners = analyzeTyreCorners();
    for (const TyreRow& row : tyreRows_) {
        int picked = 0;
        for (int corner = 0; corner < row.corners.size(); ++corner) {
            const bool on = model_->cornerPicked(row.idPrefix, corners[corner].key);
            QSignalBlocker guard(row.corners[corner]);
            row.corners[corner]->setChecked(on);
            picked += on ? 1 : 0;
        }
        {
            QSignalBlocker guard(row.combined);
            row.combined->setChecked(model_->isCombined(row.idPrefix));
        }
        {
            QSignalBlocker guard(row.all);
            row.all->setCheckState(picked == 0 ? Qt::Unchecked
                                   : picked == row.corners.size() ? Qt::Checked
                                                                  : Qt::PartiallyChecked);
        }
        // Rows with anything charted read in the normal text colour.
        row.label->setForegroundRole(picked > 0 ? QPalette::WindowText : QPalette::PlaceholderText);
    }
}

void AnalysisMetricPicker::addFirstMatch() {
    // Enter charts the first visible chip that is not charted yet.
    for (const Section& section : sections_) {
        if (section.tyreMatrix || section.widget->isHidden()) continue;
        for (int index : section.chips) {
            const Chip& chip = chips_[index];
            if (chip.button->isHidden() || chip.button->isChecked()) continue;
            model_->setMetricsSelected({chip.metricId}, true);
            return;
        }
    }
}
