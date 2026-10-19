// SPDX-License-Identifier: GPL-3.0-or-later
package org.helixscreen.app;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.util.Log;

/**
 * Keeps the process bound to the connected Wi-Fi network so printer traffic stays on the LAN.
 *
 * Android makes a Wi-Fi network with no internet access the default only when nothing else
 * is available. With mobile data up, every socket the app opens goes out over the cellular
 * link, so a Moonraker connection to a 192.168.x.x printer times out. Binding the process to
 * the Wi-Fi network routes all of its sockets, native ones included, over Wi-Fi.
 *
 * The binding is dropped (the system default network decides again) when:
 *   - no Wi-Fi is connected, so a printer reached over mobile data still works;
 *   - a VPN is the default network, so a printer reached through Tailscale or another VPN
 *     still goes through it. A process bound to the Wi-Fi network bypasses the VPN.
 *
 * Binding affects sockets opened after the change; a connection already in flight finishes
 * or times out on the network it started on and reconnects on the new one.
 */
final class WifiBinder {
    private static final String TAG = "HelixNetwork";

    private final ConnectivityManager mConnectivity;
    private final Object mLock = new Object();
    private Network mWifi;
    private boolean mDefaultIsVpn;
    private Network mBound;
    private ConnectivityManager.NetworkCallback mWifiCallback;
    private ConnectivityManager.NetworkCallback mDefaultCallback;

    WifiBinder(Context context) {
        mConnectivity = context.getSystemService(ConnectivityManager.class);
    }

    void start() {
        if (mConnectivity == null || mWifiCallback != null) {
            return;
        }
        // A printer's own access point may not claim internet, so do not require it.
        NetworkRequest wifiRequest = new NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .build();
        mWifiCallback = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onAvailable(Network network) {
                synchronized (mLock) {
                    mWifi = network;
                    updateLocked("Wi-Fi available");
                }
            }

            @Override
            public void onLost(Network network) {
                synchronized (mLock) {
                    if (network.equals(mWifi)) {
                        mWifi = null;
                    }
                    updateLocked("Wi-Fi lost");
                }
            }
        };
        mDefaultCallback = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onCapabilitiesChanged(Network network, NetworkCapabilities caps) {
                synchronized (mLock) {
                    mDefaultIsVpn = caps.hasTransport(NetworkCapabilities.TRANSPORT_VPN);
                    updateLocked("default network changed");
                }
            }

            @Override
            public void onLost(Network network) {
                synchronized (mLock) {
                    mDefaultIsVpn = false;
                    updateLocked("default network lost");
                }
            }
        };
        try {
            mConnectivity.registerNetworkCallback(wifiRequest, mWifiCallback);
            mConnectivity.registerDefaultNetworkCallback(mDefaultCallback);
        } catch (RuntimeException e) {
            Log.w(TAG, "Network callback registration failed: " + e);
            stop();
        }
    }

    void stop() {
        if (mConnectivity == null) {
            return;
        }
        for (ConnectivityManager.NetworkCallback cb :
                new ConnectivityManager.NetworkCallback[] {mWifiCallback, mDefaultCallback}) {
            if (cb == null) {
                continue;
            }
            try {
                mConnectivity.unregisterNetworkCallback(cb);
            } catch (RuntimeException e) {
                // Never registered: nothing to undo.
            }
        }
        mWifiCallback = null;
        mDefaultCallback = null;
        synchronized (mLock) {
            mWifi = null;
            mDefaultIsVpn = false;
            updateLocked("stopped");
        }
    }

    private void updateLocked(String why) {
        Network target = (mWifi != null && !mDefaultIsVpn) ? mWifi : null;
        if (target == null ? mBound == null : target.equals(mBound)) {
            return;
        }
        boolean ok = mConnectivity.bindProcessToNetwork(target);
        if (ok) {
            mBound = target;
        }
        if (target != null) {
            Log.i(TAG, "Bound process to Wi-Fi " + target + " (" + why + "): " + (ok ? "ok" : "FAILED"));
        } else {
            Log.i(TAG, "Unbound process, system default network decides (" + why
                    + (mDefaultIsVpn ? ", VPN active" : "") + "): " + (ok ? "ok" : "FAILED"));
        }
    }
}
