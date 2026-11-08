#pragma once

// Netplay sim-visibility policy.
//
// In deterministic mode no simulation decision may depend on the local
// camera frustum, the window aspect or the camera distance; drawing still
// may. A refresh site whose visibility/aspect/LOD result feeds sim state
// routes it through one of these helpers. With the switch off each helper
// returns its argument unchanged.
//
// Engine-free (only pc_netplay_det.h), so game headers may include it.

// Always visible in deterministic mode.
bool pc_netplay_sim_visible(bool frustumResult);

// Fixed 16:9 in deterministic mode, so no sim-affecting frustum reads the
// window size.
float pc_netplay_sim_aspect(float liveAspect);

// Camera distance for sim-affecting LOD: 0 (full LOD) in deterministic mode.
float pc_netplay_sim_lod_distance(float liveDistance);
