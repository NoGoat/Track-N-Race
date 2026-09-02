package com.tracknrace.android;

final class NativeTelemetry {
    interface Listener {
        void onTelemetryRow(String json);
    }

    static {
        System.loadLibrary("tracknrace_android");
    }

    private final Listener listener;

    NativeTelemetry(Listener listener) {
        this.listener = listener;
    }

    String start(int port) {
        return nativeStart(port);
    }

    void stop() {
        nativeStop();
    }

    void setRecording(boolean enabled, String outputDirectory) {
        nativeSetRecording(enabled, outputDirectory);
    }

    @SuppressWarnings("unused") // Invoked by JNI on libtnrp worker threads.
    private void onNativeRow(String json) {
        listener.onTelemetryRow(json);
    }

    private native String nativeStart(int port);
    private native void nativeStop();
    private native void nativeSetRecording(boolean enabled, String outputDirectory);
}
