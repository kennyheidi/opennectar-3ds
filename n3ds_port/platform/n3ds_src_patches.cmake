# 3DS-only edits of two upstream game sources, applied to COPIES in the build folder (the upstream tree is never touched).
# Before 0.9.3 these were separate `patch -p1` files (title_skip_to_select.patch, skip_title_cutscene.patch, ...) that had to be applied by hand
# and broke whenever upstream moved a line. Now they are plain string replacements with exact anchors; if ANY anchor of a file is missing the
# whole file is left as upstream wrote it and CMake prints a WARNING saying what the 3DS build lost.
#
#   src/plugPikiColin/gameflow.cpp  the load heap is 8 MB on the PC port; the 3DS cannot spare it -> 1 MB (32x the GameCube's 32 KB)
#   src/plugPikiColin/titles.cpp    no opening.cin fly-over behind the title (it ran at ~4 FPS on hardware: PRESS START shows at once) and,
#                                   with nectar.ini skip_title = 1, the first title visit of a session goes straight to the file select
#
# Included from CMakeLists.txt right before add_library(pikmin_legacy_3ds ...); edits the variable PLUGPIKI_SOURCES in the caller's scope.

# Replace ANCHOR (must occur exactly once) by REPL inside the string variable VAR. On failure: records it in N3DS_PATCH_FAIL.
function(n3ds_sub_one VAR ANCHOR REPL)
  set(_c "${${VAR}}")
  string(FIND "${_c}" "${ANCHOR}" _first)
  string(FIND "${_c}" "${ANCHOR}" _last REVERSE)
  if(_first EQUAL -1 OR NOT _first EQUAL _last)
    set(N3DS_PATCH_FAIL "${N3DS_PATCH_FAIL}\n    anchor missing or not unique: ${ANCHOR}" PARENT_SCOPE)
    return()
  endif()
  string(REPLACE "${ANCHOR}" "${REPL}" _c "${_c}")
  set(${VAR} "${_c}" PARENT_SCOPE)
endfunction()

# Same, for an anchor that occurs one or more times (every occurrence is replaced).
function(n3ds_sub_all VAR ANCHOR REPL)
  set(_c "${${VAR}}")
  string(FIND "${_c}" "${ANCHOR}" _first)
  if(_first EQUAL -1)
    set(N3DS_PATCH_FAIL "${N3DS_PATCH_FAIL}\n    anchor missing: ${ANCHOR}" PARENT_SCOPE)
    return()
  endif()
  string(REPLACE "${ANCHOR}" "${REPL}" _c "${_c}")
  set(${VAR} "${_c}" PARENT_SCOPE)
endfunction()

# Read SRC with LF line endings (the copy is a build artifact; the compiler does not care), patch it with the edits in the caller, write OUT.
macro(n3ds_patch_begin SRC)
  set(N3DS_PATCH_FAIL "")
  file(READ "${SRC}" N3DS_PATCH_TEXT)
  string(REPLACE "\r\n" "\n" N3DS_PATCH_TEXT "${N3DS_PATCH_TEXT}")
endmacro()

macro(n3ds_patch_end SRC OUT WHAT LOSS)
  if(N3DS_PATCH_FAIL STREQUAL "")
    n3ds_write_if_changed("${OUT}" "${N3DS_PATCH_TEXT}")
    list(REMOVE_ITEM PLUGPIKI_SOURCES "${SRC}")
    list(APPEND PLUGPIKI_SOURCES "${OUT}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SRC}")
    message(STATUS "3DS source patch: ${WHAT}")
  else()
    message(WARNING "3DS source patch NOT applied to ${SRC} (upstream changed?): ${LOSS}${N3DS_PATCH_FAIL}")
  endif()
endmacro()

# ---- gameflow.cpp: load heap -------------------------------------------------------------------------------------------------------
set(_gf "${NECTAR_ROOT}/src/plugPikiColin/gameflow.cpp")
if(EXISTS "${_gf}")
  n3ds_patch_begin("${_gf}")
  n3ds_sub_one(N3DS_PATCH_TEXT "int loadHeapSize = 0x800000; // 8MB" "int loadHeapSize = 0x100000; // 1MB (3DS: n3ds_src_patches.cmake)")
  n3ds_patch_end("${_gf}" "${CMAKE_CURRENT_BINARY_DIR}/n3ds_gameflow_patched.cpp" "load heap 8 MB -> 1 MB"
                 "the load heap stays at 8 MB (about 7 MB less free memory)")
endif()

# ---- titles.cpp: no title cutscene, optional jump to the file select ------------------------------------------------------------------
set(_tt "${NECTAR_ROOT}/src/plugPikiColin/titles.cpp")
if(EXISTS "${_tt}")
  n3ds_patch_begin("${_tt}")
  n3ds_sub_one(N3DS_PATCH_TEXT "class TitleSetupSection;"
"#if defined(__3DS__)
/// n3ds_port/platform/n3ds_config.cpp (nectar.ini: skip_title). 1 = the first time the title section runs, go straight to the file select.
extern \"C\" int n3ds_skip_title_to_select(void);
#endif

class TitleSetupSection;")
  # no opening.cin: mPlayer stays null and every use of it is guarded
  n3ds_sub_one(N3DS_PATCH_TEXT "mPlayer             = new CinematicPlayer(\"cinemas/opening.cin\");"
"#if defined(__3DS__)
		mPlayer             = nullptr; // 3DS: the fly-over costs ~200 ms/frame there, so it is left out
#else
		mPlayer             = new CinematicPlayer(\"cinemas/opening.cin\");
#endif")
  n3ds_sub_one(N3DS_PATCH_TEXT "mPlayer->mWorldMtx.makeIdentity();" "if (mPlayer) mPlayer->mWorldMtx.makeIdentity();")
  n3ds_sub_one(N3DS_PATCH_TEXT "mPlayer->mIsPlaying = true;" "if (mPlayer) mPlayer->mIsPlaying = true;")
  n3ds_sub_all(N3DS_PATCH_TEXT "mPlayer->mPlaybackSpeed = 30.0f;" "if (mPlayer) mPlayer->mPlaybackSpeed = 30.0f;")
  n3ds_sub_all(N3DS_PATCH_TEXT "mPlayer->update();" "if (mPlayer) mPlayer->update();")
  n3ds_sub_one(N3DS_PATCH_TEXT "if (mPlayer->mCurrentData) {" "if (mPlayer && mPlayer->mCurrentData) {")
  n3ds_sub_one(N3DS_PATCH_TEXT "mPlayer->addLights(gfx);" "if (mPlayer) mPlayer->addLights(gfx);")
  n3ds_sub_one(N3DS_PATCH_TEXT "mPlayer->refresh(gfx);" "if (mPlayer) mPlayer->refresh(gfx);")
  n3ds_sub_one(N3DS_PATCH_TEXT "{ mPlayer->genAge(server); }" "{ if (mPlayer) mPlayer->genAge(server); }")
  # nothing else sends TITLECMD_ShowStartMenu without the cutscene, so show PRESS START at once (and never start the idle tutorial movies)
  n3ds_sub_one(N3DS_PATCH_TEXT "startWindow   = new zen::ogScrStartMgr;\n\t\tstartWindowOn = false;"
"startWindow   = new zen::ogScrStartMgr;
		startWindowOn = false;
#if defined(__3DS__)
		if (!mPlayer) {
			startWindow->start();
			startWindowOn   = true;
			mIdleMovieTimer = 1.0e9f;
		}
#endif")
  # skip_title = 1: leave exactly as choosing STORY MODE does (GameSetup -> CardSelect = the file select), once per session
  n3ds_sub_one(N3DS_PATCH_TEXT "mController->update();"
"mController->update();

#if defined(__3DS__)
		// 3DS: no title screen. The first time this section runs in a session, leave exactly as choosing STORY MODE in the title menu would:
		// GameSetup -> CardSelect (the file select). Later visits (e.g. backing out of the file select) show the normal PRESS START screen.
		{
			static bool sJumped = false;
			if (!sJumped && mState == STATE_Active && n3ds_skip_title_to_select()) {
				sJumped           = true;
				mNextSectionsFlag = PACK_NEXT_ONEPLAYER(ONEPLAYER_GameSetup);
				Jac_SceneExit(SCENE_Exit, 0);
				mState = STATE_ExitPending;
				gsys->setFade(0.0f);
			}
		}
#endif")
  n3ds_patch_end("${_tt}" "${CMAKE_CURRENT_BINARY_DIR}/n3ds_titles_patched.cpp" "title screen without opening.cin, optional skip to the file select"
                 "the title plays the slow opening.cin background (~4 FPS) and nectar.ini skip_title has no effect")
endif()
