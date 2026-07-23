#include "tnrp/DisplayNames.h"

#include "protocols/protocol.h"

#include <unordered_map>

namespace tnrp {
namespace {

const std::unordered_map<int, std::string_view> kCircuitNames = {
    {0,  "Albert Park Circuit"},
    {2,  "Shanghai International Circuit"},
    {3,  "Bahrain International Circuit"},
    {4,  "Circuit de Barcelona-Catalunya"},
    {5,  "Circuit de Monaco"},
    {6,  "Circuit Gilles Villeneuve"},
    {7,  "Silverstone Circuit"},
    {9,  "Hungaroring"},
    {10, "Circuit de Spa-Francorchamps"},
    {11, "Autodromo Nazionale Monza"},
    {12, "Marina Bay Street Circuit"},
    {13, "Suzuka International Racing Course"},
    {14, "Yas Marina Circuit"},
    {15, "Circuit of the Americas"},
    {16, "Autodromo Jose Carlos Pace"},
    {17, "Red Bull Ring"},
    {19, "Autodromo Hermanos Rodriguez"},
    {20, "Baku City Circuit"},
    {26, "Circuit Zandvoort"},
    {27, "Autodromo Enzo e Dino Ferrari"},
    {29, "Jeddah Corniche Circuit"},
    {30, "Miami International Autodrome"},
    {31, "Las Vegas Street Circuit"},
    {32, "Lusail International Circuit"},
    {39, "Silverstone Circuit (Reverse)"},
    {40, "Red Bull Ring (Reverse)"},
    {41, "Circuit Zandvoort (Reverse)"},
    {42, "Madring"},
};

} // namespace

std::string_view circuitName(int trackId) noexcept {
    const auto it = kCircuitNames.find(trackId);
    return it == kCircuitNames.end() ? std::string_view{"Unknown circuit"} : it->second;
}

std::string_view sessionName(int sessionType) noexcept {
    const auto it = SESSION_NAMES.find(sessionType);
    return it == SESSION_NAMES.end() ? std::string_view{"Unknown session"}
                                     : std::string_view{it->second};
}

} // namespace tnrp
