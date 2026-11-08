#include "netplay/pc_online_navi.h"

#include "GameCoreSection.h"
#include "Navi.h"
#include "NaviMgr.h"
#include "gameflow.h"

namespace {

Navi* find_navi(int player)
{
	if (naviMgr == nullptr) return nullptr;
	Iterator it(naviMgr);
	CI_LOOP(it)
	{
		Navi* n = static_cast<Navi*>(*it);
		if (n != nullptr && n->mNaviID == player) return n;
	}
	return nullptr;
}

} // namespace

bool pc_online_read_captain(int player, PcOnlineCaptain* out)
{
	Navi* n = find_navi(player);
	if (n == nullptr || out == nullptr) return false;
	out->position[0]   = n->mSRT.t.x;
	out->position[1]   = n->mSRT.t.y;
	out->position[2]   = n->mSRT.t.z;
	out->velocity[0]   = n->mVelocity.x;
	out->velocity[1]   = n->mVelocity.y;
	out->velocity[2]   = n->mVelocity.z;
	out->faceDirection = n->mFaceDirection;
	return true;
}

bool pc_online_write_captain(int player, const PcOnlineCaptain& in)
{
	Navi* n = find_navi(player);
	if (n == nullptr) return false;
	n->mSRT.t.x        = in.position[0];
	n->mSRT.t.y        = in.position[1];
	n->mSRT.t.z        = in.position[2];
	n->mVelocity.x     = in.velocity[0];
	n->mVelocity.y     = in.velocity[1];
	n->mVelocity.z     = in.velocity[2];
	n->mFaceDirection  = in.faceDirection;
	return true;
}

bool pc_online_nudge_captain(int player, float dx, float dy, float dz, bool resetVelocity)
{
	Navi* n = find_navi(player);
	if (n == nullptr) return false;
	n->mSRT.t.x += dx;
	n->mSRT.t.y += dy;
	n->mSRT.t.z += dz;
	if (resetVelocity) n->mVelocity = Vector3f(0.0f, 0.0f, 0.0f);
	return true;
}

int pc_online_pause_state(void)
{
	return (int)GameCoreSection::pauseFlag | (gameflow.mIsUIOverlayActive ? 0x10000 : 0);
}
