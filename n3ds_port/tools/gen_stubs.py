#!/usr/bin/env python3
"""Generate n3ds_port/platform/n3ds_stubs_gen.cpp: default no-op definitions for the PC-port feature
functions (mods, settings, prompts, gfx queries, audio hooks) that the 3DS build does not implement yet.
Prototypes are read from upstream headers, so a signature mismatch is a compile error, not a silent link bug.
Usage: python3 n3ds_port/tools/gen_stubs.py"""
import re, os, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
HDR_DIRS = [('pc_port', 'pc_port'), ('include', 'include')]
names = [l.strip() for l in open(os.path.join(ROOT, 'n3ds_port/tools/stub_symbols.txt')) if l.strip()]

# Bodies where "return zero" would be WRONG for a vanilla single-player game. a0,a1,... are the parameters.
OVERRIDES = {
  'pc_hardmode_teki_life': 'return a0;', 'pc_hardmode_navi_damage': 'return a0;', 'pc_mods_teki_damage': 'return a0;',
  'pc_captain_piki_color': 'return -1;',
  # Normal (non-netplay) play: every cutscene joint/event/key store is guarded by this, so it MUST be 1 (upstream default).
  'pc_netplay_present_sim_side': 'return 1;',
  'pc_coop_p2_tint': '*a0 = 255; *a1 = 255; *a2 = 255;',
  # Prompts: auto-accept so cardSelect.cpp never waits forever (1 = ONE player / NORMAL game / OK).
  'pc_playercount_prompt_result': 'return 1;', 'pc_newgame_prompt_result': 'return 1;',
  'pc_devassign_prompt_result': 'return 1;', 'pc_captain_prompt_result': 'return 1;', 'pc_vsrules_prompt_result': 'return 1;',
  # Settings defaults = upstream PcConfig member initialisers (all mods off).
  'pc_settings_get_navi_health_pct': 'return 100;', 'pc_settings_get_whistle_radius_pct': 'return 100;',
  'pc_settings_get_piki_limit': 'return 100;', 'pc_settings_get_day_minutes': 'return 10;',
  'pc_settings_get_throw_speed_scale': 'return 1.0f;', 'pc_settings_get_carry_speed_scale': 'return 1.0f;',
  'pc_settings_get_navi_speed_scale': 'return 1.0f;',
  # Graphics queries: GameCube 4:3, HUD virtual size 0 = automatic.
  'pc_gfx_get_current_aspect_ratio': 'return 4.0f / 3.0f;', 'pc_gfx_get_window_aspect_ratio': 'return 4.0f / 3.0f;',
  'pc_gfx_get_ui_43': 'return 1;',
  'pc_gfx_get_proj_offset': 'if (a0) *a0 = 0.0f; if (a1) *a1 = 0.0f;',
  # ARAM: audio is off, so don't burn 16 MiB of a tiny heap on a host copy of it.
  'pc_aram_init': 'return true;',
  # Returns a reference: must point at storage that outlives the call (default-constructed = mods off).
  'pc_vs_rules': 'static PcVsRules rules{}; return rules;',
  # Called first thing in every System::run iteration: our loop heartbeat for the watchdog/trace.
  'Jac_Gsync': 'n3ds_diag_loop();',
  # ---- 0.9.3 additions (netplay / speedrun / randomizer / days / VS hooks, all inert in a vanilla single-player game) ----
  'pc_sim_rand': 'return rand();', 'pc_cosmetic_rand': 'return rand();',          # "rand() when not deterministic" (pc_sim_rng.h)
  'pc_netplay_sim_visible': 'return a0;', 'pc_netplay_sim_lod_distance': 'return a0;',   # pass-through when no netplay session
  'pc_netplay_fixed_dt': 'return a0 >= 2 ? 1.0f / 30.0f : (a0 == 1 ? 1.0f / 60.0f : 1.0f / 120.0f);',
  'pc_netplay_camlead_view': 'return a1;', 'pc_netplay_camlead_control_camera': 'return a1;',
  'pc_netplay_camlead_drag_route': 'return -1;',   # -1 = keep the per-slot camera drag routing
  'pc_speedrun_card_subdir': 'return "";', 'pc_netplay_det_profile_path': 'return nullptr;',
  'pc_randomizer_rules': 'static PcRandomizerRules rules{}; return rules;',
  'pc_input_log_yaw_quantise': 'return 0;',
  'pc_captain_skeleton_color': 'return -1;', 'pc_captain_pikmin2_model': 'return -1;',   # -1 = Olimar is drawn normally
  # Called once per frame from System::run (like pc_window.cpp): advances the BGM/SE/event sequencers. Without it only direct one-shot waves make sound.
  'pc_window_poll_events': 'n3ds_audio_game_pulse(); n3ds_prof_begin("run", "pc_audio_tick()"); pc_audio_tick(); n3ds_prof_end();',
  # Window/input
  'pc_window_get_virtual_cursor_x': 'return 320;', 'pc_window_get_virtual_cursor_y': 'return 240;',
  'pc_window_message_control_label': 'if (a1 && a2) { a1[0] = (a0 >= \'a\' && a0 <= \'z\') ? (char)(a0 - 32) : a0; if (a2 > 1) a1[1] = 0; }',
}
KW = {'int','long','char','short','float','double','bool','unsigned','signed','void','f32','f64','u8','u16','u32','u64','s8','s16','s32','s64','size_t'}

def strip_comments(t):
    t = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), t, flags=re.S)
    return re.sub(r'//[^\n]*', '', t)

heads = {}
for d, inc in HDR_DIRS:
    for dp, dn, fn in os.walk(os.path.join(ROOT, d)):
        if any(x in dp for x in ('android', 'touch', 'third_party', '/tools')): continue
        for f in fn:
            if f.endswith('.h'):
                p = os.path.join(dp, f)
                heads[p] = (strip_comments(open(p, errors='ignore').read()), os.path.relpath(p, os.path.join(ROOT, inc)))

def split_params(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch in '(<[': depth += 1
        if ch in ')>]': depth -= 1
        if ch == ',' and depth == 0: out.append(cur.strip()); cur = ''
        else: cur += ch
    if cur.strip(): out.append(cur.strip())
    return out

def clean_param(p, i):
    p = re.sub(r'\s*=\s*[^,]+$', '', p).strip()
    if '(' in p or '[' in p: return p, None   # function pointer / array: keep raw
    toks = re.findall(r'[A-Za-z_]\w*|[*&]+', p)
    last = toks[-1] if toks else ''
    if len(toks) >= 2 and re.match(r'[A-Za-z_]\w*$', last) and last not in KW and last != 'const':
        p = p[:p.rfind(last)].rstrip()
    return f'{p} a{i}', f'a{i}'

found, missing = {}, []
for n in names:
    hit = None
    for p, (txt, rel) in sorted(heads.items(), key=lambda kv: (0 if kv[0].startswith(ROOT + '/pc_port') or '/jaudio/' in kv[0] else 1, kv[0])):
        for m in re.finditer(r'(?<![\w:])' + re.escape(n) + r'\s*\(', txt):
            ls = txt.rfind('\n', 0, m.start()) + 1
            prefix = txt[ls:m.start()]
            if re.search(r'\b(return|else|if|while)\b|[=(]|->|\.', prefix): continue
            i = m.end(); depth = 1
            while i < len(txt) and depth:
                depth += (txt[i] == '(') - (txt[i] == ')'); i += 1
            params = txt[m.end():i - 1]
            tail = txt[i:i + 4].lstrip()
            if not tail.startswith(';'): continue
            ret = re.sub(r'\b(extern|static|inline|constexpr|virtual)\b|"C"', '', prefix).strip()
            if not ret: continue
            hit = (ret, params, rel); break
        if hit: break
    if hit: found[n] = hit
    else: missing.append(n)

incs = sorted({h[2] for h in found.values()})
out = ['// GENERATED by n3ds_port/tools/gen_stubs.py -- do not edit. Default (mods-off) definitions of the PC-port',
       '// feature hooks the 3DS build does not implement. Regenerate after changing tools/stub_symbols.txt.',
       '#include <stddef.h>', 'extern "C" void n3ds_diag_loop(void);', '#include "system.h"', '#include "Graphics.h"', '#include "Vector.h"']
out += [f'#include "{i}"' for i in incs]
out += ['#include "audio/pc_audio.h"', 'extern "C" void n3ds_audio_game_pulse(void);',
        'extern "C" void n3ds_prof_begin(const char* group, const char* name); extern "C" void n3ds_prof_end(void);']
out.append('')
for n in sorted(found):
    ret, params, rel = found[n]
    plist = split_params(params) if params.strip() not in ('', 'void') else []
    ps, names_ = [], []
    for i, p in enumerate(plist):
        cp, nm = clean_param(p, i); ps.append(cp)
    sig = ', '.join(ps) if ps else ''
    body = OVERRIDES.get(n)
    if body is None:
        if ret.strip().endswith('&'):
            sys.exit(f'ERROR: {n} returns a reference ({ret}); "return {{}};" would dangle. Add an OVERRIDES entry with static storage.')
        body = '' if ret.strip() == 'void' else 'return {};'
    out.append(f'{ret} {n}({sig}) {{ {body} }}')
# Overloads / classes the prototype scanner cannot express (it takes the first declaration it finds).
out += ['#include "randomizer/pc_randomizer.h"', '#include "settings/pc_settings_p2d.h"',
        'void pc_permadeath_write_block(RandomAccessStream& a0, bool a1, bool a2, const PcRandomizerRules& a3) {  }   // 4-argument overload (0.9.x randomizer)',
        'PcSettingsP2DFrame::PcSettingsP2DFrame(int, int) {  }', 'PcSettingsP2DFrame::~PcSettingsP2DFrame() {  }']
open(os.path.join(ROOT, 'n3ds_port/platform/n3ds_stubs_gen.cpp'), 'w').write('\n'.join(out) + '\n')
print(f'generated {len(found)} stubs; no prototype found for {len(missing)}:')
print('\n'.join('  ' + m for m in missing))
