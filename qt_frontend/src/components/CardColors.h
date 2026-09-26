#pragma once

#include <QApplication>
#include <QColor>
#include <QHash>
#include <QPalette>
#include <QString>

#include <cmath>

#include <tnrp/CardColors.h>

// Native side of the library-owned card-colour model (tnrp/CardColors.h). The
// per-key specs come straight from libtnrp (in-process); this maps the semantic
// tokens to QColor and evaluates a spec against the current data, so the recorder
// and the Electron app use identical thresholds and intent.

namespace tnr {

// Electron picks a separate, darker shade of every data colour on its light
// theme so it stays readable on a pale background. Mirror that by deriving the
// theme from the active palette.
inline bool isDarkTheme() {
    return QApplication::palette().color(QPalette::Window).lightness() < 128;
}

// Pick the Electron dark- or light-theme variant of a colour.
inline QColor themed(const char* dark, const char* light) {
    return QColor(isDarkTheme() ? dark : light);
}

// Compound colours (Electron's --compound-* CSS variables, dark / light).
inline QColor compoundSoftColor()   { return themed("#e8002d", "#c8001a"); }
inline QColor compoundMediumColor() { return themed("#ffd700", "#765900"); }
inline QColor compoundHardColor()   { return themed("#c8c8c8", "#555555"); }
inline QColor compoundInterColor()  { return themed("#39b54a", "#1e7a2e"); }
inline QColor compoundWetColor()    { return themed("#4488ff", "#1a55bb"); }

// token → QColor. An invalid QColor() means "use the widget default".
inline QColor tokenQColor(const std::string& token) {
    if (token == "pos")            return themed("#37872D", "#137333");
    if (token == "neg")            return QColor("#C4162A");
    if (token == "warn")           return themed("#d4ad04", "#8B5200");
    if (token == "warnAlt")        return themed("#c47d0e", "#A04300");
    if (token == "info")           return themed("#5794F2", "#0B57D0");
    if (token == "ice")            return themed("#5794F2", "#0B57D0");
    if (token == "mguk")           return themed("#FADE2A", "#765900");
    if (token == "fuel")           return themed("#F0A500", "#A04300");
    if (token == "off")            return themed("#7a7a7a", "#565B70");
    if (token == "wear1")          return themed("#73BF69", "#137333");
    if (token == "wear2")          return themed("#A8D436", "#5F7418");
    if (token == "wear3")          return themed("#FF9830", "#A04300");
    if (token == "compoundSoft")   return compoundSoftColor();
    if (token == "compoundMedium") return compoundMediumColor();
    if (token == "compoundHard")   return compoundHardColor();
    if (token == "compoundInter")  return compoundInterColor();
    if (token == "compoundWet")    return compoundWetColor();
    return QColor();   // neutral / unknown → default
}

inline bool cardCmp(const std::string& op, double lhs, double rhs) {
    if (op == "lt")  return lhs <  rhs;
    if (op == "lte") return lhs <= rhs;
    if (op == "gt")  return lhs >  rhs;
    if (op == "gte") return lhs >= rhs;
    if (op == "eq")  return lhs == rhs;
    return false;
}

// Evaluate the spec for `specKey`: first satisfied rule wins; `self` is the card's
// own value, other fields come from `fields`. Returns an (in)valid QColor.
inline QColor cardColor(const std::string& specKey, double self = NAN,
                        const QHash<QString, double>& fields = {}) {
    const auto& specs = tnrp::cardColors();
    auto it = specs.find(specKey);
    if (it == specs.end()) return QColor();
    for (const tnrp::ColorRule& r : it->second.rules) {
        const double lhs = (r.on == "self")
            ? self
            : fields.value(QString::fromStdString(r.on), NAN);
        if (std::isnan(lhs)) continue;
        if (cardCmp(r.op, lhs, r.value)) return tokenQColor(r.color);
    }
    return tokenQColor(it->second.def);
}

// Convenience: a "color: #rrggbb; font-weight: bold;" stylesheet, or just bold
// when the colour is the default. Matches the recorder's existing card styling.
inline QString cardColorStyle(const QColor& c) {
    return c.isValid()
        ? QString("color: %1; font-weight: bold;").arg(c.name())
        : QString("font-weight: bold;");
}

} // namespace tnr
