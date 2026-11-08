#pragma once

#include "netplay/pc_netplay_sha256.h"

#include <cstdint>
#include <string>

#ifndef PIKMIN_NETPLAY_SOURCE_ID
#error "PIKMIN_NETPLAY_SOURCE_ID must identify the source tree of a netplay build"
#endif

#ifndef PIKMIN_NETPLAY_GAME_VERSION
#error "PIKMIN_NETPLAY_GAME_VERSION must identify the selected retail game version"
#endif

namespace pc_netplay_build {

// Increment whenever two builds with the same source tree would interpret
// deterministic state differently. The protocol version protects the wire;
// this protects the simulation contract.
constexpr unsigned kSimulationAbi = 2;

inline std::string manifest()
{
	return std::string("open-nectar-lockstep;") + "simulation-abi=" + std::to_string(kSimulationAbi)
	     + ";source=" PIKMIN_NETPLAY_SOURCE_ID ";game=pikmin1;region=" PIKMIN_NETPLAY_GAME_VERSION
	       ";float=ieee754-software-libm;tick=30;";
}

inline void digest(std::uint8_t out[32])
{
	const std::string value = manifest();
	pc_netplay_sha::sha256(value.data(), value.size(), out);
}

} // namespace pc_netplay_build
