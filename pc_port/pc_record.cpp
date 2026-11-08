#include "pc_record.h"

#include <SDL.h>
#include <cstdio>

#include "pc_photo_mode.h"

namespace {

bool sHudHidden = false;
bool sCtrlPrev[10] = {};

const char* const kCameraFile = "record_camera.txt";

} // namespace

extern "C" {

int pc_record_ctrl_pressed(int digit)
{
	if (digit < 1 || digit > 9) return 0;
	int numKeys       = 0;
	const Uint8* keys = SDL_GetKeyboardState(&numKeys);
	// SDL_SCANCODE_1..9 son consecutivos.
	const int sc      = SDL_SCANCODE_1 + (digit - 1);
	const bool ctrl   = (SDL_GetModState() & KMOD_CTRL) != 0;
	const bool down   = ctrl && keys != nullptr && sc < numKeys && keys[sc] != 0;
	const bool edge   = down && !sCtrlPrev[digit];
	sCtrlPrev[digit]  = down;
	return edge ? 1 : 0;
}

int pc_record_hud_hidden(void) { return sHudHidden ? 1 : 0; }

void pc_record_toggle_hud(void)
{
	sHudHidden = !sHudHidden;
	fprintf(stderr, "[RECORD] HUD %s\n", sHudHidden ? "hidden" : "shown");
	fflush(stderr);
}

void pc_record_save_camera(void)
{
	if (!pc_photo_mode_active()) return;
	float x, y, z, pitch, yaw, roll;
	pc_photo_mode_get_pose(&x, &y, &z, &pitch, &yaw, &roll);
	FILE* f = fopen(kCameraFile, "w");
	if (!f) {
		fprintf(stderr, "[RECORD] cannot write %s\n", kCameraFile);
		fflush(stderr);
		return;
	}
	fprintf(f, "%.6f %.6f %.6f %.6f %.6f %.6f\n", x, y, z, pitch, yaw, roll);
	fclose(f);
	fprintf(stderr, "[RECORD] camera saved to %s\n", kCameraFile);
	fflush(stderr);
}

void pc_record_load_camera(void)
{
	if (!pc_photo_mode_active()) return;
	FILE* f = fopen(kCameraFile, "r");
	if (!f) {
		fprintf(stderr, "[RECORD] no %s yet (Ctrl+7 saves one)\n", kCameraFile);
		fflush(stderr);
		return;
	}
	float x, y, z, pitch, yaw, roll;
	const int n = fscanf(f, "%f %f %f %f %f %f", &x, &y, &z, &pitch, &yaw, &roll);
	fclose(f);
	if (n != 6) {
		fprintf(stderr, "[RECORD] %s is not a saved camera\n", kCameraFile);
		fflush(stderr);
		return;
	}
	pc_photo_mode_set_pose(x, y, z, pitch, yaw, roll);
	fprintf(stderr, "[RECORD] camera restored\n");
	fflush(stderr);
}

} // extern "C"
