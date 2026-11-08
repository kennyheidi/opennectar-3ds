# n3ds_audio_opt.cmake - build-time patches for the sound engine (included from n3ds_port/CMakeLists.txt, shares its variables).
#
# Needs: _pa (the text of pc_audio.cpp, patched so far), N3DS_PCJAM_SRC, PC_PORT_DIR, CMAKE_CURRENT_BINARY_DIR.
# Sets : _pa (updated), N3DS_PCJAM_SRC (a patched copy in the build dir), N3DS_IBANK_SRC (a patched copy of pc_instrument_bank.cpp).
#
# Every pattern is a single line (CRLF checkouts match too). If a pattern is missing the patch is skipped with a WARNING - never a build failure.
#
# 1) PLUCKING (and every other percussion sound) WAS SILENT. pc_instrument_bank.cpp gives percussion regions a built-in envelope whose only
#    point is { curve 0x0E, time 0, value 32767 }. Curve 0x0E means "hold the current value"; the envelope starts at 0, so it held 0 for
#    ever: the note was "playing" at volume 0. Pulling a Pikmin out (orima sounds 3 and 4, and the Pikmin voices 0x0B, 0x0D-0x14 that go with
#    it) are percussion programs, as are orima 0x0F-0x19. Fixed with a first point { curve 0, time 0, value 32767 } = jump to full volume,
#    then the hold.
# 2) SEQUENCER COST. Every sequencer tick re-published the volume/pan/pitch of EVERY sounding note (a VoiceUpdate event), and each of those
#    cost an eventMix() (walks the track's ancestors, one powf per node), then a mixer-lock round trip and a powf + expf in
#    pc_audio_update_wave(). That is most of pc_audio_tick()'s 3-8 ms per frame on the console. Same results, less work:
#    - eventMix() is computed once per track per tick instead of once per sounding note,
#    - the 2^(4x) pitch factor is remembered per input value,
#    - pc_audio_update_wave() returns at once when the voice already has exactly these values.
# 3) PER-TICK OVERHEAD (sequencers run ~100 ticks per frame, per player): the voice updates are only published on the LAST tick of a frame's burst
#    (the mixer only ever sees the final values; the ones in between were overwritten before it ran), finished voices are looked for once per burst
#    and only when a handle is alive, and tracks that are not active are skipped before the interpreter is called.

set(N3DS_IBANK_SRC "${PC_PORT_DIR}/audio/pc_instrument_bank.cpp")

# ---- 1) percussion envelope ----
file(READ "${PC_PORT_DIR}/audio/pc_instrument_bank.cpp" _ib)
set(_ib1 "envelope.attack.push_back({ 0x0E, 0, 32767 });")
string(FIND "${_ib}" "${_ib1}" _ibf)
if(NOT _ibf EQUAL -1)
  string(REPLACE "${_ib1}" "envelope.attack.push_back({ 0, 0, 32767 }); envelope.attack.push_back({ 0x0E, 0, 0 });   /* n3ds: jump to full volume, THEN hold (the lone hold point kept percussion at volume 0) */" _ib "${_ib}")
  set(N3DS_IBANK_SRC "${CMAKE_CURRENT_BINARY_DIR}/n3ds_pc_instrument_bank_patched.cpp")
  n3ds_write_if_changed("${N3DS_IBANK_SRC}" "${_ib}")
  message(STATUS "sound: percussion envelope fixed (plucking and the other percussion sounds were silent)")
else()
  message(WARNING "pc_instrument_bank.cpp does not match the percussion-envelope patch (already fixed upstream, or changed): plucking stays as it was")
endif()

# ---- 2a) pc_audio_update_wave: skip when nothing changed ----
set(_ou1 "    pitch = std::clamp(pitch, 0.125f, 8.0f);")
string(FIND "${_pa}" "${_ou1}" _ouf)
if(NOT _ouf EQUAL -1)
  string(REPLACE "${_ou1}" "${_ou1}
    {   /* n3ds: this voice (the handle includes its generation) already has exactly these values: nothing to do. The voice fields are only ever written by this function and at note start. */
        struct N3dsLastUpdate { int handle; float volume, pan, pitch, fxMix, dolby; u8 cutoff; };
        static N3dsLastUpdate sN3dsLast[kSampleVoiceCount];   /* zero-initialised: pitch >= 0.125 here, so an empty entry never matches */
        N3dsLastUpdate& last = sN3dsLast[voiceIndex];
        if (last.handle == voiceHandle && last.volume == volume && last.pan == pan && last.pitch == pitch
            && last.fxMix == fxMix && last.dolby == dolby && last.cutoff == cutoff) return;
        last.handle = voiceHandle; last.volume = volume; last.pan = pan; last.pitch = pitch;
        last.fxMix = fxMix; last.dolby = dolby; last.cutoff = cutoff;
    }" _pa "${_pa}")
else()
  message(WARNING "pc_audio.cpp does not match the voice-update patch: pc_audio_tick() keeps its old cost")
endif()

# ---- 2b) pc_jam.cpp: eventMix once per track per tick, remembered 2^(4x) ----
file(READ "${N3DS_PCJAM_SRC}" _jm)
set(_j1 "void PCJamPlayer::eventMix(size_t index, PCJamEvent& event) const {")
set(_j2 "        const float localPitch = std::pow(2.0f, 4.0f * node.parameters[1].current);")
set(_j3 "    event.envelope = mTracks[index].adsr;")
set(_j4 "    // all tracks have advanced this tick.")
set(_j5 "    if (!active()) mResult = PCJamResult::Finished;")
set(_j6 "        const PCJamResult result = runTrack(i, events, operations, maxOperations);")
string(FIND "${_jm}" "${_j1}" _jf1)
string(FIND "${_jm}" "${_j2}" _jf2)
string(FIND "${_jm}" "${_j3}" _jf3)
string(FIND "${_jm}" "${_j4}" _jf4)
string(FIND "${_jm}" "${_j5}" _jf5)
string(FIND "${_jm}" "${_j6}" _jf6)
if(NOT _jf1 EQUAL -1 AND NOT _jf2 EQUAL -1 AND NOT _jf3 EQUAL -1 AND NOT _jf4 EQUAL -1 AND NOT _jf5 EQUAL -1 AND NOT _jf6 EQUAL -1)
  string(REPLACE "${_j2}" "        const float localPitch = n3dsPow2x4(node.parameters[1].current);" _jm "${_jm}")
  string(REPLACE "${_j3}" "${_j3}
    if (sN3dsMixMemo.on) { sN3dsMixMemo.p = this; sN3dsMixMemo.index = index; sN3dsMixMemo.ev = event; }" _jm "${_jm}")
  string(REPLACE "${_j4}" "${_j4}
    if (!gN3dsPubUpd) { if (!active()) mResult = PCJamResult::Finished; return mResult; }   /* n3ds: not the last tick of this frame's burst: nobody would see these updates */
    sN3dsMixMemo.on = true; sN3dsMixMemo.p = nullptr;" _jm "${_jm}")
  string(REPLACE "${_j6}" "        if (!track.active) continue;   /* n3ds: skip the call (it was a no-op for an inactive track) */
${_j6}" _jm "${_jm}")
  string(REPLACE "${_j5}" "    sN3dsMixMemo.on = false;
${_j5}" _jm "${_jm}")
  string(REPLACE "${_j1}" "/* n3ds: true = publish the VoiceUpdate events of the sounding notes at the end of this tick (pc_audio_tick sets it false on all but the last tick of a burst) */
bool gN3dsPubUpd = true;
/* n3ds: 2^(4x) remembered per input value (same function, same result) */
static inline float n3dsPow2x4(float x) {
    struct Entry { unsigned key; float result; bool used; };
    static Entry table[256];
    unsigned bits; __builtin_memcpy(&bits, &x, sizeof bits);
    Entry& e = table[(bits ^ (bits >> 9) ^ (bits >> 17)) & 255u];
    if (e.used && e.key == bits) return e.result;
    const float r = std::pow(2.0f, 4.0f * x);
    e.key = bits; e.result = r; e.used = true;
    return r;
}
/* n3ds: while a tick publishes the VoiceUpdate events of all sounding notes, the second and later notes of one track reuse the first one's mix */
struct N3dsMixMemo { bool on = false; const void* p = nullptr; size_t index = 0; PCJamEvent ev; };
static N3dsMixMemo sN3dsMixMemo;
${_j1}
    if (sN3dsMixMemo.on && sN3dsMixMemo.p == this && sN3dsMixMemo.index == index) {
        const PCJamEvent& m = sN3dsMixMemo.ev;
        event.volume = m.volume; event.pan = m.pan; event.pitch = m.pitch; event.fxMix = m.fxMix; event.dolby = m.dolby;
        event.source = m.source; event.cutoff = m.cutoff; event.envelope = m.envelope;
        return;
    }" _jm "${_jm}")
  set(N3DS_PCJAM_SRC "${CMAKE_CURRENT_BINARY_DIR}/n3ds_pc_jam_patched.cpp")
  n3ds_write_if_changed("${N3DS_PCJAM_SRC}" "${_jm}")

  # ---- 3) pc_audio.cpp side: publish only on the last tick of the burst, reap once per burst, cheap reap scan ----
  set(_pk_ok ON)
  function(n3ds_need_anchor text anchor)   # sets _pk_ok OFF in the parent when the anchor text is missing
    string(FIND "${text}" "${anchor}" _af)
    if(_af EQUAL -1)
      set(_pk_ok OFF PARENT_SCOPE)
    endif()
  endfunction()
  n3ds_need_anchor("${_pa}" "static PCJamPlayer sJamPlayer;")
  n3ds_need_anchor("${_pa}" "static void reap_finished_voices(PCJamPlayer& player, int (&voices)[N][8]) {")
  n3ds_need_anchor("${_pa}" "            reap_finished_voices(sJamPlayer, sJamVoices);")
  n3ds_need_anchor("${_pa}" "            reap_finished_voices(sBossJamPlayer, sBossJamVoices);")
  n3ds_need_anchor("${_pa}" "            reap_finished_voices(sSEJamPlayer, sSEJamVoices);")
  n3ds_need_anchor("${_pa}" "            reap_finished_voices(sEventJamPlayer, sEventJamVoices);")
  n3ds_need_anchor("${_pa}" "            const PCJamResult result = sJamPlayer.tick(sJamEvents, 4096);")
  n3ds_need_anchor("${_pa}" "            const PCJamResult result = sBossJamPlayer.tick(sBossJamEvents, 4096);")
  n3ds_need_anchor("${_pa}" "            const PCJamResult result = sSEJamPlayer.tick(sSEJamEvents, 4096);")
  n3ds_need_anchor("${_pa}" "            const PCJamResult result = sEventJamPlayer.tick(sEventJamEvents, 4096);")
  if(_pk_ok)
    string(REPLACE "static PCJamPlayer sJamPlayer;" "extern bool gN3dsPubUpd;   /* n3ds: defined in the patched pc_jam.cpp */
static PCJamPlayer sJamPlayer;" _pa "${_pa}")
    string(REPLACE "static void reap_finished_voices(PCJamPlayer& player, int (&voices)[N][8]) {" "static void reap_finished_voices(PCJamPlayer& player, int (&voices)[N][8]) {
    {   /* n3ds: nothing to do when no handle is alive (the AND of a track's 8 handles keeps its sign bit only if all are negative). Only this thread writes the handles. */
        bool any = false;
        for (size_t t = 0; t < N; ++t) {
            const int* v = voices[t];
            if ((v[0] & v[1] & v[2] & v[3] & v[4] & v[5] & v[6] & v[7]) >= 0) { any = true; break; }
        }
        if (!any) return;
    }" _pa "${_pa}")
    foreach(_pl "sJamPlayer, sJamVoices" "sBossJamPlayer, sBossJamVoices" "sSEJamPlayer, sSEJamVoices" "sEventJamPlayer, sEventJamVoices")
      string(REPLACE "            reap_finished_voices(${_pl});" "            if (ticks == 1) reap_finished_voices(${_pl});   /* n3ds: once per burst */" _pa "${_pa}")
    endforeach()
    foreach(_pl "sJamPlayer;sJamTickAccumulator;sJamEvents" "sBossJamPlayer;sBossJamTickAccumulator;sBossJamEvents" "sSEJamPlayer;sSEJamTickAccumulator;sSEJamEvents" "sEventJamPlayer;sEventJamTickAccumulator;sEventJamEvents")
      list(GET _pl 0 _pp)
      list(GET _pl 1 _pacc)
      list(GET _pl 2 _pev)
      string(REPLACE "            const PCJamResult result = ${_pp}.tick(${_pev}, 4096);" "            gN3dsPubUpd = (${_pacc} < 1.0);   /* n3ds: only the last tick of the burst publishes the voice updates */
            const PCJamResult result = ${_pp}.tick(${_pev}, 4096);
            gN3dsPubUpd = true;" _pa "${_pa}")
    endforeach()
  else()
    message(WARNING "pc_audio.cpp does not match the per-tick patch: the sequencers keep publishing voice updates on every tick")
    # the jam side must not skip updates then: restore the unpatched flag behaviour (gN3dsPubUpd stays true forever)
  endif()
else()
  message(WARNING "pc_jam.cpp does not match the eventMix patch: pc_audio_tick() keeps its old cost")
endif()

# ---- 4) r51: where pc_audio_tick()'s time goes: the "[audioprof]" log line (counters in n3ds_sdl_audio.cpp, report in n3ds_window.cpp) ----
#    Times the four sequencers (whole block, and the interpreter alone with the number of ticks), note starts, voice updates, finished-voice
#    scans and the BGM mix advance. Two clock reads per timed call, no strings: cheap enough to stay on. Skipped (WARNING) if a pattern is missing.
set(_ap_ok ON)
function(n3ds_ap_need anchor)
  string(FIND "${_pa}" "${anchor}" _af)
  if(_af EQUAL -1)
    set(_ap_ok OFF PARENT_SCOPE)
    message(STATUS "audio profile: pattern not found: ${anchor}")
  endif()
endfunction()
n3ds_ap_need("#include <SDL2/SDL.h>")
n3ds_ap_need("    if (sJamPlayer.result() == PCJamResult::Ok) {")
n3ds_ap_need("    if (sBossJamPlayer.result() == PCJamResult::Ok) {")
n3ds_ap_need("    if (sSEJamPlayer.result() == PCJamResult::Ok) {")
n3ds_ap_need("    if (kEnablePositionalEventJam && sEventJamPlayer.result() == PCJamResult::Ok) {")
n3ds_ap_need("const PCJamResult result = sJamPlayer.tick(sJamEvents, 4096);")
n3ds_ap_need("const PCJamResult result = sBossJamPlayer.tick(sBossJamEvents, 4096);")
n3ds_ap_need("const PCJamResult result = sSEJamPlayer.tick(sSEJamEvents, 4096);")
n3ds_ap_need("const PCJamResult result = sEventJamPlayer.tick(sEventJamEvents, 4096);")
n3ds_ap_need("                       std::shared_ptr<const std::vector<PCInstrumentOscillator>> envelope) {")
n3ds_ap_need("    pitch = std::clamp(pitch, 0.125f, 8.0f);")
n3ds_ap_need("    advance_bgm_mix();")
n3ds_ap_need("static void reap_finished_voices(PCJamPlayer& player, int (&voices)[N][8]) {")
if(_ap_ok)
  string(REPLACE "#include <SDL2/SDL.h>" "#include <SDL2/SDL.h>
/* n3ds r51: [audioprof] timers (n3ds_sdl_audio.cpp) */
extern \"C\" unsigned long long n3ds_aprof_now(void);
extern \"C\" void n3ds_aprof_add(int slot, unsigned long long t0);
struct N3dsAProf { int s; unsigned long long t0; explicit N3dsAProf(int slot) : s(slot), t0(n3ds_aprof_now()) {} ~N3dsAProf() { n3ds_aprof_add(s, t0); } };
template <class P, class E> static inline auto n3dsApTick(int slot, P& player, E& events) { const unsigned long long t0 = n3ds_aprof_now(); const auto r = player.tick(events, 4096); n3ds_aprof_add(slot, t0); return r; }" _pa "${_pa}")
  string(REPLACE "    if (sJamPlayer.result() == PCJamResult::Ok) {" "    if (sJamPlayer.result() == PCJamResult::Ok) { N3dsAProf n3dsAp(0);" _pa "${_pa}")
  string(REPLACE "    if (sBossJamPlayer.result() == PCJamResult::Ok) {" "    if (sBossJamPlayer.result() == PCJamResult::Ok) { N3dsAProf n3dsAp(1);" _pa "${_pa}")
  string(REPLACE "    if (sSEJamPlayer.result() == PCJamResult::Ok) {" "    if (sSEJamPlayer.result() == PCJamResult::Ok) { N3dsAProf n3dsAp(2);" _pa "${_pa}")
  string(REPLACE "    if (kEnablePositionalEventJam && sEventJamPlayer.result() == PCJamResult::Ok) {" "    if (kEnablePositionalEventJam && sEventJamPlayer.result() == PCJamResult::Ok) { N3dsAProf n3dsAp(3);" _pa "${_pa}")
  string(REPLACE "const PCJamResult result = sJamPlayer.tick(sJamEvents, 4096);" "const PCJamResult result = n3dsApTick(4, sJamPlayer, sJamEvents);" _pa "${_pa}")
  string(REPLACE "const PCJamResult result = sBossJamPlayer.tick(sBossJamEvents, 4096);" "const PCJamResult result = n3dsApTick(5, sBossJamPlayer, sBossJamEvents);" _pa "${_pa}")
  string(REPLACE "const PCJamResult result = sSEJamPlayer.tick(sSEJamEvents, 4096);" "const PCJamResult result = n3dsApTick(6, sSEJamPlayer, sSEJamEvents);" _pa "${_pa}")
  string(REPLACE "const PCJamResult result = sEventJamPlayer.tick(sEventJamEvents, 4096);" "const PCJamResult result = n3dsApTick(7, sEventJamPlayer, sEventJamEvents);" _pa "${_pa}")
  string(REPLACE "                       std::shared_ptr<const std::vector<PCInstrumentOscillator>> envelope) {" "                       std::shared_ptr<const std::vector<PCInstrumentOscillator>> envelope) { N3dsAProf n3dsAp(8);" _pa "${_pa}")
  string(REPLACE "    pitch = std::clamp(pitch, 0.125f, 8.0f);" "    pitch = std::clamp(pitch, 0.125f, 8.0f);
    N3dsAProf n3dsAp(9);" _pa "${_pa}")
  string(REPLACE "    advance_bgm_mix();" "    { N3dsAProf n3dsAp(12); advance_bgm_mix(); }" _pa "${_pa}")
  string(REPLACE "static void reap_finished_voices(PCJamPlayer& player, int (&voices)[N][8]) {" "static void reap_finished_voices(PCJamPlayer& player, int (&voices)[N][8]) { N3dsAProf n3dsAp(13);" _pa "${_pa}")
  message(STATUS "sound: [audioprof] timers inside pc_audio_tick() added")
else()
  message(WARNING "pc_audio.cpp does not match the [audioprof] patterns: the line will only show the whole tick and the lock waits")
endif()
