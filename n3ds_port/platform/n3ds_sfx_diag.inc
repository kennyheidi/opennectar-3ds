/* ---- 3DS diagnostics, spliced into the build-time copy of audio_stubs.cpp just before Jac_Start() (see n3ds_port/CMakeLists.txt) ----
   Answers "which sound effects are lost and where": a request can be (1) never made, (2) swallowed by the game's own demo / menu / pause gate
   (audio_demo_active(), sMenuOrPauseActive: while one is stuck on, every Pikmin voice and many effects are silent), (3) refused by the sound
   engine (the SE sequencer is not running), (4) accepted but dropped for want of a voice / wave (the engine's own "[PC Audio] DROPPED" line, needs
   nectar.ini audio_stats = 1). n3ds_window.cpp prints the counters every 300 frames as a "[sfx]" line. */
void n3ds_log(const char* fmt, ...);
extern "C" int n3ds_audio_diag(int* out, char* misses, int missesLen);   /* appended to the build-time copy of pc_audio.cpp (n3ds_port/CMakeLists.txt) */
#ifndef N3DS_SE_QUEUE_MODE
#define N3DS_SE_QUEUE_MODE 0
#endif
struct N3dsSfxCounters {
    unsigned orimaReq, orimaDemoGate, orimaMenuGate, sysReq, sysMenuGate, sendCalls, sendRefused, logged;
    unsigned orimaReqTotal, sendRefusedTotal;
};
static N3dsSfxCounters gN3dsSfx;
static bool n3ds_sfx_send_orima(u16 id, bool stop, bool piki)
{
    ++gN3dsSfx.sendCalls;
    const bool ok = pc_audio_send_orima_se(id, stop, piki);
    if (!ok) { ++gN3dsSfx.sendRefused; ++gN3dsSfx.sendRefusedTotal; }
    return ok;
}
static bool n3ds_sfx_send_system(u16 id, bool stop)
{
    ++gN3dsSfx.sendCalls;
    const bool ok = pc_audio_send_system_se(id, stop);
    if (!ok) { ++gN3dsSfx.sendRefused; ++gN3dsSfx.sendRefusedTotal; }
    return ok;
}
static void n3ds_sfx_note_orima(unsigned id)
{
    ++gN3dsSfx.orimaReq; ++gN3dsSfx.orimaReqTotal;
    const bool d = audio_demo_active(), m = sMenuOrPauseActive;
    if (d) ++gN3dsSfx.orimaDemoGate;
    if (m) ++gN3dsSfx.orimaMenuGate;
    if (gN3dsSfx.logged < 8) { ++gN3dsSfx.logged; n3ds_log("[sfx] request Jac_PlayOrimaSe(0x%x): demo gate %d, menu/pause gate %d", id, d ? 1 : 0, m ? 1 : 0); }
}
static void n3ds_sfx_note_system(void) { ++gN3dsSfx.sysReq; if (sMenuOrPauseActive) ++gN3dsSfx.sysMenuGate; }

/* Positional "event" sounds (Pikmin voices incl. plucking, enemies, ship ...): soundMgr.cpp -> Jac_CreateEvent() + Jac_PlayEventAction(), which are
   NOT the Jac_PlayOrimaSe path counted above. A play request can die in the game-side slot logic (inactive event, no command in the table, group/priority
   "DESCARTADA", no slot), or in the sequencer (pc_audio_send_event_action returns false). These counters are always on (no audio_stats needed). */
struct N3dsEvCounters {
    unsigned create, createFail, play, playFail, sent, sentRefused, stops, sentSinceStart, playSinceStart, playFailSinceStart, createSinceStart, createFailSinceStart;
};
static N3dsEvCounters gN3dsEv;
static bool n3ds_sfx_send_event(u8 event, u8 slot, u16 command, bool stop)
{
    if (stop) { ++gN3dsEv.stops; return pc_audio_send_event_action(event, slot, command, stop); }
    ++gN3dsEv.sent; ++gN3dsEv.sentSinceStart;
    const bool ok = pc_audio_send_event_action(event, slot, command, stop);
    if (!ok) ++gN3dsEv.sentRefused;
    return ok;
}
static void n3ds_sfx_note_create(int r) { ++gN3dsEv.create; ++gN3dsEv.createSinceStart; if (r < 0) { ++gN3dsEv.createFail; ++gN3dsEv.createFailSinceStart; } }
static void n3ds_sfx_note_play(bool ok) { ++gN3dsEv.play; ++gN3dsEv.playSinceStart; if (!ok) { ++gN3dsEv.playFail; ++gN3dsEv.playFailSinceStart; } }

void n3ds_sfx_report(void)
{
    n3ds_log("[sfx] events (positional sounds: Pikmin voices, plucking, enemies ...), last window: Jac_CreateEvent %u (failed %u) | Jac_PlayEventAction %u (refused by the game-side slot logic or sequencer %u) | sent to the sequencer %u (sequencer refused %u), stop commands %u | free event slots now %d | since start: create %u/%u failed, play %u/%u refused, sent %u",
             gN3dsEv.create, gN3dsEv.createFail, gN3dsEv.play, gN3dsEv.playFail, gN3dsEv.sent, gN3dsEv.sentRefused, gN3dsEv.stops, (int)Jac_CheckFreeEvents(),
             gN3dsEv.createSinceStart, gN3dsEv.createFailSinceStart, gN3dsEv.playSinceStart, gN3dsEv.playFailSinceStart, gN3dsEv.sentSinceStart);
    gN3dsEv.create = gN3dsEv.createFail = gN3dsEv.play = gN3dsEv.playFail = gN3dsEv.sent = gN3dsEv.sentRefused = gN3dsEv.stops = 0;
    PCAudioMetrics m = {}; pc_audio_get_metrics(&m);
    n3ds_log("[sfx] last window: orima/piki requests %u (while demo gate closed %u, while menu/pause gate closed %u), system requests %u (menu/pause %u), engine refused %u of %u | gates now: demo=%d [current demo %d, parts-find %d, text %d] menu=%d pause=%d dvd=%d | voices: peak %u, active %u, stolen %u, REJECTED (no free voice) %u | requests/refusals since start %u/%u | SE port queue: %s",
             gN3dsSfx.orimaReq, gN3dsSfx.orimaDemoGate, gN3dsSfx.orimaMenuGate, gN3dsSfx.sysReq, gN3dsSfx.sysMenuGate, gN3dsSfx.sendRefused, gN3dsSfx.sendCalls,
             audio_demo_active() ? 1 : 0, sCurrentDemo, sPartsFindDemoActive ? 1 : 0, sTextDemoActive ? 1 : 0, sMenuActive ? 1 : 0, sPauseActive ? 1 : 0, sDVDPauseActive ? 1 : 0,
             (unsigned)m.peakActiveVoices, (unsigned)pc_audio_get_active_voice_count(), (unsigned)m.voiceSteals, (unsigned)m.voiceRejects,
             gN3dsSfx.orimaReqTotal, gN3dsSfx.sendRefusedTotal, N3DS_SE_QUEUE_MODE ? "applied from n3ds_port/audio_dropin" : "already in your pc_port/audio");
    {   /* always-on engine counters (no audio_stats needed): sounds the engine accepted but could not play, plus how often its sequencers really ran */
        int d[12] = {}; char miss[320] = "";
        const int have = n3ds_audio_diag(d, miss, (int)sizeof miss);
        static unsigned long long lastSe = 0, lastEv = 0, lastBgm = 0;
        const unsigned long long se = m.seTicks, ev = m.eventTicks, bgm = m.bgmTicks;
        if (have)
            n3ds_log("[sfx] engine drops since start: no instrument %d, no wave system %d, wave missing %d (%d exist in another scene), WAVE DECODE FAILED %d (last one %d samples), silenced oscillator-type instruments %d (last program 0x%x) | decoded-wave cache: %d waves (%d of them low-passed to the output rate), %d KB | sequencer ticks this window: SE %llu, event %llu, bgm %llu | first missing waves:%s",
                     d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[10], d[9], se - lastSe, ev - lastEv, bgm - lastBgm, miss[0] ? miss : " none");
        else
            n3ds_log("[sfx] engine drops: n/a in this build (pc_audio.cpp did not match the patch) | sequencer ticks this window: SE %llu, event %llu, bgm %llu", se - lastSe, ev - lastEv, bgm - lastBgm);
        lastSe = se; lastEv = ev; lastBgm = bgm;
    }
    gN3dsSfx.orimaReq = gN3dsSfx.orimaDemoGate = gN3dsSfx.orimaMenuGate = gN3dsSfx.sysReq = gN3dsSfx.sysMenuGate = gN3dsSfx.sendCalls = gN3dsSfx.sendRefused = 0;
    gN3dsSfx.logged = 4;   /* a few single requests per window are enough to see which ids arrive */
}
#define pc_audio_send_orima_se n3ds_sfx_send_orima
#define pc_audio_send_system_se n3ds_sfx_send_system
#define pc_audio_send_event_action n3ds_sfx_send_event
