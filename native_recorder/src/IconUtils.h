#pragma once

#include <QIcon>
#include <QIconEngine>
#include <QPainter>
#include <QStyle>
#include <QWidget>

// Recolours a source theme icon to a fixed tint while keeping it fully scalable.
// It renders the source at the exact size + device-pixel-ratio Qt requests (so it
// stays crisp on HiDPI / fractional display scaling) and tints via SourceIn —
// unlike baking a handful of fixed-size pixmaps, which Qt then scales to whatever
// size the toolbar actually wants, producing blur.
#if defined(Q_OS_WIN) || defined(HAVE_BREEZE_ICONS)
class TintedIconEngine : public QIconEngine {
public:
    TintedIconEngine(QIcon src, QColor tint) : src_(std::move(src)), tint_(tint) {}
    QIconEngine* clone() const override { return new TintedIconEngine(src_, tint_); }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state,
                         qreal scale) override {
        QPixmap pm = src_.pixmap(size, scale, mode, state);  // crisp at device res
        if (pm.isNull()) return pm;
        QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QPainter p(&img);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(img.rect(), tint_);
        p.end();
        QPixmap out = QPixmap::fromImage(img);
        out.setDevicePixelRatio(scale);
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
        return QIcon(new TintedIconEngine(themed, tint));
#endif
    Q_UNUSED(tint);
    return themed;
}
