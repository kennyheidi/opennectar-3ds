#include "Boss.h"
#include "DebugLog.h"
#include "Shape.h"
#include "sysNew.h"
#if defined(PIKI_PC_PORT)
#include "Graphics.h"
#include "netplay/pc_netplay_present.h"
#include "timing/pc_render_phase.h"
#endif

/**
 * @todo: Documentation
 * @note UNUSED Size: 00009C
 */
DEFINE_ERROR(__LINE__) // Never used in the DLL

/**
 * @todo: Documentation
 * @note UNUSED Size: 0000F4
 */
DEFINE_PRINT("BossShapeObj");

/**
 * @todo: Documentation
 */
BossShapeObject::BossShapeObject(Shape* shape, immut char* bossName)
{
	mShape               = shape;
	mShape->mFrameCacher = nullptr;

	if (bossName) {
		PRINT("########## AnimMgr Construct Start -> %s\n", bossName);
		char binFileName[128];
		sprintf(binFileName, "bosses/%s/anims.bin", bossName);
		mAnimMgr        = new AnimMgr(shape, binFileName, ANIMMGR_LOAD_BUNDLE, nullptr);
		mAnimMgr->mName = bossName;
		PRINT("########## AnimMgr Construct END\n");
	} else {
		mAnimMgr = new AnimMgr(shape, nullptr, ANIMMGR_LOAD_NOSKIP, nullptr);
	}

	mShape->overrideAnim(0, &mAnimContext);
}

#if defined(PIKI_PC_PORT)
/**
 * @brief Keeps the shape's final joint matrices in world space (see BossPresentJoints).
 */
void BossPresentJoints::capture(BossShapeObject* shapeObj, Graphics& gfx)
{
	if (!pc_netplay_present_two_pass_active() || !pc_render_is_authoritative()) {
		return;
	}

	Shape* shape = shapeObj->mShape;
	Matrix4f invLookAt;
	gfx.mCamera->mLookAtMtx.inverse(&invLookAt);
	mWorld.resize(shape->mJointCount);
	for (int i = 0; i < shape->mJointCount; i++) {
		invLookAt.multiplyTo(shape->getAnimMatrix(i), mWorld[i]);
	}
}

/**
 * @brief Rebuilds the shape's joint matrices for the presentation camera from the world-space copy.
 */
void BossPresentJoints::apply(BossShapeObject* shapeObj, Graphics& gfx)
{
	Shape* shape = shapeObj->mShape;
	if (!pc_netplay_present_two_pass_active() || pc_render_is_authoritative() || int(mWorld.size()) != shape->mJointCount) {
		return;
	}

	for (int i = 0; i < shape->mJointCount; i++) {
		gfx.mCamera->mLookAtMtx.multiplyTo(mWorld[i], shape->getAnimMatrix(i));
	}
}
#endif
