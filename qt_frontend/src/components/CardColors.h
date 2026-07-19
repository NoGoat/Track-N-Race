#pragma once

#include <QColor>
#include <QHash>
#include <QString>

#include <cmath>

#include <tnrp/CardColors.h>

// Native side of the library-owned card-colour model (tnrp/CardColors.h). The
// per-key specs come straight from libtnrp (in-process); this maps the semantic
// tokens to QColor and evaluates a spec against the current data, so the recorder
// and the Electron app use identical thresholds and intent.

namespace tnr {

// token → QColor. An invalid QColor() means "use the widget default".
inline QColor tokenQColor(const std::string& token) {
    if (token == "pos")            return QColor("#37872D");
    if (token == "neg")            return QColor("#C4162A");
    if (token == "warn")           return QColor("#d4ad04");
    if (token == "warnAlt")        return QColor("#c47d0e");
    if (token == "info")           return QColor("#5794F2");
    if (token == "ice")            return QColor("#5794F2");
    if (token == "mguk")           return QColor("#FADE2A");
    if (token == "fuel")           return QColor("#F0A500");
    if (token == "off")            return QColor("#7a7a7a");
    if (token == "wear1")          return QColor("#73BF69");
    if (token == "wear2")          return QColor("#A8D436");
    if (token == "wear3")          return QColor("#FF9830");
    if (token == "compoundSoft")   return QColor("#e8002d");
    if (token == "compoundMedium") return QColor("#ffd700");
    if (token == "compoundHard")   return QColor();          // default text
    if (token == "compoundInter")  return QColor("#39b54a");
    if (token == "compoundWet")    return QColor("#4488ff");
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
