package com.tracknrace.android;

import android.os.SystemClock;

import org.json.JSONObject;

/** Latest-value store. It deliberately retains no 60 Hz history. */
final class DashboardTelemetryStore {
    static final class Snapshot {
        int speedKph;
        int rpm;
        int gear;
        float throttle;
        float brake;
        double steering;
        int drs;
        int slm;
        int engineTemp;
        int position;
        int lapNumber;
        int totalLaps;
        int currentLapMs;
        int lastLapMs;
        boolean lapInvalid;
        int ersPercent;
        double fuelLaps;
        int brakeBias;
        int tyreCompound;
        int tyreAgeLaps;
        int activeFormat;
        String aeroMode = "drs";
        boolean receiving;
        String recordingError;
    }

    private final Snapshot value = new Snapshot();
    private long lastTelemetryAt;

    synchronized boolean accept(String json) throws Exception {
        JSONObject row = new JSONObject(json);
        switch (row.optString("type")) {
            case "telemetry":
                value.speedKph = row.optInt("speed_kph");
                value.rpm = row.optInt("rpm");
                value.gear = row.optInt("gear");
                value.throttle = (float) row.optDouble("throttle");
                value.brake = (float) row.optDouble("brake");
                value.steering = row.optDouble("steering");
                value.drs = row.optInt("drs");
                value.slm = row.optInt("slm");
                value.engineTemp = row.optInt("engine_temp");
                lastTelemetryAt = SystemClock.elapsedRealtime();
                return true;
            case "lap":
                value.position = row.optInt("position");
                value.lapNumber = row.optInt("lap_num");
                value.currentLapMs = row.optInt("current_lap_ms");
                value.lastLapMs = row.optInt("last_lap_ms");
                value.lapInvalid = row.optBoolean("lap_invalid");
                return true;
            case "status":
                value.ersPercent = (int) Math.round(row.optDouble("ers_pct"));
                value.fuelLaps = row.optDouble("fuel_laps");
                value.brakeBias = row.optInt("front_brake_bias");
                value.tyreCompound = row.optInt("visual_compound");
                value.tyreAgeLaps = row.optInt("tyre_age_laps");
                return true;
            case "session":
                value.totalLaps = row.optInt("total_laps");
                return true;
            case "protocol_status":
                if (!row.isNull("active_format")) {
                    value.activeFormat = row.optInt("active_format");
                }
                value.aeroMode = row.optString("aero_mode", "drs");
                return true;
            case "recording_error":
                value.recordingError = "Recording "
                    + row.optString("operation", "save") + " failed: "
                    + row.optString("message", "Unknown recording error");
                return true;
            default:
                return false;
        }
    }

    synchronized Snapshot snapshot() {
        Snapshot copy = new Snapshot();
        copy.speedKph = value.speedKph;
        copy.rpm = value.rpm;
        copy.gear = value.gear;
        copy.throttle = value.throttle;
        copy.brake = value.brake;
        copy.steering = value.steering;
        copy.drs = value.drs;
        copy.slm = value.slm;
        copy.engineTemp = value.engineTemp;
        copy.position = value.position;
        copy.lapNumber = value.lapNumber;
        copy.totalLaps = value.totalLaps;
        copy.currentLapMs = value.currentLapMs;
        copy.lastLapMs = value.lastLapMs;
        copy.lapInvalid = value.lapInvalid;
        copy.ersPercent = value.ersPercent;
        copy.fuelLaps = value.fuelLaps;
        copy.brakeBias = value.brakeBias;
        copy.tyreCompound = value.tyreCompound;
        copy.tyreAgeLaps = value.tyreAgeLaps;
        copy.activeFormat = value.activeFormat;
        copy.aeroMode = value.aeroMode;
        copy.recordingError = value.recordingError;
        copy.receiving = lastTelemetryAt > 0
            && SystemClock.elapsedRealtime() - lastTelemetryAt < 1_500;
        return copy;
    }
}
