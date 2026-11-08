#pragma once
// Netplay lead (instant) camera.
//
// In a lockstep session the presentation pass renders the local captain's
// camera with the local camera controls applied on the frame they are
// sampled, instead of `delay` + 1 frames later when the synced input reaches
// the sim camera. Design and maths: pc_netplay_camlead_core.h.
//
// Contract:
//   - presentation only: the lead camera is a separate Camera object that
//     only the presentation pass renders with (and the local yaw sampler
//     reads). The prediction runs on a byte snapshot of the sim camera and
//     restores it, with camera sounds muted, and gfx.mCamera is put back to
//     the sim camera at the end of the presentation pass;
//   - session only: every entry point is inert until a lockstep session calls
//     pc_netplay_camlead_session_begin;
//   - PIKMIN_NETPLAY_CAMERA_LEAD=0 renders the sim camera;
//   - PIKMIN_NETPLAY_JOINER_OWN_CAMERA (default on): this peer's pad samples,
//     and its free-camera drags turn, this peer's own captain's camera.
// Diagnostics: PIKMIN_NETPLAY_CAMERA_TRACE=1 logs one line per presented
// frame; PIKMIN_NETPLAY_TEST_CAMERA_DRAG=<frame>:<amount>,... adds a mouse
// free-camera drag before those frames' ticks;
// PIKMIN_NETPLAY_TEST_CAMLEAD_KEY_SKEW=<n> files every noted local input n
// frames off its landing frame (negative control: key_mismatch must go
// non-zero). All presentation only.

#include "netplay/pc_netplay_gekko_input.h"

#include <cstdint>

// ---- Session side ----
// Arms the lead for `localRole` (0 host/P1, 1 joiner/P2) unless
// PIKMIN_NETPLAY_CAMERA_LEAD=0. Logs one line.
void pc_netplay_camlead_session_begin(int localRole);
// Logs the summary line and disarms everything.
void pc_netplay_camlead_session_end(void);
// A local input was submitted for lockstep frame `frame` (submit + delay).
void pc_netplay_camlead_note_local_input(uint64_t frame, const PcNetplayInput& in);
// The Advance of `frame` carries this peer's input `wire` (16 bytes): if a
// local input was noted for `frame`, it must encode to the same bytes, else
// key_mismatch counts it.
void pc_netplay_camlead_check_applied(uint64_t frame, const uint8_t* wire);
// The Advance of `frame` is about to run its tick.
void pc_netplay_camlead_begin_frame(uint64_t frame);
bool pc_netplay_camlead_armed(void);

#if defined(PIKI_PC_PORT) && defined(__cplusplus)
class Camera;
class Graphics;
class PcamCamera;
class PcamCameraManager;

// ---- Engine side ----
// Presentation pass, single local view: `simView` is the local captain's sim
// camera. Returns the camera to render with: the lead camera while a
// correction is active, else simView.
Camera* pc_netplay_camlead_view(int localPlayer, Camera* simView);
// End of the presentation pass: puts gfx.mCamera back to the sim camera,
// logs the trace line, and drops the correction when no view was presented.
void pc_netplay_camlead_end_presentation(Graphics& gfx);
// Local control-yaw sampler: the camera whose yaw pad `pad` submits.
Camera* pc_netplay_camlead_control_camera(int pad, Camera* cam);
// PcamCameraManager::update, after the sim camera's update and before its
// vibration events: records the posture the sim camera shows.
void pc_netplay_camlead_note_sim_update(PcamCameraManager* mgr);
// PcamCamera::makeCurrentPosition: the sim camera was snapped, so a stale
// correction must not carry over.
void pc_netplay_camlead_note_snap(PcamCamera* cam);
// True while the prediction runs on the sim camera (camera sounds muted).
bool pc_netplay_camlead_predicting(void);
// Free-camera drag routing for `cam`: -1 keeps the per-slot routing (no
// session, or PIKMIN_NETPLAY_JOINER_OWN_CAMERA=0); 1: `cam` is this peer's
// own captain's camera and takes every local drag; 0: it is the other
// captain's camera and takes none.
int pc_netplay_camlead_drag_route(const PcamCamera* cam);
#endif
