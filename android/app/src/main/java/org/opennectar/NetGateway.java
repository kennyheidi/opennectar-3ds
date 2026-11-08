package org.opennectar;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.RouteInfo;

import java.net.Inet4Address;
import java.net.InetAddress;

/**
 * Hands the default IPv4 gateway to the native online layer, which asks that
 * router to forward a UDP port (PCP / NAT-PMP). Android 10+ hides
 * /proc/net/route from apps, so the route comes from ConnectivityManager.
 * Nothing leaves the device here; UPnP keeps working without it.
 */
final class NetGateway {
    private NetGateway() {}

    private static native void nativeSetGateway(byte[] address);

    static void publish(Context context) {
        try {
            ConnectivityManager cm = (ConnectivityManager) context.getSystemService(Context.CONNECTIVITY_SERVICE);
            if (cm == null) return;
            Network network = cm.getActiveNetwork();
            if (network == null) return;
            LinkProperties links = cm.getLinkProperties(network);
            if (links == null) return;
            for (RouteInfo route : links.getRoutes()) {
                InetAddress gateway = route.getGateway();
                if (route.isDefaultRoute() && gateway instanceof Inet4Address) {
                    nativeSetGateway(gateway.getAddress());
                    return;
                }
            }
        } catch (SecurityException | UnsatisfiedLinkError e) {
            // No network-state permission or a build without the online
            // layer: the game simply skips PCP/NAT-PMP.
        }
    }
}
