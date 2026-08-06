package com.tracknrace.android;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

final class TimingStore {
    static final class Driver {
        int idx;
        int raceNumber;
        String name = "";
        String color = "#8E8E8E";
    }

    static final class Car {
        int idx;
        int position;
        int lapNum;
        int lastLapMs;
        int gapMs;
        int pitStatus;
        int resultStatus;
        int penalties;
        int driveThroughs;
        int stopGos;
        int sector;
        boolean invalid;
        int s1;
        int s2;
        int s3;
        int actualCompound;
        int visualCompound;
        boolean player;
        boolean fastest;
        Driver driver;
    }

    private static final class SectorSnapshot {
        int lap;
        int s1;
        int s2;
    }

    private static final class FrozenSectors {
        int s1;
        int s2;
        int s3;
        long expiresAt;
    }

    private final Map<Integer, Driver> drivers = new HashMap<>();
    private final Map<Integer, int[]> compounds = new HashMap<>();
    private final Map<Integer, Car> previous = new HashMap<>();
    private final Map<Integer, SectorSnapshot> snapshots = new HashMap<>();
    private final Map<Integer, FrozenSectors> frozen = new HashMap<>();
    private List<Car> cars = Collections.emptyList();
    private int fastestCar = -1;
    private int activeFormat;

    synchronized boolean accept(String json) throws Exception {
        JSONObject root = new JSONObject(json);
        String type = root.optString("type");
        switch (type) {
            case "participants":
                parseParticipants(root.optJSONArray("drivers"));
                return true;
            case "all_status":
                parseCompounds(root.optJSONArray("cars"));
                return true;
            case "fastest_lap":
            case "session_history_fastest":
                fastestCar = root.optInt("car_idx", -1);
                return true;
            case "protocol_status":
                if (!root.isNull("active_format")) activeFormat = root.optInt("active_format", 0);
                return true;
            case "timing":
                parseTiming(root);
                return true;
            default:
                return false;
        }
    }

    synchronized List<Car> snapshot() {
        return new ArrayList<>(cars);
    }

    synchronized int activeFormat() {
        return activeFormat;
    }

    private void parseParticipants(JSONArray array) {
        if (array == null) return;
        drivers.clear();
        for (int i = 0; i < array.length(); i++) {
            JSONObject item = array.optJSONObject(i);
            if (item == null) continue;
            Driver d = new Driver();
            d.idx = item.optInt("idx", i);
            d.raceNumber = item.optInt("race_number", 0);
            d.name = item.optString("name", "Car " + d.idx);
            d.color = item.optString("livery_color", "#8E8E8E");
            drivers.put(d.idx, d);
        }
    }

    private void parseCompounds(JSONArray array) {
        if (array == null) return;
        compounds.clear();
        for (int i = 0; i < array.length(); i++) {
            JSONObject item = array.optJSONObject(i);
            if (item == null) continue;
            compounds.put(item.optInt("idx", i), new int[] {
                item.optInt("tyre_compound", 0), item.optInt("visual_compound", 0)
            });
        }
    }

    private void parseTiming(JSONObject root) {
        JSONArray array = root.optJSONArray("cars");
        if (array == null) return;
        int playerIdx = root.optInt("player_idx", -1);
        long now = System.currentTimeMillis();
        List<Car> next = new ArrayList<>();
        Map<Integer, Car> current = new HashMap<>();

        for (int i = 0; i < array.length(); i++) {
            JSONObject item = array.optJSONObject(i);
            if (item == null) continue;
            Car car = new Car();
            car.idx = item.optInt("idx", i);
            car.position = item.optInt("position");
            car.lapNum = item.optInt("lap_num");
            car.lastLapMs = item.optInt("last_lap_ms");
            car.gapMs = item.optInt("gap_ms");
            car.pitStatus = item.optInt("pit_status");
            car.resultStatus = item.optInt("result_status");
            car.penalties = item.optInt("penalties_s");
            car.driveThroughs = item.optInt("num_dt_pens");
            car.stopGos = item.optInt("num_sg_pens");
            car.sector = item.optInt("sector");
            car.invalid = item.optBoolean("lap_invalid");
            car.s1 = item.optInt("s1_ms");
            car.s2 = item.optInt("s2_ms");
            car.player = car.idx == playerIdx;
            car.fastest = car.idx == fastestCar;
            car.driver = drivers.get(car.idx);
            int[] tyre = compounds.get(car.idx);
            if (tyre != null) {
                car.actualCompound = tyre[0];
                car.visualCompound = tyre[1];
            }

            if (car.sector == 2 && car.s1 > 0 && car.s2 > 0) {
                SectorSnapshot old = snapshots.get(car.idx);
                if (old == null || old.lap != car.lapNum) {
                    SectorSnapshot snap = new SectorSnapshot();
                    snap.lap = car.lapNum;
                    snap.s1 = car.s1;
                    snap.s2 = car.s2;
                    snapshots.put(car.idx, snap);
                }
            }

            Car old = previous.get(car.idx);
            if (old != null && car.lapNum > old.lapNum) {
                SectorSnapshot snap = snapshots.get(car.idx);
                FrozenSectors hold = new FrozenSectors();
                hold.s1 = old.s1;
                hold.s2 = old.s2;
                hold.s3 = snap != null && snap.lap == old.lapNum && car.lastLapMs > 0
                    ? Math.max(0, car.lastLapMs - snap.s1 - snap.s2) : 0;
                hold.expiresAt = now + 7_000;
                frozen.put(car.idx, hold);
            }
            FrozenSectors hold = frozen.get(car.idx);
            if (hold != null && now < hold.expiresAt) {
                car.s1 = hold.s1;
                car.s2 = hold.s2;
                car.s3 = hold.s3;
            }

            current.put(car.idx, car);
            if (car.position > 0 && car.resultStatus > 1) next.add(car);
        }
        next.sort(Comparator.comparingInt(value -> value.position));
        previous.clear();
        previous.putAll(current);
        cars = next;
    }

    static String abbreviation(Driver driver, int idx) {
        if (driver == null || driver.name.trim().isEmpty()) return "C" + idx;
        String[] bits = driver.name.trim().split("\\s+");
        String last = bits[bits.length - 1];
        return last.substring(0, Math.min(3, last.length())).toUpperCase(Locale.ROOT);
    }
}
