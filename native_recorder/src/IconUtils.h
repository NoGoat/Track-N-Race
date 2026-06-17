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
#if defined(Q_OS_WIN)
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

// On Windows the bundled Breeze icons are monochrome and won't recolour for dark
// mode (that needs the KDE platform theme, which we don't ship), so they'd render
// black. Wrap the theme icon in an engine that tints it to `tint` while keeping
// it scalable. Everywhere else (KDE recolours them, or no Breeze bundled) the icon
// is returned untouched.
inline QIcon adaptThemeIcon(const QIcon& themed, const QColor& tint, const QIcon& fallback) {
    if (themed.isNull()) return fallback;
#if defined(Q_OS_WIN)
    return QIcon(new TintedIconEngine(themed, tint));
#else
    Q_UNUSED(tint);
    return themed;
#endif
}
