#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>

// Track maps: the track_<id>.json schema every host draws, the maps a user
// generated or imported, and which map a session uses. Hosts bundle the
// built-in maps and register them here; libtnrp owns generating, storing,
// matching and choosing maps so every frontend behaves the same.

namespace tnrp {

// [x, y] in the map's 1000 x 1000 view box.
using MapPoint = std::array<double, 2>;

struct TrackMapZone {
    MapPoint start{};
    MapPoint end{};
};

struct TrackMapSector {
    int index{};
    std::vector<MapPoint> points;
};

// World (x, z) to view box: ((x - min_x) * scale + off_x, (z - min_z) * scale + off_z).
struct TrackMapTransform {
    double min_x{};
    double min_z{};
    double scale{1.0};
    double off_x{};
    double off_z{};
};

struct TrackMapViewBox {
    double width{1000.0};
    double height{1000.0};
};

// One map. The fields after `drs_detection_points` exist only on maps the
// user generated or imported; built-in maps leave them empty.
struct TrackMapData {
    int track_id{-1};
    std::string track_name;
    std::string circuit_name;
    double track_length_m{};
    std::optional<double> pit_time;
    std::optional<double> inlap_pit_time;
    std::optional<double> outlap_pit_time;
    TrackMapViewBox view_box;
    double rotation_deg{};
    TrackMapTransform transform;
    std::vector<TrackMapSector> sectors;
    std::vector<TrackMapZone> drs_zones;
    std::optional<std::vector<TrackMapZone>> slm_dry;
    std::optional<std::vector<TrackMapZone>> slm_wet;
    std::vector<MapPoint> speed_traps;
    std::optional<MapPoint> start_finish;
    std::optional<MapPoint> overtake_detection_point;
    std::optional<MapPoint> overtake_activation_point;
    std::optional<std::vector<MapPoint>> drs_detection_points;

    std::optional<int> map_format;              // kTrackMapFormat
    std::optional<std::string> source;          // "generated" | "imported" | "edited"
    std::optional<std::string> display_name;
    std::optional<std::string> created_from;    // recording file name, or "Live session"
    std::optional<int64_t> created_at;          // Unix ms
    std::optional<int> laps_used;
};

constexpr int kTrackMapFormat = 1;

// Parses a map file; false (with a reason) when it is not a usable map: it
// needs a positive scale and a centerline of at least 16 points.
bool readTrackMap(std::string_view json, TrackMapData& out, std::string* errorOut = nullptr);
std::string writeTrackMap(const TrackMapData& map);

// The map's sectors joined into one closed centerline, in view box units.
std::vector<MapPoint> trackMapCenterline(const TrackMapData& map);

// ── Geometry matching ───────────────────────────────────────────────────────

// World (x, z) positions of cars on track.
using WorldPoint = std::array<double, 2>;

struct TrackMapFit {
    size_t samples{};
    double medianM{-1.0};    // median distance from the centerline, metres
    double withinShare{};    // share of samples within kFitNearM of it
};

constexpr double kFitMedianM = 12.0;
constexpr double kFitNearM = 30.0;
constexpr double kFitWithinShare = 0.8;

TrackMapFit fitTrackMap(const TrackMapData& map, const std::vector<WorldPoint>& samples);
bool fitAccepted(const TrackMapFit& fit);
// Whether samples cover enough of a track to judge a fit: at least 40 of them
// spread over 300 m.
bool enoughFitSamples(const std::vector<WorldPoint>& samples);

// ── Generation ──────────────────────────────────────────────────────────────

// What a generated map is labelled with. A trackLengthM of 0 takes the laps'
// own length.
struct TrackMapGenerateInfo {
    int trackId{-1};
    std::string trackName;
    double trackLengthM{};
    std::string createdFrom;
};

// Builds a map from a TNRD V6 recording: the centerline from every car's
// completed laps outside the pits, sectors from the lap times, and DRS / SLM
// zones from where cars opened them. False with a reason when the recording
// is not V6 or holds no usable lap.
bool generateTrackMapFromRecording(const std::string& path, TrackMapData& out,
                                   std::string* errorOut = nullptr,
                                   const std::function<bool()>& cancelled = {});

// World positions of up to `perDriver` points along one completed lap of each
// car in a V6 recording, for matching. Empty for other formats.
std::vector<WorldPoint> recordingFitSamples(const std::string& path, size_t perDriver = 40);

// ── Library ─────────────────────────────────────────────────────────────────

// What the user can change about a map; unset fields stay as they are.
struct TrackMapEdit {
    std::optional<std::string> display_name;
    std::optional<std::string> track_name;
    std::optional<std::string> circuit_name;
    std::optional<double> rotation_deg;   // the angle hosts turn the map by
};

// Applies an edit to a map, with the rotation wrapped to [0, 360).
void applyTrackMapEdit(TrackMapData& map, const TrackMapEdit& edit);

struct TrackMapEntry {
    std::string id;          // "builtin:<track id>" or "user:<file stem>"
    std::string source;      // "builtin" | "generated" | "imported" | "edited"
    std::string name;
    std::string circuit;
    int trackId{-1};
    double trackLengthM{};
    int64_t createdAt{};
    std::string createdFrom;
    int lapsUsed{};
};

// Which map a session uses, and why. Sent to hosts as a "track_map" row.
struct TrackMapResolution {
    std::string type{"track_map"};
    std::string scope;               // "live" | "playback" | "recording"
    int track_id{-1};
    double track_length_m{};
    std::string track_key;           // what a pin is stored under
    std::string map_id;              // empty: no map
    // "pinned" | "track_id" | "length" | "geometry" | "generated" | "none"
    std::string reason{"none"};
    bool checked{};                  // geometry has been compared
    bool mismatch{};                 // the chosen map does not fit the cars
    double fit_m{-1.0};
    bool generating{};               // a map is being built for this track
};

// The pin key for a track: "id:<track id>", or for a track without an ID
// "len:<length rounded to 10 m>".
std::string trackMapKey(int trackId, double trackLengthM);

// Built-in, generated and imported maps plus the user's per-track choices.
// User maps are JSON files in one directory; choices live in its
// selections.json. Thread-safe.
class TrackMapLibrary {
public:
    // A host's bundled map. Replaces an earlier one with the same track ID.
    bool addBuiltin(std::string_view json, std::string* errorOut = nullptr);
    bool hasBuiltins() const;
    // Where user maps live; created when missing, then rescanned.
    void setUserDirectory(const std::string& directory);
    std::string userDirectory() const;
    void rescan();

    std::vector<TrackMapEntry> entries() const;
    // {"maps":[{id, source, name, circuit, track_id, track_length_m, ...}], "pins":{key: id}}
    std::string catalogJson() const;
    std::optional<TrackMapData> map(const std::string& id) const;
    // The map file, as written; empty when there is no such map.
    std::string mapJson(const std::string& id) const;

    // Writes a generated map and returns its ID, or empty with a reason.
    std::string saveGenerated(TrackMapData map, std::string* errorOut = nullptr);
    // Validates a map file and copies it in; returns its new ID.
    std::string importFile(const std::string& path, std::string* errorOut = nullptr);
    bool exportFile(const std::string& id, const std::string& path, std::string* errorOut = nullptr) const;
    bool rename(const std::string& id, const std::string& name, std::string* errorOut = nullptr);
    // Changes a saved map in place and returns its ID. A built-in map is never
    // changed: the edit is saved as a copy ("edited") and pinned for its
    // track, so choosing Auto brings the original back. Empty with a reason
    // on failure.
    std::string edit(const std::string& id, const TrackMapEdit& edit, std::string* errorOut = nullptr);
    // User maps only. Pins on it are cleared.
    bool remove(const std::string& id, std::string* errorOut = nullptr);

    // Empty mapId clears the pin.
    void pin(const std::string& trackKey, const std::string& mapId);
    std::string pinned(const std::string& trackKey) const;

    // Chooses a map: the pin, else the map for this track ID (built-in first)
    // or length, then, when `samples` are enough to judge, the best fitting
    // map of all if that one does not fit. A pin is kept even when it does
    // not fit; the result says so.
    TrackMapResolution resolve(int trackId, double trackLengthM,
                               const std::vector<WorldPoint>& samples) const;

private:
    struct Stored {
        TrackMapEntry entry;
        TrackMapData data;
        std::string path;   // user maps
    };
    void rescanLocked();
    void savePinsLocked() const;
    std::vector<std::string> candidatesLocked(int trackId, double trackLengthM) const;

    mutable std::mutex mutex_;
    std::map<std::string, Stored> maps_;
    std::string directory_;
    std::map<std::string, std::string> pins_;
};

} // namespace tnrp

template <>
struct glz::meta<tnrp::TrackMapZone> {
    using T = tnrp::TrackMapZone;
    static constexpr auto value = glz::object("start", &T::start, "end", &T::end);
};

template <>
struct glz::meta<tnrp::TrackMapSector> {
    using T = tnrp::TrackMapSector;
    static constexpr auto value = glz::object("index", &T::index, "points", &T::points);
};

template <>
struct glz::meta<tnrp::TrackMapTransform> {
    using T = tnrp::TrackMapTransform;
    static constexpr auto value = glz::object(
        "min_x", &T::min_x, "min_z", &T::min_z, "scale", &T::scale,
        "off_x", &T::off_x, "off_z", &T::off_z);
};

template <>
struct glz::meta<tnrp::TrackMapViewBox> {
    using T = tnrp::TrackMapViewBox;
    static constexpr auto value = glz::object("width", &T::width, "height", &T::height);
};

template <>
struct glz::meta<tnrp::TrackMapData> {
    using T = tnrp::TrackMapData;
    static constexpr auto value = glz::object(
        "track_id",                  &T::track_id,
        "track_name",                &T::track_name,
        "circuit_name",              &T::circuit_name,
        "track_length_m",            &T::track_length_m,
        "pit_time",                  &T::pit_time,
        "inlap_pit_time",            &T::inlap_pit_time,
        "outlap_pit_time",           &T::outlap_pit_time,
        "view_box",                  &T::view_box,
        "rotation_deg",              &T::rotation_deg,
        "transform",                 &T::transform,
        "sectors",                   &T::sectors,
        "drs_zones",                 &T::drs_zones,
        "slm_dry",                   &T::slm_dry,
        "slm_wet",                   &T::slm_wet,
        "speed_traps",               &T::speed_traps,
        "start_finish",              &T::start_finish,
        "overtake_detection_point",  &T::overtake_detection_point,
        "overtake_activation_point", &T::overtake_activation_point,
        "drs_detection_points",      &T::drs_detection_points,
        "map_format",                &T::map_format,
        "source",                    &T::source,
        "display_name",              &T::display_name,
        "created_from",              &T::created_from,
        "created_at",                &T::created_at,
        "laps_used",                 &T::laps_used);
};

template <>
struct glz::meta<tnrp::TrackMapResolution> {
    using T = tnrp::TrackMapResolution;
    static constexpr auto value = glz::object(
        "type",           &T::type,
        "scope",          &T::scope,
        "track_id",       &T::track_id,
        "track_length_m", &T::track_length_m,
        "track_key",      &T::track_key,
        "map_id",         &T::map_id,
        "reason",         &T::reason,
        "checked",        &T::checked,
        "mismatch",       &T::mismatch,
        "fit_m",          &T::fit_m,
        "generating",     &T::generating);
};

template <>
struct glz::meta<tnrp::TrackMapEdit> {
    using T = tnrp::TrackMapEdit;
    static constexpr auto value = glz::object(
        "display_name", &T::display_name,
        "track_name",   &T::track_name,
        "circuit_name", &T::circuit_name,
        "rotation_deg", &T::rotation_deg);
};
