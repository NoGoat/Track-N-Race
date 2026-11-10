#include "TrackMapBuilder.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <numbers>

#include "tnrp/DisplayNames.h"
#include "tnrd/TNRD_V6.h"

namespace tnrp::detail {

// Read with glaze reflection, which MSVC cannot apply to types in an
// anonymous namespace.
struct TrackMapAeroFields {
    std::string type;
    std::optional<int> active_aero_track_status;
};

namespace {

constexpr size_t kMaxLaps = 24;            // laps averaged into the centerline
constexpr size_t kMaxLapAttempts = 96;     // laps read looking for them
constexpr size_t kMinLapSamples = 100;
constexpr double kMaxStepM = 60.0;         // a longer jump between samples is a teleport
constexpr double kMaxClosureM = 60.0;      // lap start to lap end
constexpr double kMinZoneM = 80.0;
constexpr double kZoneShare = 0.25;        // of the most-used stretch
constexpr float kAeroLookbackS = 600.0f;   // edge-encoded state before a lap starts
constexpr double kViewSize = 1000.0;
constexpr double kViewFill = 900.0;

struct Trace {
    uint8_t driver{};
    float start{};
    float end{};
    uint32_t s1Ms{};
    uint32_t s2Ms{};
    std::vector<float> t;
    std::vector<double> x;
    std::vector<double> z;
};

// Resampled at even distances along the lap.
struct Resampled {
    std::vector<double> x;
    std::vector<double> z;
    std::vector<float> t;
};

struct StatusChange {
    float time{};
    int status{};
};

constexpr glz::opts kPartialRead{.null_terminated = false, .error_on_unknown_keys = false};

bool positions(TnrdV6Archive& archive, uint8_t driver, float from, float to,
               std::vector<float>& t, std::vector<double>& x, std::vector<double>& z) {
    std::vector<std::pair<float, double>> xs, zs;
    if (!archive.fieldSamples(driver, V6DataType::Position, "x", from, to, xs, nullptr) ||
        !archive.fieldSamples(driver, V6DataType::Position, "z", from, to, zs, nullptr)) return false;
    t.clear(); x.clear(); z.clear();
    size_t j = 0;
    for (const auto& [time, value] : xs) {
        while (j < zs.size() && zs[j].first < time) ++j;
        if (j < zs.size() && zs[j].first == time) {
            t.push_back(time);
            x.push_back(value);
            z.push_back(zs[j].second);
            ++j;
        }
    }
    return true;
}

// The value an edge-encoded field holds at `time`.
int stateAt(const std::vector<std::pair<float, double>>& edges, float time) {
    const auto it = std::upper_bound(edges.begin(), edges.end(), time,
        [](float value, const auto& edge) { return value < edge.first; });
    return it == edges.begin() ? 0 : static_cast<int>(std::prev(it)->second);
}

int statusAt(const std::vector<StatusChange>& timeline, float time) {
    int status = 0;
    for (const auto& change : timeline) {
        if (change.time > time) break;
        status = change.status;
    }
    return status;
}

std::vector<StatusChange> aeroTimeline(const TnrdV6Archive& archive) {
    std::vector<const V6SharedRecord*> race;
    for (const auto& record : archive.sharedRecords())
        if (record.phase == V6Phase::Race) race.push_back(&record);
    std::stable_sort(race.begin(), race.end(),
        [](const auto* a, const auto* b) { return a->sessionTime < b->sessionTime; });
    std::vector<StatusChange> timeline;
    for (const auto* record : race) {
        TrackMapAeroFields fields;
        if (glz::read<kPartialRead>(fields, record->json) || fields.type != "session" ||
            !fields.active_aero_track_status || *fields.active_aero_track_status < 0) continue;
        const int status = *fields.active_aero_track_status == 1 ? 1 : 0;
        if (timeline.empty() || timeline.back().status != status)
            timeline.push_back({record->sessionTime, status});
    }
    return timeline;
}

double arcLength(const Trace& trace) {
    double total = 0.0;
    for (size_t i = 1; i < trace.x.size(); ++i)
        total += std::hypot(trace.x[i] - trace.x[i - 1], trace.z[i] - trace.z[i - 1]);
    return total;
}

bool usable(const Trace& trace, double trackLengthM) {
    if (trace.t.size() < kMinLapSamples) return false;
    for (size_t i = 1; i < trace.x.size(); ++i)
        if (std::hypot(trace.x[i] - trace.x[i - 1], trace.z[i] - trace.z[i - 1]) > kMaxStepM) return false;
    if (std::hypot(trace.x.front() - trace.x.back(), trace.z.front() - trace.z.back()) > kMaxClosureM)
        return false;
    if (trackLengthM > 0.0) {
        const double length = arcLength(trace);
        if (length < 0.8 * trackLengthM || length > 1.25 * trackLengthM) return false;
    }
    return true;
}

Resampled resample(const Trace& trace, size_t count) {
    std::vector<double> distance(trace.x.size(), 0.0);
    for (size_t i = 1; i < trace.x.size(); ++i)
        distance[i] = distance[i - 1] +
            std::hypot(trace.x[i] - trace.x[i - 1], trace.z[i] - trace.z[i - 1]);
    // The lap closes back on its first point.
    const double closing = std::hypot(trace.x.front() - trace.x.back(), trace.z.front() - trace.z.back());
    const double total = distance.back() + closing;
    Resampled out;
    out.x.resize(count); out.z.resize(count); out.t.resize(count);
    size_t k = 0;
    for (size_t i = 0; i < count; ++i) {
        const double target = total * static_cast<double>(i) / static_cast<double>(count);
        while (k + 1 < distance.size() && distance[k + 1] < target) ++k;
        if (k + 1 >= distance.size()) {
            // On the closing stretch from the last sample back to the first.
            const double f = closing > 0.0 ? (target - distance.back()) / closing : 0.0;
            out.x[i] = trace.x.back() + (trace.x.front() - trace.x.back()) * f;
            out.z[i] = trace.z.back() + (trace.z.front() - trace.z.back()) * f;
            out.t[i] = trace.t.back() + static_cast<float>((trace.end - trace.t.back()) * f);
            continue;
        }
        const double span = distance[k + 1] - distance[k];
        const double f = span > 0.0 ? (target - distance[k]) / span : 0.0;
        out.x[i] = trace.x[k] + (trace.x[k + 1] - trace.x[k]) * f;
        out.z[i] = trace.z[k] + (trace.z[k + 1] - trace.z[k]) * f;
        out.t[i] = trace.t[k] + static_cast<float>((trace.t[k + 1] - trace.t[k]) * f);
    }
    return out;
}

double median(std::vector<double>& values) {
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

// Runs of the closed lap where coverage reaches the threshold, as
// [first, last] index pairs.
std::vector<std::pair<size_t, size_t>> zones(const std::vector<int>& coverage, double stepM) {
    std::vector<std::pair<size_t, size_t>> out;
    const size_t n = coverage.size();
    const int peak = n ? *std::max_element(coverage.begin(), coverage.end()) : 0;
    if (peak <= 0) return out;
    const int threshold = std::max(1, static_cast<int>(std::ceil(kZoneShare * peak)));
    // Start scanning just after a point below the threshold so a zone across
    // the line stays whole.
    size_t origin = n;
    for (size_t i = 0; i < n; ++i) if (coverage[i] < threshold) { origin = i; break; }
    if (origin == n) return out;
    const size_t minLength = static_cast<size_t>(std::ceil(kMinZoneM / std::max(stepM, 0.1)));
    size_t runStart = 0, runLength = 0;
    for (size_t k = 1; k <= n; ++k) {
        const size_t i = (origin + k) % n;
        const bool on = k < n && coverage[i] >= threshold;
        if (on) {
            if (runLength == 0) runStart = i;
            ++runLength;
        } else if (runLength > 0) {
            if (runLength >= minLength) out.emplace_back(runStart, (runStart + runLength - 1) % n);
            runLength = 0;
        }
    }
    return out;
}

// The rotation (degrees) the hosts apply that makes the track's bounding box
// smallest, turned to landscape.
double bestRotation(const std::vector<double>& x, const std::vector<double>& z) {
    double bestAngle = 0.0, bestArea = std::numeric_limits<double>::infinity();
    double bestWidth = 0.0, bestHeight = 0.0;
    for (int degrees = 0; degrees < 180; ++degrees) {
        const double r = degrees * std::numbers::pi / 180.0;
        const double c = std::cos(r), s = std::sin(r);
        double minX = std::numeric_limits<double>::infinity(), maxX = -minX, minY = minX, maxY = -minX;
        for (size_t i = 0; i < x.size(); ++i) {
            const double rx = c * x[i] - s * z[i];
            const double ry = s * x[i] + c * z[i];
            minX = std::min(minX, rx); maxX = std::max(maxX, rx);
            minY = std::min(minY, ry); maxY = std::max(maxY, ry);
        }
        const double area = (maxX - minX) * (maxY - minY);
        if (area < bestArea) {
            bestArea = area; bestAngle = degrees;
            bestWidth = maxX - minX; bestHeight = maxY - minY;
        }
    }
    if (bestHeight > bestWidth) bestAngle += 90.0;
    return std::fmod(bestAngle, 360.0);
}

} // namespace

bool buildTrackMap(TnrdV6Archive& archive, const TrackMapGenerateInfo& info, TrackMapData& out,
                   std::string* errorOut, const std::function<bool()>& cancelled) {
    const auto fail = [&](const char* message) {
        if (errorOut) *errorOut = message;
        return false;
    };
    if (!archive.isOpen()) return fail("The recording is not open");

    struct Candidate { uint8_t driver; V6LapSummary lap; };
    std::vector<Candidate> candidates;
    for (const auto& header : archive.driverHeaders()) {
        for (const auto& lap : archive.driverLapSummaries(header.vehicleIndex)) {
            if (lap.phase != V6Phase::Race || !lap.isCompleted || lap.isPartial ||
                lap.lapTimeMs == 0 || lap.lapNumber == 0 || !(lap.endSessionTime > lap.startSessionTime))
                continue;
            candidates.push_back({header.vehicleIndex, lap});
        }
    }
    if (candidates.empty()) return fail("No completed laps yet");
    // Clean, quick laps hold the tidiest line.
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.lap.isValid != b.lap.isValid) return a.lap.isValid;
        return a.lap.lapTimeMs < b.lap.lapTimeMs;
    });

    const double headerLength = info.trackLengthM > 0.0 ? info.trackLengthM : 0.0;
    std::vector<Trace> traces;
    size_t attempts = 0;
    for (const auto& candidate : candidates) {
        if (traces.size() >= kMaxLaps || attempts >= kMaxLapAttempts) break;
        if (cancelled && cancelled()) return fail("Cancelled");
        ++attempts;
        const float from = candidate.lap.startSessionTime;
        const float to = candidate.lap.endSessionTime;
        std::vector<std::pair<float, double>> pit;
        if (!archive.fieldSamples(candidate.driver, V6DataType::LapTiming, "pit_status", from, to, pit, nullptr))
            continue;
        if (std::any_of(pit.begin(), pit.end(), [](const auto& s) { return s.second != 0.0; })) continue;
        Trace trace;
        trace.driver = candidate.driver;
        trace.start = from;
        trace.end = to;
        trace.s1Ms = candidate.lap.s1Ms;
        trace.s2Ms = candidate.lap.s2Ms;
        if (!positions(archive, candidate.driver, from, to, trace.t, trace.x, trace.z)) continue;
        if (!usable(trace, headerLength)) continue;
        traces.push_back(std::move(trace));
    }
    if (traces.empty()) return fail("No clean lap found");

    double lengthM = headerLength;
    if (!(lengthM > 0.0)) {
        std::vector<double> lengths;
        for (const auto& trace : traces) lengths.push_back(arcLength(trace));
        lengthM = median(lengths);
    }
    const size_t count = std::clamp<size_t>(static_cast<size_t>(lengthM / 2.0), 400, 4000);
    std::vector<Resampled> laps;
    for (const auto& trace : traces) laps.push_back(resample(trace, count));

    // The centerline: the median position across laps at each point.
    std::vector<double> cx(count), cz(count);
    {
        std::vector<double> xs, zs;
        for (size_t i = 0; i < count; ++i) {
            xs.clear(); zs.clear();
            for (const auto& lap : laps) { xs.push_back(lap.x[i]); zs.push_back(lap.z[i]); }
            cx[i] = median(xs);
            cz[i] = median(zs);
        }
    }
    {
        // Light smoothing of what is left of the cars' jitter.
        std::vector<double> sx(count), sz(count);
        constexpr int radius = 2;
        for (size_t i = 0; i < count; ++i) {
            double ax = 0.0, az = 0.0;
            for (int d = -radius; d <= radius; ++d) {
                const size_t j = static_cast<size_t>(
                    static_cast<long long>(i + count) + d) % count;
                ax += cx[j]; az += cz[j];
            }
            sx[i] = ax / (2 * radius + 1);
            sz[i] = az / (2 * radius + 1);
        }
        cx.swap(sx); cz.swap(sz);
    }

    // Sector boundaries: where each lap was when its sector times ran out.
    std::vector<double> s1Index, s2Index;
    for (size_t l = 0; l < laps.size(); ++l) {
        const auto& trace = traces[l];
        if (trace.s1Ms == 0) continue;
        const float t1 = trace.start + static_cast<float>(trace.s1Ms) / 1000.0f;
        const auto first = std::lower_bound(laps[l].t.begin(), laps[l].t.end(), t1);
        if (first == laps[l].t.end()) continue;
        s1Index.push_back(static_cast<double>(first - laps[l].t.begin()));
        if (trace.s2Ms == 0) continue;
        const float t2 = t1 + static_cast<float>(trace.s2Ms) / 1000.0f;
        const auto second = std::lower_bound(laps[l].t.begin(), laps[l].t.end(), t2);
        if (second != laps[l].t.end()) s2Index.push_back(static_cast<double>(second - laps[l].t.begin()));
    }
    size_t i1 = s1Index.empty() ? count / 3 : static_cast<size_t>(median(s1Index));
    size_t i2 = s2Index.empty() ? 2 * count / 3 : static_cast<size_t>(median(s2Index));
    i1 = std::clamp<size_t>(i1, 1, count - 3);
    i2 = std::clamp<size_t>(i2, i1 + 1, count - 2);

    // DRS and SLM zones: where the cars had them open.
    const auto timeline = aeroTimeline(archive);
    std::vector<int> drs(count, 0), slmDry(count, 0), slmWet(count, 0);
    for (size_t l = 0; l < laps.size(); ++l) {
        const auto& trace = traces[l];
        std::vector<std::pair<float, double>> drsEdges, slmEdges;
        (void)archive.fieldSamples(trace.driver, V6DataType::Aero, "drs",
                                   trace.start - kAeroLookbackS, trace.end, drsEdges, nullptr);
        (void)archive.fieldSamples(trace.driver, V6DataType::Aero, "slm",
                                   trace.start - kAeroLookbackS, trace.end, slmEdges, nullptr);
        for (size_t i = 0; i < count; ++i) {
            const float time = laps[l].t[i];
            if (!drsEdges.empty() && stateAt(drsEdges, time) != 0) ++drs[i];
            if (!slmEdges.empty() && stateAt(slmEdges, time) != 0)
                ++(statusAt(timeline, time) == 1 ? slmWet : slmDry)[i];
        }
    }

    // World to view box, as the built-in maps lay it out.
    double minX = *std::min_element(cx.begin(), cx.end()), maxX = *std::max_element(cx.begin(), cx.end());
    double minZ = *std::min_element(cz.begin(), cz.end()), maxZ = *std::max_element(cz.begin(), cz.end());
    const double extent = std::max({maxX - minX, maxZ - minZ, 1.0});
    TrackMapData map;
    map.transform.min_x = minX;
    map.transform.min_z = minZ;
    map.transform.scale = kViewFill / extent;
    map.transform.off_x = (kViewSize - (maxX - minX) * map.transform.scale) / 2.0;
    map.transform.off_z = (kViewSize - (maxZ - minZ) * map.transform.scale) / 2.0;
    const auto view = [&](size_t i) -> MapPoint {
        const double vx = (cx[i] - minX) * map.transform.scale + map.transform.off_x;
        const double vz = (cz[i] - minZ) * map.transform.scale + map.transform.off_z;
        return {std::round(vx * 100.0) / 100.0, std::round(vz * 100.0) / 100.0};
    };
    map.rotation_deg = bestRotation(cx, cz);

    const auto slice = [&](size_t from, size_t to) {
        std::vector<MapPoint> points;
        for (size_t i = from; i <= to; ++i) points.push_back(view(i % count));
        return points;
    };
    map.sectors.push_back({1, slice(0, i1)});
    map.sectors.push_back({2, slice(i1, i2)});
    map.sectors.push_back({3, slice(i2, count)});
    map.start_finish = view(0);

    const double stepM = lengthM / static_cast<double>(count);
    const auto zoneList = [&](const std::vector<int>& coverage) {
        std::vector<TrackMapZone> out;
        for (const auto& [first, last] : zones(coverage, stepM)) out.push_back({view(first), view(last)});
        return out;
    };
    map.drs_zones = zoneList(drs);
    if (auto dry = zoneList(slmDry); !dry.empty()) map.slm_dry = std::move(dry);
    if (auto wet = zoneList(slmWet); !wet.empty()) map.slm_wet = std::move(wet);

    map.track_id = info.trackId;
    map.track_name = info.trackName;
    if (map.track_name.empty() && map.track_id >= 0) map.track_name = std::string(circuitName(map.track_id));
    if (map.track_id >= 0) map.circuit_name = std::string(circuitName(map.track_id));
    map.track_length_m = std::round(lengthM);
    map.display_name = map.track_name.empty() ? std::string("Unknown track") : map.track_name;
    if (!info.createdFrom.empty()) map.created_from = info.createdFrom;
    map.laps_used = static_cast<int>(traces.size());
    map.source = "generated";
    map.map_format = kTrackMapFormat;
    out = std::move(map);
    return true;
}

std::vector<WorldPoint> archiveFitSamples(TnrdV6Archive& archive, size_t perDriver) {
    std::vector<WorldPoint> out;
    for (const auto& header : archive.driverHeaders()) {
        // One whole lap, later laps first: lap 1 starts on the grid.
        const auto laps = archive.driverLapSummaries(header.vehicleIndex);
        const V6LapSummary* chosen = nullptr;
        for (const auto& lap : laps) {
            if (lap.phase != V6Phase::Race || !lap.isCompleted || lap.isPartial ||
                !(lap.endSessionTime > lap.startSessionTime)) continue;
            if (!chosen || (chosen->lapNumber < 2 && lap.lapNumber >= 2)) chosen = &lap;
            if (chosen->lapNumber >= 2) break;
        }
        if (!chosen) continue;
        std::vector<float> t;
        std::vector<double> x, z;
        if (!positions(archive, header.vehicleIndex, chosen->startSessionTime, chosen->endSessionTime, t, x, z) ||
            t.empty()) continue;
        const size_t stride = std::max<size_t>(1, t.size() / std::max<size_t>(perDriver, 1));
        for (size_t i = 0; i < t.size(); i += stride) out.push_back({x[i], z[i]});
    }
    return out;
}

} // namespace tnrp::detail

namespace tnrp {

bool generateTrackMapFromRecording(const std::string& path, TrackMapData& out,
                                   std::string* errorOut, const std::function<bool()>& cancelled) {
    detail::V6LoadResult loaded;
    std::string error;
    if (!detail::TNRD_V6::load(path, loaded, error) || !loaded.archive) {
        if (errorOut) *errorOut = error.empty() ? "Maps can only be generated from V6 recordings" : error;
        return false;
    }
    TrackMapGenerateInfo info;
    info.trackId = loaded.header.track_id;
    info.trackName = loaded.header.track_name;
    info.trackLengthM = loaded.header.track_length_m.value_or(0);
    const std::u8string name = std::filesystem::path(
        std::u8string(path.begin(), path.end())).filename().u8string();
    info.createdFrom = std::string(name.begin(), name.end());
    return detail::buildTrackMap(*loaded.archive, info, out, errorOut, cancelled);
}

std::vector<WorldPoint> recordingFitSamples(const std::string& path, size_t perDriver) {
    detail::V6LoadResult loaded;
    std::string error;
    if (!detail::TNRD_V6::load(path, loaded, error) || !loaded.archive) return {};
    return detail::archiveFitSamples(*loaded.archive, perDriver);
}

} // namespace tnrp
