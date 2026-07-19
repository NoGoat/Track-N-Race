#pragma once

#include <QIcon>
#include <QIconEngine>
#include <QPainter>
#include <QStyle>
#include <QWidget>
#include <QApplication>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QPalette>
// TEMP DIAGNOSTIC (icon blur investigation) — remove with the logging block below.
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QSet>

// Recolours a source theme icon to a fixed tint while keeping it fully scalable.
// It renders the source at the exact size + device-pixel-ratio Qt requests (so it
// stays crisp on HiDPI / fractional display scaling) and tints via SourceIn —
// unlike baking a handful of fixed-size pixmaps, which Qt then scales to whatever
// size the toolbar actually wants, producing blur.
#if defined(Q_OS_WIN) || defined(HAVE_BREEZE_ICONS)
class TintedIconEngine : public QIconEngine {
public:
    // followForeground: for the normal (enabled) state, resolve the tint from the
    // live application-palette foreground at paint time rather than the colour
    // captured here. Icons built early — e.g. the toolbar, whose actions are created
    // during window construction before the dark Breeze palette has fully settled —
    // would otherwise bake in the wrong (light-palette) foreground and render dark on
    // a dark toolbar, with no palette-change afterwards to rebuild them. Resolving
    // live fixes that without depending on a rebuild (this is exactly what the
    // disabled state already does, which is why disabled icons were unaffected).
    // Fixed-colour icons (e.g. a weather glyph's own colour) leave this false so
    // their tint is honoured verbatim.
    TintedIconEngine(QIcon src, QColor tint, bool followForeground = false)
        : src_(std::move(src)), tint_(tint), follow_(followForeground) {}
    QIconEngine* clone() const override { return new TintedIconEngine(src_, tint_, follow_); }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state,
                         qreal scale) override {
        QPixmap pm = src_.pixmap(size, scale, mode, state);  // crisp at device res
        if (pm.isNull()) return pm;
        // SourceIn fills the glyph with a flat tint, which discards the mode-specific
        // shading Qt would normally apply. Disabled actions tint with the palette's
        // disabled foreground so they dim like every other widget; follow-foreground
        // icons track the live palette foreground (see above); otherwise the fixed
        // tint captured at construction is used.
        QColor tint = tint_;
        if (mode == QIcon::Disabled)
            tint = QApplication::palette().color(QPalette::Disabled, QPalette::WindowText);
        else if (follow_)
            tint = QApplication::palette().color(QPalette::WindowText);
        QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QPainter p(&img);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(img.rect(), tint);
        p.end();
        QPixmap out = QPixmap::fromImage(img);
        out.setDevicePixelRatio(scale);
        // TEMP DIAGNOSTIC (icon blur investigation): record requested vs actual
        // source pixmap size to <temp>/tnr_icon_debug.log (GUI apps have no console).
        // `pm` is what gets drawn; if its device size is smaller than size*scale the
        // source didn't resolve as a scalable SVG and is being upscaled → blur.
        // Deduped per unique (size, scale, result) so repaints don't flood the file.
        {
            static QSet<QString> seen;
            const QString key = QStringLiteral("%1x%2@%3->%4x%5@%6f%7")
                .arg(size.width()).arg(size.height()).arg(scale)
                .arg(pm.width()).arg(pm.height()).arg(pm.devicePixelRatio()).arg(follow_);
            if (!seen.contains(key)) {
                seen.insert(key);
                QFile lf(QCoreApplication::applicationDirPath() + QStringLiteral("/tnr_icon_debug.log"));
                if (lf.open(QIODevice::Append | QIODevice::Text)) {
                    QTextStream(&lf)
                        << "icon requested " << size.width() << "x" << size.height()
                        << "  scale " << scale
                        << "  -> source pixmap " << pm.width() << "x" << pm.height()
                        << " (dpr " << pm.devicePixelRatio() << ")"
                        << "  followFg " << follow_ << "\n";
                }
            }
        }
        return out;
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode,
               QIcon::State state) override {
        const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
        painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, dpr));
    }
private:
    QIcon  src_;
    QColor tint_;
    bool   follow_ = false;
};
#endif

// The bundled Breeze action icons are monochrome SVGs meant to be recoloured at
// runtime to the palette foreground by KDE's KIconThemes engine. We ship the
// icons and select the "breeze" theme, but load them through plain Qt, which does
// NOT recolour them — so in dark mode they'd render in their built-in dark colour
// (≈ black on a dark background). When the bundled Breeze theme is the *active*
// icon theme, tint them to `tint` (the palette foreground) ourselves, on any
// platform. Anything else — a real KDE session that recolours its own breeze
// icons, system icons under a non-Breeze style, or a build without bundled Breeze
// icons — is returned untouched (the compile guard keeps dev builds out of here).
inline QIcon adaptThemeIcon(const QIcon& themed, const QColor& tint, const QIcon& fallback) {
    if (themed.isNull()) return fallback;
#if defined(Q_OS_WIN) || defined(HAVE_BREEZE_ICONS)
    if (QIcon::themeName() == QLatin1String("breeze"))
        return QIcon(new TintedIconEngine(themed, tint, /*followForeground=*/true));
#endif
    Q_UNUSED(tint);
    return themed;
}

// Loads a bundled Breeze icon *directly* from the embedded icon resource
// (:/icons/breeze/<subpath>/<name>.svg) rather than through QIcon::fromTheme, so
// it returns the Breeze glyph regardless of the platform's active icon theme
// (on Linux the OS theme is primary, with Breeze only a fallback). The Breeze
// symbolic icons are monochrome SVGs, so tint to `tint` ourselves, exactly as
// adaptThemeIcon() does for themed icons. `subpath` is the theme-relative folder
// (e.g. "applets/48"). Returns a null icon when Breeze isn't bundled (resource
// absent) or the named icon doesn't exist, so callers can fall back gracefully.
inline QIcon breezeIcon(const QString& name, const QColor& tint,
                        const QString& subpath = QStringLiteral("applets/48")) {
    QIcon raw(QStringLiteral(":/icons/breeze/%1/%2.svg").arg(subpath, name));
    if (raw.isNull()) return QIcon();
#if defined(Q_OS_WIN) || defined(HAVE_BREEZE_ICONS)
    return QIcon(new TintedIconEngine(raw, tint));
#else
    Q_UNUSED(tint);
    return raw;
#endif
}

#if defined(Q_OS_WIN) || defined(HAVE_BREEZE_ICONS)
// QMessageBox / QDialogButtonBox don't build their button icons through
// adaptThemeIcon — they ask the active QStyle for them via standardIcon(). Under
// Breeze those are the same monochrome theme icons plain Qt won't recolour, so
// they render black (invisible in dark mode), as seen on a dialog's No/Cancel
// button. Wrap the style so the *monochrome* button/navigation standard icons are
// tinted to the palette foreground while the bundled Breeze theme is active.
// Colourful standard icons (message-box severity, file/device icons) fall through
// the switch and are returned untouched.
class BreezeIconProxyStyle : public QProxyStyle {
public:
    explicit BreezeIconProxyStyle(QStyle* base) : QProxyStyle(base) {}

    QIcon standardIcon(StandardPixmap sp, const QStyleOption* opt,
                       const QWidget* w) const override {
        const QIcon base = QProxyStyle::standardIcon(sp, opt, w);
        if (base.isNull() || QIcon::themeName() != QLatin1String("breeze"))
            return base;
        switch (sp) {
            case SP_DialogOkButton:      case SP_DialogCancelButton:
            case SP_DialogYesButton:     case SP_DialogNoButton:
            case SP_DialogApplyButton:   case SP_DialogResetButton:
            case SP_DialogDiscardButton: case SP_DialogHelpButton:
            case SP_DialogSaveButton:    case SP_DialogOpenButton:
            case SP_DialogCloseButton:
            case SP_ArrowBack: case SP_ArrowForward:
            case SP_ArrowUp:   case SP_ArrowDown:
            case SP_ArrowLeft: case SP_ArrowRight:
                break;        // monochrome — tint below
            default:
                return base;  // colourful or unknown — leave as-is
        }
        const QPalette pal = w ? w->palette() : QApplication::palette();
        return QIcon(new TintedIconEngine(base, pal.color(QPalette::WindowText),
                                          /*followForeground=*/true));
    }
};
#endif

// Apply `base` as the application style, wrapping it in BreezeIconProxyStyle when
// Breeze is selected so its monochrome standard button icons get recoloured (see
// above). Takes ownership of `base` (QApplication::setStyle / QProxyStyle do).
// A no-op if `base` is null.
inline void setApplicationStyle(QStyle* base, bool isBreeze) {
    if (!base) return;
#if defined(Q_OS_WIN) || defined(HAVE_BREEZE_ICONS)
    if (isBreeze) {
        QApplication::setStyle(new BreezeIconProxyStyle(base));
        return;
    }
#else
    Q_UNUSED(isBreeze);
#endif
    QApplication::setStyle(base);
}
