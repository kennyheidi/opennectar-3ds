#include "DebugLog.h"
#include "NaviMgr.h"
#include "Pcam/Camera.h"
#include "Pcam/CameraManager.h"
#include "Pcam/MotionEvents.h"
#include "Peve/Condition.h"
#include "Peve/Event.h"
#include "sysNew.h"
#if defined(PIKI_PC_PORT)
#include "netplay/pc_netplay_camlead.h"
#include "netplay/pc_netplay_det.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
DEFINE_PRINT("pcamcameramanager")

/**
 * @todo: Documentation
 */
PcamCameraManager::PcamCameraManager(Camera* camera, Controller* controller)
    : Node("PcamCameraManager")
{
	mCamera          = new PcamCamera(camera);
	mController      = controller;
	mVibrationEvents = new PeveEvent*[PCAMVIB_VibrationCount];

	PcamVibrationEvent* vib1             = new PcamVibrationEvent(mCamera);
	vib1->mVibrationDuration             = 0.6f;
	vib1->mVibrationAmplitude            = 0.2f;
	vib1->mVibrationFrequency            = 8.0f;
	mVibrationEvents[PCAMVIB_Vibration1] = vib1;

	PcamVibrationEvent* vib2             = new PcamVibrationEvent(mCamera);
	vib2->mVibrationDuration             = 0.6f;
	vib2->mVibrationAmplitude            = 0.2f;
	vib2->mVibrationFrequency            = 4.0f;
	mVibrationEvents[PCAMVIB_Vibration2] = vib2;

	mVibrationEvents[PCAMVIB_LongVibration] = new PcamLongVibrationEvent(mCamera);

	PcamDamageEvent* damage = new PcamDamageEvent(mCamera);
	// nice typo.
	vib2->mVibrationDuration  = 0.6f;
	vib2->mVibrationAmplitude = 0.2f;

	damage->mVibrationFrequency      = 30.0f;
	mVibrationEvents[PCAMVIB_Damage] = damage;

	PcamSideVibrationEvent* sideVib = new PcamSideVibrationEvent(mCamera);
	// nice typo.
	vib2->mVibrationDuration  = 0.6f;
	vib2->mVibrationAmplitude = 0.2f;

	sideVib->mMaxRotation                   = NMathF::pi / 48.0f;
	mVibrationEvents[PCAMVIB_SideVibration] = sideVib;
	mCurrEventIndex                         = -1;
}

/**
 * @todo: Documentation
 */
void PcamCameraManager::startCamera(Creature* target)
{
	mCamera->startCamera(target);
}

/**
 * @todo: Documentation
 */
void PcamCameraManager::update()
{
#if defined(PIKI_PC_PORT)
	// Netplay test knob, deterministic mode only:
	// PIKMIN_NETPLAY_TEST_CAMERA_WOBBLE=<degrees> swings the camera yaw on a
	// 97-tick sine between control (which banks input drag into the azimuth)
	// and update (which builds the matrices), then restores it, so the view
	// really moves but nothing drifts. A replay must hash the same with and
	// without it, since the sim takes its yaw from the input, not the camera.
	static bool wobRead  = false;
	static double wobDeg = 0.0;
	if (!wobRead) {
		wobRead         = true;
		const char* env = std::getenv("PIKMIN_NETPLAY_TEST_CAMERA_WOBBLE");
		wobDeg          = (env != nullptr && *env != '\0') ? std::atof(env) : 0.0;
		if (wobDeg != 0.0) {
			std::printf("[netplay] test camera wobble: %.1f deg, 97-tick period\n", wobDeg);
			std::fflush(stdout);
		}
	}
	mCamera->control(*mController);
	f32 wobSavedAz  = 0.0f;
	f32 wobSavedCur = 0.0f;
	bool wobActive  = false;
	if (pc_netplay_deterministic() && wobDeg != 0.0) {
		const double phase  = 6.283185307179586 * (double)pc_netplay_tick() / 97.0;
		const double wobRad = wobDeg * 3.141592653589793 / 180.0 * std::sin(phase);
		if (wobRad != 0.0) {
			wobSavedAz                  = mCamera->mPolarDir.mAzimuth;
			wobSavedCur                 = mCamera->mCurrentAzimuth;
			mCamera->mPolarDir.mAzimuth = wobSavedAz + (f32)wobRad;
			mCamera->mCurrentAzimuth    = wobSavedCur + (f32)wobRad;
			wobActive                   = true;
		}
	}
	mCamera->update();
	pc_netplay_camlead_note_sim_update(this);
	updateVibrationEvent();
	if (wobActive) {
		mCamera->mPolarDir.mAzimuth = wobSavedAz;
		mCamera->mCurrentAzimuth    = wobSavedCur;
	}
#else
	mCamera->control(*mController);
	mCamera->update();
	updateVibrationEvent();
#endif
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000024
 */
void PcamCameraManager::startMotion(PcamMotionInfo& info)
{
	mCamera->startMotion(info);
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000024
 */
void PcamCameraManager::finishMotion()
{
	mCamera->finishMotion();
}

/**
 * @todo: Documentation
 */
void PcamCameraManager::updateVibrationEvent()
{
	if (mCurrEventIndex < 0) {
		return;
	}

	PeveEvent* event = mVibrationEvents[mCurrEventIndex];
	if (event->isFinished()) {
		PRINT_NAKATA("updateVibrationEvent:event->isFinished:%08x\n", event);
		event->finish();
		mCurrEventIndex = PCAMVIB_NULL;
	} else {
		event->update();
	}
}

/**
 * @todo: Documentation
 */
#if defined(PIKI_PC_PORT)
PcamCameraManager* cameraMgrP2 = nullptr;
PcamCameraManager* cameraMgrP1 = nullptr;
#endif

#if defined(PIKI_PC_PORT)
void PcamCameraManager::startVibrationEvent(int eventIdx, immut Vector3f& p2, bool mirror)
#else
void PcamCameraManager::startVibrationEvent(int eventIdx, immut Vector3f& p2)
#endif
{
	PRINT("startVibrationEvent:%d,%d\n", mCurrEventIndex, eventIdx);
#if defined(PIKI_PC_PORT)
	if (mirror && cameraMgrP2 && this != cameraMgrP2 && this == cameraMgr) {
		cameraMgrP2->startVibrationEvent(eventIdx, p2, false);
	}
#endif
	if (mCurrEventIndex < 0 || mCurrEventIndex >= eventIdx) {
		NVector3f vec1;
		outputNaviPosition(vec1);
		f32 dist = vec1.distanceXZ(p2);
		if (dist > mCamera->getParameterF(PCAMF_VibrationDistance)) {
			PRINT("startVibrationEvent:distance>:%f\n", dist);
		} else {
			mCurrEventIndex  = eventIdx;
			PeveEvent* event = mVibrationEvents[mCurrEventIndex];
			if (mCurrEventIndex == PCAMVIB_Vibration1) {
				static_cast<PcamVibrationEvent*>(event)->makePcamVibrationEvent();
			} else if (mCurrEventIndex == PCAMVIB_Vibration2) {
				static_cast<PcamVibrationEvent*>(event)->makePcamVibrationEvent();
			} else if (mCurrEventIndex == PCAMVIB_LongVibration) {
				static_cast<PcamLongVibrationEvent*>(event)->makePcamLongVibrationEvent(0.4f, 0.6f, 0.2f, 3.0f);
			} else if (mCurrEventIndex == PCAMVIB_Damage) {
				static_cast<PcamDamageEvent*>(event)->makePcamDamageEvent();
			} else if (mCurrEventIndex == PCAMVIB_SideVibration) {
				static_cast<PcamSideVibrationEvent*>(event)->makePcamSideVibrationEvent();
			}
			event->reset();
		}
	}
}

/**
 * @todo: Documentation
 */
void PcamCameraManager::outputNaviPosition(Vector3f& naviPos)
{
	Navi* navi = naviMgr->getNavi(0);
	naviPos.input(navi->getPosition());
}
