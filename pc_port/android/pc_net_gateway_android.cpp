// Online co-op on Android: apps may not read /proc/net/route (Android 10+), so
// org.opennectar.NetGateway reads the default route from ConnectivityManager
// and passes the IPv4 gateway here, for PCP/NAT-PMP port mapping. Builds
// without the online layer accept the call and ignore it.
#include <jni.h>

#include <cstdint>

#if PIKI_NECTAR_ONLINE || PIKI_NETPLAY_PORTMAP
#include "nectar_net/net_platform.h"
#endif

extern "C" JNIEXPORT void JNICALL Java_org_opennectar_NetGateway_nativeSetGateway(JNIEnv* env, jclass /*clazz*/,
                                                                                    jbyteArray address)
{
#if PIKI_NECTAR_ONLINE || PIKI_NETPLAY_PORTMAP
	if (env == nullptr || address == nullptr || env->GetArrayLength(address) != 4) return;
	jbyte bytes[4];
	env->GetByteArrayRegion(address, 0, 4, bytes);
	nectar::net::setGatewayHint({ (std::uint8_t)bytes[0], (std::uint8_t)bytes[1], (std::uint8_t)bytes[2],
	                              (std::uint8_t)bytes[3] });
#else
	(void)env;
	(void)address;
#endif
}
