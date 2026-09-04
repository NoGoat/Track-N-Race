package com.tracknrace.android;

final class NativePairDiscovery {
    interface Listener {
        void onService(Service service);
    }

    static final class Service {
        final String serverId;
        final String name;
        final String address;
        final int port;
        final boolean pairing;

        Service(String serverId, String name, String address, int port, boolean pairing) {
            this.serverId = serverId;
            this.name = name;
            this.address = address;
            this.port = port;
            this.pairing = pairing;
        }
    }

    static { System.loadLibrary("tracknrace_android"); }

    private final Listener listener;

    NativePairDiscovery(Listener listener) { this.listener = listener; }
    String start() { return nativeStart(); }
    void stop() { nativeStop(); }

    @SuppressWarnings("unused")
    private void onNativeService(String serverId, String name, String address,
                                 int port, boolean pairing) {
        listener.onService(new Service(serverId, name, address, port, pairing));
    }

    private native String nativeStart();
    private native void nativeStop();
}
