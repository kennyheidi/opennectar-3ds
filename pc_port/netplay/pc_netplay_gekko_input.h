#pragma once
// One player's input for one lockstep frame: a fixed 16-byte record,
// little-endian, serialised field by field (never a struct memcpy):
//   offset  size  field
//   0       2     buttons u16 (PAD_BUTTON_* bits)
//   2       1     stickX s8
//   3       1     stickY s8
//   4       1     substickX s8
//   5       1     substickY s8
//   6       1     triggerL u8
//   7       1     triggerR u8
//   8       2     control yaw u16 (pc_input_log_yaw_quantise)
//   10      1     ext u8: Bomb, Lock-On and Charge presses and the held
//                 Swarm button (pc_input_log::kExt*)
//   11      2     cursor rotate s16 LE, 1/65536 turns (mouse/touch/stick
//                 free-camera drag since the last input)
//   13      3     spare, zero
//
// Engine-free: only <cstddef>/<cstdint>.

#include <cstddef>
#include <cstdint>

namespace pc_netplay_gekko {
constexpr size_t kInputBytes = 16;
} // namespace pc_netplay_gekko

struct PcNetplayInput {
	uint16_t buttons = 0;
	int8_t stickX = 0;
	int8_t stickY = 0;
	int8_t substickX = 0;
	int8_t substickY = 0;
	uint8_t triggerL = 0;
	uint8_t triggerR = 0;
	uint16_t controlYaw = 0;
	uint8_t ext = 0;
	int16_t cursorRotate = 0;
	uint8_t spare[3] = {};
};

// Encodes exactly 16 bytes. Returns bytes written (16).
inline size_t pc_netplay_input_encode(const PcNetplayInput& in, uint8_t out[16])
{
	out[0]  = (uint8_t)(in.buttons & 0xFF);
	out[1]  = (uint8_t)((in.buttons >> 8) & 0xFF);
	out[2]  = (uint8_t)in.stickX;
	out[3]  = (uint8_t)in.stickY;
	out[4]  = (uint8_t)in.substickX;
	out[5]  = (uint8_t)in.substickY;
	out[6]  = in.triggerL;
	out[7]  = in.triggerR;
	out[8]  = (uint8_t)(in.controlYaw & 0xFF);
	out[9]  = (uint8_t)((in.controlYaw >> 8) & 0xFF);
	out[10] = in.ext;
	out[11] = (uint8_t)((uint16_t)in.cursorRotate & 0xFF);
	out[12] = (uint8_t)(((uint16_t)in.cursorRotate >> 8) & 0xFF);
	out[13] = in.spare[0];
	out[14] = in.spare[1];
	out[15] = in.spare[2];
	return pc_netplay_gekko::kInputBytes;
}

// Decodes from the first 16 bytes. Returns false when avail < 16.
inline bool pc_netplay_input_decode(const uint8_t* data, size_t avail, PcNetplayInput& out)
{
	if (data == nullptr || avail < pc_netplay_gekko::kInputBytes) return false;
	out.buttons     = (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
	out.stickX      = (int8_t)data[2];
	out.stickY      = (int8_t)data[3];
	out.substickX   = (int8_t)data[4];
	out.substickY   = (int8_t)data[5];
	out.triggerL    = data[6];
	out.triggerR    = data[7];
	out.controlYaw  = (uint16_t)(data[8] | ((uint16_t)data[9] << 8));
	out.ext          = data[10];
	out.cursorRotate = (int16_t)(uint16_t)(data[11] | ((uint16_t)data[12] << 8));
	out.spare[0]     = data[13];
	out.spare[1]     = data[14];
	out.spare[2]     = data[15];
	return true;
}

// Neutral (hands-off) input: no buttons, centred sticks, yaw 0.
inline PcNetplayInput pc_netplay_input_neutral()
{
	return PcNetplayInput();
}

// Per-turn physical-pad accumulator. The driver samples the pad every loop
// turn but submits at most one input per Advance, so a tap that starts and
// ends between two submit turns would be lost. Between submits every sample
// is folded in: button bits OR-accumulate, while the sticks, triggers and
// control yaw keep the latest sample, the ext bits OR-accumulate and the
// cursor rotate sums (saturating). take() returns the merged input and
// clears the button, ext and rotate latches.
struct PcNetplayAccum {
	uint16_t buttons = 0;
	int8_t stickX = 0;
	int8_t stickY = 0;
	int8_t substickX = 0;
	int8_t substickY = 0;
	uint8_t triggerL = 0;
	uint8_t triggerR = 0;
	uint16_t controlYaw = 0;
	uint8_t ext = 0;
	int32_t cursorRotate = 0;

	void reset()
	{
		buttons = 0;
		stickX = stickY = 0;
		substickX = substickY = 0;
		triggerL = triggerR = 0;
		controlYaw = 0;
		ext = 0;
		cursorRotate = 0;
	}

	void add_ext(uint8_t e, int16_t rotate)
	{
		ext |= e;
		cursorRotate += rotate;
		if (cursorRotate > 32767) cursorRotate = 32767;
		if (cursorRotate < -32767) cursorRotate = -32767;
	}

	void add(uint16_t b, int8_t sx, int8_t sy, int8_t cx, int8_t cy, uint8_t tl, uint8_t tr, uint16_t yaw)
	{
		buttons |= b;
		stickX = sx;
		stickY = sy;
		substickX = cx;
		substickY = cy;
		triggerL = tl;
		triggerR = tr;
		controlYaw = yaw;
	}

	void add_input(const PcNetplayInput& in)
	{
		add(in.buttons, in.stickX, in.stickY, in.substickX, in.substickY, in.triggerL, in.triggerR, in.controlYaw);
		add_ext(in.ext, in.cursorRotate);
	}

	PcNetplayInput take()
	{
		PcNetplayInput out;
		out.buttons    = buttons;
		out.stickX     = stickX;
		out.stickY     = stickY;
		out.substickX  = substickX;
		out.substickY  = substickY;
		out.triggerL   = triggerL;
		out.triggerR   = triggerR;
		out.controlYaw = controlYaw;
		out.ext          = ext;
		out.cursorRotate = (int16_t)cursorRotate;
		buttons          = 0;
		ext              = 0;
		cursorRotate     = 0;
		return out;
	}
};
