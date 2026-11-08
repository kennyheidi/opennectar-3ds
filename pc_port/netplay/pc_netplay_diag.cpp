// Netplay-only failure-path diagnostics.
// See pc_netplay_diag.h. Reads game state and writes stderr; called only on a
// path that halts right after, so it cannot perturb a running simulation.

#include "netplay/pc_netplay_diag.h"
#include "netplay/pc_netplay_det.h"

#include "GoalItem.h"
#include "ItemMgr.h"
#include "Navi.h"
#include "NaviMgr.h"
#include "Piki.h"
#include "PlayerState.h"

#include <cstdio>

void pc_netplay_diag_bad_action_target(const char* site, Piki* piki, Creature* target)
{
	std::fprintf(stderr, "[netplay-diag] %s: bad target at tick=%u dayEnd=%d\n", site, pc_netplay_tick(),
	             playerState ? (int)playerState->inDayEnd() : -1);
	if (target) {
		std::fprintf(stderr, "[netplay-diag] %s: target=%p objType=%d\n", site, (void*)target, (int)target->mObjType);
	} else {
		std::fprintf(stderr, "[netplay-diag] %s: target=null\n", site);
	}
	if (piki) {
		Navi* navi = piki->mNavi;
		std::fprintf(stderr, "[netplay-diag] %s: piki=%p color=%d mode=%d state=%d alive=%d navi=%p naviIndex=%d pos=(%.1f,%.1f)\n",
		             site, (void*)piki, (int)piki->mColor, (int)piki->mMode, piki->getState(), (int)piki->isAlive(), (void*)navi,
		             navi ? (int)navi->mNaviID : -1, piki->mSRT.t.x, piki->mSRT.t.z);
	}
	int naviCount = naviMgr ? naviMgr->getNaviCount() : 0;
	for (int i = 0; i < naviCount; i++) {
		Navi* navi = naviMgr->getNavi(i);
		if (!navi) {
			continue;
		}
		GoalItem* goal = navi->mGoalItem;
		std::fprintf(stderr, "[netplay-diag] %s: navi[%d]=%p goalItem=%p goalColor=%d pos=(%.1f,%.1f)\n", site, i, (void*)navi,
		             (void*)goal, goal ? (int)goal->mOnionColour : -1, navi->mSRT.t.x, navi->mSRT.t.z);
	}
	if (itemMgr) {
		for (int color = 0; color < PikiColorCount; color++) {
			GoalItem* goal = itemMgr->getContainer(color);
			std::fprintf(stderr, "[netplay-diag] %s: container[%d]=%p\n", site, color, (void*)goal);
		}
	}
	pc_netplay_diag_backtrace(site);
}
