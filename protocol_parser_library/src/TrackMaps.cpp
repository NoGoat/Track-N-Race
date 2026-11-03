#include "tnrp/TrackMaps.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>

namespace tnrp {

// Read and written with glaze reflection, which MSVC cannot apply to types in
// an anonymous namespace.
struct TrackMapSelectionsFile {
    std::map<std::string, std::string> pins;
};

struct TrackMapCatalogItem {
    std::string id;
    std::string source;
    std::string name;
    std::string circuit;
    int track_id{-1};
    double track_length_m{};
    int64_t created_at{};
    std::string created_from;
    int laps_used{};
};

struct TrackMapCatalogJson {
    std::vector<TrackMapCatalogItem> maps;
    std::map<std::string, std::string> pins;
};

namespace {

namespace fs = std::filesystem;

constexpr glz::opts kMapRead{.null_terminated = false, .error_on_unknown_keys = false};
constexpr char kSelectionsFile[] = "selections.json";
constexpr double kLengthMatchM = 30.0;
// Samples farther than this outside a map's bounds rule it out without
// measuring every one against its centerline.
constexpr double kBoundsMarginM = 60.0;
constexpr double kMinSampleSpreadM = 300.0;
constexpr size_t kMinFitSamples = 40;

fs::path pathFromUtf8(const std::string& text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

std::string pathToUtf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

bool readFile(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return static_cast<bool>(in) || in.eof();
}

bool writeFileAtomically(const fs::path& path, const std::string& text, std::string* errorOut) {
    fs::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (errorOut) *errorOut = "Cannot write " + path.string();
            return false;
        }
        out << text;
        if (!out) {
            if (errorOut) *errorOut = "Cannot write " + path.string();
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(temp, ec);
        if (errorOut) *errorOut = "Cannot write " + path.string();
        return false;
    }
    return true;
}

std::string displayNameOf(const TrackMapData& map) {
    if (map.display_name && !map.display_name->empty()) return *map.display_name;
    if (!map.track_name.empty()) return map.track_name;
    return "Track " + std::to_string(map.track_id);
}

// A file stem from a map name: lowercase letters, digits and dashes.
std::string slug(std::string_view name) {
    std::string out;
    for (const char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) out.push_back(static_cast<char>(std::tolower(u)));
        else if (!out.empty() && out.back() != '-') out.push_back('-');
        if (out.size() >= 48) break;
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "map" : out;
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

double segmentDistance(const MapPoint& p, const MapPoint& a, const MapPoint& b) {
    const double dx = b[0] - a[0];
    const double dy = b[1] - a[1];
    const double length2 = dx * dx + dy * dy;
    double t = length2 > 0.0 ? ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / length2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(p[0] - (a[0] + t * dx), p[1] - (a[1] + t * dy));
}

TrackMapEntry entryFor(const std::string& id, const std::string& source, const TrackMapData& map) {
    TrackMapEntry entry;
    entry.id = id;
    entry.source = source;
    entry.name = displayNameOf(map);
    entry.circuit = map.circuit_name;
    entry.trackId = map.track_id;
    entry.trackLengthM = map.track_length_m;
    entry.createdAt = map.created_at.value_or(0);
    entry.createdFrom = map.created_from.value_or("");
    entry.lapsUsed = map.laps_used.value_or(0);
    return entry;
}

} // namespace

bool readTrackMap(std::string_view json, TrackMapData& out, std::string* errorOut) {
    TrackMapData map;
    if (const auto error = glz::read<kMapRead>(map, json)) {
        if (errorOut) *errorOut = "Not a track map file";
        return false;
    }
    if (!(map.transform.scale > 0.0) || !std::isfinite(map.transform.scale)) {
        if (errorOut) *errorOut = "The map has no valid transform";
        return false;
    }
    if (trackMapCenterline(map).size() < 16) {
        if (errorOut) *errorOut = "The map has no track outline";
        return false;
    }
    out = std::move(map);
    return true;
}

std::string writeTrackMap(const TrackMapData& map) {
    std::string out;
    (void)glz::write<glz::opts{.prettify = true}>(map, out);
    return out;
}

void applyTrackMapEdit(TrackMapData& map, const TrackMapEdit& edit) {
    if (edit.display_name) map.display_name = *edit.display_name;
    if (edit.track_name) map.track_name = *edit.track_name;
    if (edit.circuit_name) map.circuit_name = *edit.circuit_name;
    if (edit.rotation_deg && std::isfinite(*edit.rotation_deg)) {
        double degrees = std::fmod(*edit.rotation_deg, 360.0);
        if (degrees < 0.0) degrees += 360.0;
        map.rotation_deg = std::round(degrees * 10.0) / 10.0;
    }
}

std::vector<MapPoint> trackMapCenterline(const TrackMapData& map) {
    std::vector<const TrackMapSector*> ordered;
    for (const auto& sector : map.sectors) ordered.push_back(&sector);
    std::stable_sort(ordered.begin(), ordered.end(),
        [](const TrackMapSector* a, const TrackMapSector* b) { return a->index < b->index; });
    std::vector<MapPoint> points;
    for (const auto* sector : ordered) {
        for (const auto& point : sector->points) {
            if (!std::isfinite(point[0]) || !std::isfinite(point[1])) continue;
            if (points.empty() || points.back() != point) points.push_back(point);
        }
    }
    if (points.size() > 1 && points.front() == points.back()) points.pop_back();
    return points;
}

// ── Geometry matching ───────────────────────────────────────────────────────

TrackMapFit fitTrackMap(const TrackMapData& map, const std::vector<WorldPoint>& samples) {
    TrackMapFit fit;
    fit.samples = samples.size();
    const auto centerline = trackMapCenterline(map);
    const double scale = map.transform.scale;
    if (samples.empty() || centerline.size() < 2 || !(scale > 0.0)) return fit;

    double minX = std::numeric_limits<double>::infinity(), maxX = -minX;
    double minY = minX, maxY = -minX;
    for (const auto& p : centerline) {
        minX = std::min(minX, p[0]); maxX = std::max(maxX, p[0]);
        minY = std::min(minY, p[1]); maxY = std::max(maxY, p[1]);
    }
    const double margin = kBoundsMarginM * scale;
    std::vector<MapPoint> view;
    view.reserve(samples.size());
    size_t inside = 0;
    for (const auto& s : samples) {
        const MapPoint v{(s[0] - map.transform.min_x) * scale + map.transform.off_x,
                         (s[1] - map.transform.min_z) * scale + map.transform.off_z};
        if (v[0] >= minX - margin && v[0] <= maxX + margin &&
            v[1] >= minY - margin && v[1] <= maxY + margin) ++inside;
        view.push_back(v);
    }
    // Most cars outside the map's bounds: a different track. Report it as
    // far off without measuring.
    if (static_cast<double>(inside) < kFitWithinShare * static_cast<double>(samples.size())) {
        fit.medianM = std::numeric_limits<double>::infinity();
        fit.withinShare = static_cast<double>(inside) / static_cast<double>(samples.size());
        return fit;
    }

    std::vector<double> distances;
    distances.reserve(view.size());
    size_t nearCount = 0;
    for (const auto& v : view) {
        double best = std::numeric_limits<double>::infinity();
        for (size_t i = 0; i < centerline.size(); ++i)
            best = std::min(best, segmentDistance(v, centerline[i],
                                                  centerline[(i + 1) % centerline.size()]));
        const double metres = best / scale;
        if (metres <= kFitNearM) ++nearCount;
        distances.push_back(metres);
    }
    const auto middle = distances.begin() + static_cast<std::ptrdiff_t>(distances.size() / 2);
    std::nth_element(distances.begin(), middle, distances.end());
    fit.medianM = *middle;
    fit.withinShare = static_cast<double>(nearCount) / static_cast<double>(distances.size());
    return fit;
}

bool fitAccepted(const TrackMapFit& fit) {
    return fit.samples >= kMinFitSamples && fit.medianM >= 0.0 &&
           fit.medianM <= kFitMedianM && fit.withinShare >= kFitWithinShare;
}

bool enoughFitSamples(const std::vector<WorldPoint>& samples) {
    if (samples.size() < kMinFitSamples) return false;
    double minX = std::numeric_limits<double>::infinity(), maxX = -minX;
    double minZ = minX, maxZ = -minX;
    for (const auto& s : samples) {
        minX = std::min(minX, s[0]); maxX = std::max(maxX, s[0]);
        minZ = std::min(minZ, s[1]); maxZ = std::max(maxZ, s[1]);
    }
    return std::hypot(maxX - minX, maxZ - minZ) >= kMinSampleSpreadM;
}

std::string trackMapKey(int trackId, double trackLengthM) {
    if (trackId >= 0) return "id:" + std::to_string(trackId);
    const long rounded = std::lround(std::max(0.0, trackLengthM) / 10.0) * 10;
    return "len:" + std::to_string(rounded);
}

// ── Library ─────────────────────────────────────────────────────────────────

bool TrackMapLibrary::addBuiltin(std::string_view json, std::string* errorOut) {
    TrackMapData map;
    if (!readTrackMap(json, map, errorOut)) return false;
    if (map.track_id < 0) {
        if (errorOut) *errorOut = "A built-in map needs a track ID";
        return false;
    }
    std::lock_guard lock(mutex_);
    const std::string id = "builtin:" + std::to_string(map.track_id);
    Stored stored;
    stored.entry = entryFor(id, "builtin", map);
    stored.data = std::move(map);
    maps_[id] = std::move(stored);
    return true;
}

bool TrackMapLibrary::hasBuiltins() const {
    std::lock_guard lock(mutex_);
    return std::any_of(maps_.begin(), maps_.end(),
        [](const auto& item) { return item.second.entry.source == "builtin"; });
}

void TrackMapLibrary::setUserDirectory(const std::string& directory) {
    std::lock_guard lock(mutex_);
    directory_ = directory;
    std::error_code ec;
    if (!directory_.empty()) fs::create_directories(pathFromUtf8(directory_), ec);
    rescanLocked();
}

std::string TrackMapLibrary::userDirectory() const {
    std::lock_guard lock(mutex_);
    return directory_;
}

void TrackMapLibrary::rescan() {
    std::lock_guard lock(mutex_);
    rescanLocked();
}

void TrackMapLibrary::rescanLocked() {
    for (auto it = maps_.begin(); it != maps_.end();) {
        if (it->second.entry.source != "builtin") it = maps_.erase(it);
        else ++it;
    }
    pins_.clear();
    if (directory_.empty()) return;
    const fs::path root = pathFromUtf8(directory_);
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path path = it->path();
        if (!it->is_regular_file(ec) || path.extension() != ".json") continue;
        if (pathToUtf8(path.filename()) == kSelectionsFile) continue;
        std::string text;
        TrackMapData map;
        if (!readFile(path, text) || !readTrackMap(text, map)) continue;
        const std::string id = "user:" + pathToUtf8(path.stem());
        Stored stored;
        stored.entry = entryFor(id, map.source.value_or("imported"), map);
        stored.data = std::move(map);
        stored.path = pathToUtf8(path);
        maps_[id] = std::move(stored);
    }
    std::string text;
    TrackMapSelectionsFile selections;
    if (readFile(root / kSelectionsFile, text) &&
        !glz::read<kMapRead>(selections, text)) {
        for (auto& [key, id] : selections.pins)
            if (maps_.count(id)) pins_[key] = id;
    }
}

void TrackMapLibrary::savePinsLocked() const {
    if (directory_.empty()) return;
    TrackMapSelectionsFile selections{pins_};
    std::string text;
    (void)glz::write<glz::opts{.prettify = true}>(selections, text);
    (void)writeFileAtomically(pathFromUtf8(directory_) / kSelectionsFile, text, nullptr);
}

std::vector<TrackMapEntry> TrackMapLibrary::entries() const {
    std::lock_guard lock(mutex_);
    std::vector<TrackMapEntry> out;
    out.reserve(maps_.size());
    for (const auto& [id, stored] : maps_) out.push_back(stored.entry);
    std::stable_sort(out.begin(), out.end(), [](const TrackMapEntry& a, const TrackMapEntry& b) {
        return a.name < b.name;
    });
    return out;
}

std::string TrackMapLibrary::catalogJson() const {
    TrackMapCatalogJson catalog;
    for (const auto& entry : entries()) {
        catalog.maps.push_back({entry.id, entry.source, entry.name, entry.circuit, entry.trackId,
                                entry.trackLengthM, entry.createdAt, entry.createdFrom, entry.lapsUsed});
    }
    {
        std::lock_guard lock(mutex_);
        catalog.pins = pins_;
    }
    std::string out;
    (void)glz::write_json(catalog, out);
    return out;
}

std::optional<TrackMapData> TrackMapLibrary::map(const std::string& id) const {
    std::lock_guard lock(mutex_);
    const auto it = maps_.find(id);
    if (it == maps_.end()) return std::nullopt;
    return it->second.data;
}

std::string TrackMapLibrary::mapJson(const std::string& id) const {
    const auto found = map(id);
    return found ? writeTrackMap(*found) : std::string{};
}

std::string TrackMapLibrary::saveGenerated(TrackMapData map, std::string* errorOut) {
    std::lock_guard lock(mutex_);
    if (directory_.empty()) {
        if (errorOut) *errorOut = "No folder for saved maps";
        return {};
    }
    map.map_format = kTrackMapFormat;
    if (!map.source) map.source = "generated";
    if (!map.created_at) map.created_at = nowMs();
    const fs::path root = pathFromUtf8(directory_);
    std::error_code ec;
    fs::create_directories(root, ec);
    std::string stem = slug(displayNameOf(map)) + "-" + std::to_string(*map.created_at);
    for (int n = 2; fs::exists(root / (stem + ".json"), ec); ++n)
        stem = slug(displayNameOf(map)) + "-" + std::to_string(*map.created_at) + "-" + std::to_string(n);
    const fs::path path = root / (stem + ".json");
    if (!writeFileAtomically(path, writeTrackMap(map), errorOut)) return {};
    const std::string id = "user:" + stem;
    Stored stored;
    stored.entry = entryFor(id, *map.source, map);
    stored.data = std::move(map);
    stored.path = pathToUtf8(path);
    maps_[id] = std::move(stored);
    return id;
}

std::string TrackMapLibrary::importFile(const std::string& path, std::string* errorOut) {
    std::string text;
    if (!readFile(pathFromUtf8(path), text)) {
        if (errorOut) *errorOut = "Cannot read " + path;
        return {};
    }
    TrackMapData map;
    if (!readTrackMap(text, map, errorOut)) return {};
    if (!map.display_name || map.display_name->empty())
        map.display_name = pathToUtf8(pathFromUtf8(path).stem());
    map.source = "imported";
    map.created_at = nowMs();
    return saveGenerated(std::move(map), errorOut);
}

bool TrackMapLibrary::exportFile(const std::string& id, const std::string& path,
                                 std::string* errorOut) const {
    const auto found = map(id);
    if (!found) {
        if (errorOut) *errorOut = "No such map";
        return false;
    }
    return writeFileAtomically(pathFromUtf8(path), writeTrackMap(*found), errorOut);
}

bool TrackMapLibrary::rename(const std::string& id, const std::string& name, std::string* errorOut) {
    std::lock_guard lock(mutex_);
    const auto it = maps_.find(id);
    if (it == maps_.end() || it->second.path.empty()) {
        if (errorOut) *errorOut = "Only saved maps can be renamed";
        return false;
    }
    TrackMapData updated = it->second.data;
    updated.display_name = name;
    if (!writeFileAtomically(pathFromUtf8(it->second.path), writeTrackMap(updated), errorOut)) return false;
    it->second.data = std::move(updated);
    it->second.entry.name = displayNameOf(it->second.data);
    return true;
}

std::string TrackMapLibrary::edit(const std::string& id, const TrackMapEdit& edit, std::string* errorOut) {
    TrackMapData copy;
    {
        std::lock_guard lock(mutex_);
        const auto it = maps_.find(id);
        if (it == maps_.end()) {
            if (errorOut) *errorOut = "No such map";
            return {};
        }
        if (!it->second.path.empty()) {
            TrackMapData updated = it->second.data;
            applyTrackMapEdit(updated, edit);
            if (!writeFileAtomically(pathFromUtf8(it->second.path), writeTrackMap(updated), errorOut)) return {};
            it->second.data = std::move(updated);
            it->second.entry = entryFor(id, it->second.entry.source, it->second.data);
            return id;
        }
        copy = it->second.data;
    }
    // A built-in map: save the edited copy and use it for this track.
    applyTrackMapEdit(copy, edit);
    if (!copy.display_name || copy.display_name->empty()) copy.display_name = displayNameOf(copy);
    copy.source = "edited";
    copy.created_at = nowMs();
    copy.created_from = "Built-in map";
    const std::string key = trackMapKey(copy.track_id, copy.track_length_m);
    const std::string saved = saveGenerated(std::move(copy), errorOut);
    if (!saved.empty()) pin(key, saved);
    return saved;
}

bool TrackMapLibrary::remove(const std::string& id, std::string* errorOut) {
    std::lock_guard lock(mutex_);
    const auto it = maps_.find(id);
    if (it == maps_.end() || it->second.path.empty()) {
        if (errorOut) *errorOut = "Only saved maps can be deleted";
        return false;
    }
    std::error_code ec;
    fs::remove(pathFromUtf8(it->second.path), ec);
    if (ec) {
        if (errorOut) *errorOut = "Cannot delete " + it->second.path;
        return false;
    }
    maps_.erase(it);
    bool pinsChanged = false;
    for (auto pin = pins_.begin(); pin != pins_.end();) {
        if (pin->second == id) { pin = pins_.erase(pin); pinsChanged = true; }
        else ++pin;
    }
    if (pinsChanged) savePinsLocked();
    return true;
}

void TrackMapLibrary::pin(const std::string& trackKey, const std::string& mapId) {
    std::lock_guard lock(mutex_);
    if (mapId.empty()) pins_.erase(trackKey);
    else if (maps_.count(mapId)) pins_[trackKey] = mapId;
    else return;
    savePinsLocked();
}

std::string TrackMapLibrary::pinned(const std::string& trackKey) const {
    std::lock_guard lock(mutex_);
    const auto it = pins_.find(trackKey);
    return it == pins_.end() ? std::string{} : it->second;
}

std::vector<std::string> TrackMapLibrary::candidatesLocked(int trackId, double trackLengthM) const {
    std::vector<std::string> out;
    std::vector<const Stored*> user;
    if (trackId >= 0) {
        const std::string builtin = "builtin:" + std::to_string(trackId);
        if (maps_.count(builtin)) out.push_back(builtin);
        for (const auto& [id, stored] : maps_)
            if (stored.entry.source != "builtin" && stored.entry.trackId == trackId) user.push_back(&stored);
    } else {
        for (const auto& [id, stored] : maps_)
            if (stored.entry.source != "builtin" && stored.entry.trackId < 0 && trackLengthM > 0.0 &&
                std::abs(stored.entry.trackLengthM - trackLengthM) <= kLengthMatchM) user.push_back(&stored);
    }
    std::stable_sort(user.begin(), user.end(), [](const Stored* a, const Stored* b) {
        return a->entry.createdAt > b->entry.createdAt;
    });
    for (const auto* stored : user) out.push_back(stored->entry.id);
    return out;
}

TrackMapResolution TrackMapLibrary::resolve(int trackId, double trackLengthM,
                                            const std::vector<WorldPoint>& samples) const {
    std::lock_guard lock(mutex_);
    TrackMapResolution result;
    result.track_id = trackId;
    result.track_length_m = trackLengthM;
    result.track_key = trackMapKey(trackId, trackLengthM);

    bool pinnedChoice = false;
    if (const auto pin = pins_.find(result.track_key); pin != pins_.end() && maps_.count(pin->second)) {
        result.map_id = pin->second;
        result.reason = "pinned";
        pinnedChoice = true;
    } else if (const auto candidates = candidatesLocked(trackId, trackLengthM); !candidates.empty()) {
        result.map_id = candidates.front();
        result.reason = trackId >= 0 ? "track_id" : "length";
    }
    if (!enoughFitSamples(samples)) return result;
    result.checked = true;

    if (!result.map_id.empty()) {
        const TrackMapFit fit = fitTrackMap(maps_.at(result.map_id).data, samples);
        result.fit_m = std::isfinite(fit.medianM) ? fit.medianM : -1.0;
        if (fitAccepted(fit)) return result;
        if (pinnedChoice) {
            result.mismatch = true;
            return result;
        }
    }

    // The map for this track does not fit (or there is none): a modded
    // track. Take whichever map fits best.
    std::string bestId;
    TrackMapFit best;
    for (const auto& [id, stored] : maps_) {
        if (id == result.map_id) continue;
        const TrackMapFit fit = fitTrackMap(stored.data, samples);
        if (fitAccepted(fit) && (bestId.empty() || fit.medianM < best.medianM)) {
            bestId = id;
            best = fit;
        }
    }
    if (!bestId.empty()) {
        result.map_id = bestId;
        result.reason = "geometry";
        result.fit_m = best.medianM;
        result.mismatch = false;
    } else {
        result.mismatch = !result.map_id.empty();
    }
    return result;
}

} // namespace tnrp
