#include "n3ds_tev.h"
#include <string.h>

namespace {

struct Arg {
    bool zero = false, one = false;       // constant 0 / 1
    uint8_t src = N3DS_TS_PREV, op = N3DS_TO_C;
    uint32_t k = 0; bool hasK = false;    // for src == CONST: the constant it stands for
};

inline uint32_t grey(uint32_t v) { return v | v << 8 | v << 16 | 0xFF000000u; }

uint32_t comp(uint32_t c, int i) { return (c >> (8 * i)) & 255; }

// GX_TEV_KCSEL_* -> constant RGB (alpha ignored)
uint32_t kcolorSel(int sel, const uint32_t k[4])
{
    if (sel >= 0 && sel < 8) return grey(255 * (8 - sel) / 8);
    if (sel >= 12 && sel <= 15) return k[sel - 12] | 0xFF000000u;
    if (sel >= 16 && sel <= 31) return grey(comp(k[(sel - 16) & 3], (sel - 16) >> 2));
    return grey(255);
}
// GX_TEV_KASEL_* -> constant alpha 0..255
uint32_t kalphaSel(int sel, const uint32_t k[4])
{
    if (sel >= 0 && sel < 8) return 255 * (8 - sel) / 8;
    if (sel >= 16 && sel <= 31) return comp(k[(sel - 16) & 3], (sel - 16) >> 2);
    return 255;
}

struct Ctx {
    const N3dsTevIn* in; int stage; bool haveKC, haveKA; uint32_t kc, ka; int approx;
    bool prevValid = false;                                // a stage has already been emitted, so PREVIOUS means something
    bool cz[4] = {false, false, false, false};             // lenient mode: colour / alpha of TEV register i was written by a dropped stage -> reads as 0
    bool az[4] = {false, false, false, false};
    int bufC = -1, bufA = -1;                              // r48: TEV register (1..3) whose colour / alpha the PICA combiner buffer holds
    int bufCStage = -9, bufAStage = -9;                    // r49: PICA stage that produced it; the buffer shows it only from that stage + 2
    int curStage = 0;                                      // r49: PICA stage index the arguments are being resolved for
};

// PICA texture unit (0..2) that GX texture map `map` is bound to, or -1.
int unitOf(const N3dsTevIn& in, int map)
{
    if (map < 0 || map >= 8) return -1;
    if (in.nUnits > 0) { for (int u = 0; u < in.nUnits && u < 3; ++u) if (in.unitMap[u] == map) return u; return -1; }
    return in.primaryMap >= 0 && map == in.primaryMap ? 0 : -1;
}
inline uint8_t texSrc(int unit) { return unit == 0 ? N3DS_TS_TEX0 : unit == 1 ? N3DS_TS_TEX1 : N3DS_TS_TEX2; }

// A constant that is exactly 0 or exactly 1 (per the channel it is read as) is the same as GX's ZERO / ONE argument; saying so lets
// solve() use the exact one-stage forms (e.g. lerp(black, K, x) = K * x) instead of spending a constant slot.
void normConst(Arg& r)
{
    if (r.src != N3DS_TS_CONST || !r.hasK || r.zero || r.one) return;
    const uint32_t v = (r.op == N3DS_TO_A || r.op == N3DS_TO_1MA) ? (r.k >> 24) : (r.k & 0x00FFFFFFu);
    const uint32_t full = (r.op == N3DS_TO_A || r.op == N3DS_TO_1MA) ? 255u : 0x00FFFFFFu;
    if (v == 0) { r.zero = true; r.hasK = false; }
    else if (v == full) { r.one = true; r.hasK = false; }
}

Arg colorArgRaw(int a, Ctx& c)
{
    Arg r; const N3dsTevIn& in = *c.in; const N3dsTevRaw& s = in.st[c.stage];
    const int unit = unitOf(in, s.map);
    switch (a) {
    case 0: case 1:                                        // CPREV / APREV
        if (c.prevValid) { r.src = N3DS_TS_PREV; r.op = a == 0 ? N3DS_TO_C : N3DS_TO_A; }
        else { r.src = N3DS_TS_CONST; r.hasK = true; r.k = in.reg[0]; r.op = a == 0 ? N3DS_TO_C : N3DS_TO_A; }
        break;
    case 2: case 3: case 4: case 5: case 6: case 7: {      // C0 A0 C1 A1 C2 A2
        const int ri = 1 + (a - 2) / 2;
        /* r48: written by an earlier stage, kept in the combiner buffer. r49 (white Wollywog on hardware): the PICA buffer takes stage p's output
           for stage p + 2 onwards (sms-3ds: "kept in the buffer (from stage 3)" for stage 1); stage p + 1 still sees the old buffer, but its
           PREVIOUS is exactly stage p's output. */
        if (!(a & 1) && c.bufC == ri) { r.src = c.curStage == c.bufCStage + 1 ? N3DS_TS_PREV : N3DS_TS_BUFFER; r.op = N3DS_TO_C; break; }
        if ((a & 1) && c.bufA == ri) { r.src = c.curStage == c.bufAStage + 1 ? N3DS_TS_PREV : N3DS_TS_BUFFER; r.op = N3DS_TO_A; break; }
        if ((a & 1) ? c.az[ri] : c.cz[ri]) { r.zero = true; break; }   // written by a dropped stage (lenient mode)
        r.src = N3DS_TS_CONST; r.hasK = true; r.k = in.reg[ri]; r.op = (a & 1) ? N3DS_TO_A : N3DS_TO_C; break; }
    case 8: case 9:                                        // TEXC / TEXA
        if (unit >= 0) { r.src = texSrc(unit); r.op = a == 8 ? N3DS_TO_C : N3DS_TO_A; }
        else { r.one = true; if ((in.primaryMap >= 0 || in.nUnits > 0) && s.map < 8) ++c.approx; }   // a texture that has no PICA unit: reads as white
        break;
    case 10: r.src = N3DS_TS_PRIMARY; r.op = N3DS_TO_C; break;
    case 11: r.src = N3DS_TS_PRIMARY; r.op = N3DS_TO_A; break;
    case 12: r.one = true; break;
    case 13: r.src = N3DS_TS_CONST; r.hasK = true; r.k = grey(128); r.op = N3DS_TO_C; break;
    case 14: r.src = N3DS_TS_CONST; r.hasK = true; r.k = kcolorSel(s.ksel, in.kcolor); r.op = N3DS_TO_C; break;
    default: r.zero = true; break;
    }
    return r;
}

Arg alphaArgRaw(int a, Ctx& c)
{
    Arg r; const N3dsTevIn& in = *c.in; const N3dsTevRaw& s = in.st[c.stage];
    const int unit = unitOf(in, s.map);
    r.op = N3DS_TO_A;
    switch (a) {
    case 0: if (c.prevValid) r.src = N3DS_TS_PREV; else { r.src = N3DS_TS_CONST; r.hasK = true; r.k = in.reg[0]; } break;
    case 1: case 2: case 3:
        if (c.bufA == a) { r.src = c.curStage == c.bufAStage + 1 ? N3DS_TS_PREV : N3DS_TS_BUFFER; break; }   // r48 / r49
        if (c.az[a]) { r.zero = true; break; }
        r.src = N3DS_TS_CONST; r.hasK = true; r.k = in.reg[a]; break;
    case 4: if (unit >= 0) r.src = texSrc(unit); else r.one = true; break;
    case 5: r.src = N3DS_TS_PRIMARY; break;
    case 6: r.src = N3DS_TS_CONST; r.hasK = true; r.k = kalphaSel(s.kasel, in.kcolor) << 24; break;
    default: r.zero = true; break;
    }
    return r;
}

Arg colorArg(int a, Ctx& c) { Arg r = colorArgRaw(a, c); normConst(r); return r; }
Arg alphaArg(int a, Ctx& c) { Arg r = alphaArgRaw(a, c); normConst(r); return r; }

// Places an Arg into a combiner slot. Constants share the stage's single constant colour (first one wins).
struct Slots { uint8_t src[3], op[3]; int fn, n; };

void putSlot(Slots& sl, int i, const Arg& a, bool invert, bool isAlpha, Ctx& c, bool& usedConst, uint32_t& constVal)
{
    Arg x = a;
    if (x.zero || x.one) {                                 // a constant 0 or 1 needs the constant colour
        bool v = x.one; if (invert) { v = !v; invert = false; }
        x.src = N3DS_TS_CONST; x.op = isAlpha ? N3DS_TO_A : N3DS_TO_C; x.hasK = true;
        x.k = isAlpha ? (v ? 0xFF000000u : 0x00000000u) : (v ? 0xFFFFFFFFu : 0xFF000000u);   // 1p fix: an alpha 0 was 0xFF000000 = alpha 1
    }
    if (x.src == N3DS_TS_CONST) {
        if (!usedConst) { usedConst = true; constVal = x.k; }
        else if (constVal != x.k) ++c.approx;              // two different constants in one stage: PICA has one
    }
    uint8_t op = x.op;
    if (invert) op = op == N3DS_TO_C ? N3DS_TO_1MC : op == N3DS_TO_A ? N3DS_TO_1MA : op == N3DS_TO_1MC ? N3DS_TO_C : N3DS_TO_A;
    if (isAlpha) op = (op == N3DS_TO_1MC || op == N3DS_TO_1MA) ? N3DS_TO_1MA : N3DS_TO_A;
    sl.src[i] = x.src; sl.op[i] = op;
}

// out = (d + ((1-c)*a + c*b)) for one combiner (colour or alpha). Picks the PICA function that computes it exactly where it can.
void solve(const Arg& A, const Arg& B, const Arg& C, const Arg& D, bool isAlpha, Ctx& c, Slots& sl, bool& usedConst, uint32_t& constVal)
{
    memset(&sl, 0, sizeof sl);
    auto S = [&](int i, const Arg& a, bool inv = false) { putSlot(sl, i, a, inv, isAlpha, c, usedConst, constVal); };
    auto replace = [&](const Arg& a) { sl.fn = N3DS_TFN_REPLACE; S(0, a); };
    Arg zero; zero.zero = true;
    if (D.zero) {
        if (A.zero) {                                      // c * b
            if (B.zero || C.zero) replace(zero);
            else if (C.one) replace(B);
            else if (B.one) replace(C);
            else { sl.fn = N3DS_TFN_MODULATE; S(0, B); S(1, C); }
        } else if (B.zero) {                               // (1-c) * a
            if (C.zero) replace(A);
            else if (C.one) replace(zero);
            else { sl.fn = N3DS_TFN_MODULATE; S(0, A); S(1, C, true); }
        } else {                                           // lerp(a, b, c) = b*c + a*(1-c)
            if (C.zero) replace(A);
            else if (C.one) replace(B);
            else { sl.fn = N3DS_TFN_INTERP; S(0, B); S(1, A); S(2, C); }
        }
    } else {
        if (A.zero) {                                      // d + c*b
            if (B.zero || C.zero) replace(D);
            else if (C.one) { sl.fn = N3DS_TFN_ADD; S(0, B); S(1, D); }
            else if (B.one) { sl.fn = N3DS_TFN_ADD; S(0, C); S(1, D); }
            else { sl.fn = N3DS_TFN_MULADD; S(0, B); S(1, C); S(2, D); }
        } else if (B.zero) {                               // d + (1-c)*a
            if (C.zero) { sl.fn = N3DS_TFN_ADD; S(0, A); S(1, D); }
            else if (C.one) replace(D);
            else { sl.fn = N3DS_TFN_MULADD; S(0, A); S(1, C, true); S(2, D); }
        } else {                                           // full form: PICA needs two stages; keep the lerp, drop d
            ++c.approx;
            if (C.zero) replace(A); else if (C.one) replace(B);
            else { sl.fn = N3DS_TFN_INTERP; S(0, B); S(1, A); S(2, C); }
        }
    }
}

// Value of an argument that is a constant (a TEV register / konst, or literal 0 / 1), as 0x00BBGGRR (colour) or 0..255 (alpha).
bool constValue(const Arg& a, bool isAlpha, uint32_t& v)
{
    if (a.one) { v = isAlpha ? 255u : 0x00FFFFFFu; return true; }
    if (a.zero) { v = 0; return true; }
    if (a.src != N3DS_TS_CONST || !a.hasK) return false;
    if (isAlpha) { v = a.k >> 24; return true; }
    if (a.op == N3DS_TO_A || a.op == N3DS_TO_1MA) { const uint32_t x = a.k >> 24; v = x | x << 8 | x << 16; return true; }
    v = a.k & 0x00FFFFFFu; return true;
}

// lerp(K1, K2, x) with two DIFFERENT constants (d = 0). One PICA stage has a single constant colour, so this is done in two stages:
//   H1: t = x * |K2 - K1|        H2: K1 + t (K2 >= K1 in every channel)  or  K1 - t (K2 <= K1 in every channel).
// Exact (up to 8-bit rounding) for any x, including PREVIOUS (H1 reads it before anything overwrites it). Channels pointing in both
// directions cannot be done this way: returns false and the caller keeps the old one-stage approximation.
struct TwoConst { bool on, sub; uint32_t k1, d; };
TwoConst twoConstLerp(const Arg* a, bool isAlpha)
{
    TwoConst r{false, false, 0, 0};
    const Arg &A = a[0], &B = a[1], &C = a[2], &D = a[3];
    if (!D.zero || C.zero || C.one || C.src == N3DS_TS_CONST) return r;
    uint32_t k1, k2;
    if (!constValue(A, isAlpha, k1) || !constValue(B, isAlpha, k2)) return r;
    if (k1 == k2) return r;                                 // one constant: the normal path is exact
    if (A.zero || B.zero) return r;                         // lerp(0, K, x) = K*x and lerp(K, 0, x) = K*(1-x): one stage, exact
    const int n = isAlpha ? 1 : 3;
    bool up = true, down = true; uint32_t d = 0;
    for (int i = 0; i < n; ++i) {
        const int c1 = (int)((k1 >> (8 * i)) & 255), c2 = (int)((k2 >> (8 * i)) & 255);
        if (c2 < c1) up = false;
        if (c2 > c1) down = false;
    }
    if (!up && !down) return r;
    for (int i = 0; i < n; ++i) {
        const int c1 = (int)((k1 >> (8 * i)) & 255), c2 = (int)((k2 >> (8 * i)) & 255);
        d |= (uint32_t)(up ? c2 - c1 : c1 - c2) << (8 * i);
    }
    r.on = true; r.sub = !up; r.k1 = k1; r.d = d;
    return r;
}

uint8_t scaleOf(int gx, Ctx& c) { if (gx == 1) return 2; if (gx == 2) return 4; if (gx == 3) ++c.approx; return 1; }

}  // namespace

void n3dsTevFallback(bool textured, N3dsTevOut& out)
{
    memset(&out, 0, sizeof out);
    out.n = 1;
    N3dsTevStage& s = out.st[0];
    s.rgbScale = s.aScale = 1; s.color = 0xFFFFFFFFu;
    if (textured) {
        s.rgbFn = s.aFn = N3DS_TFN_MODULATE;
        s.rgbSrc[0] = s.aSrc[0] = N3DS_TS_TEX0; s.rgbSrc[1] = s.aSrc[1] = N3DS_TS_PRIMARY;
    } else {
        s.rgbFn = s.aFn = N3DS_TFN_REPLACE;
        s.rgbSrc[0] = s.aSrc[0] = N3DS_TS_PRIMARY;
    }
    s.rgbOp[0] = s.rgbOp[1] = N3DS_TO_C; s.aOp[0] = s.aOp[1] = N3DS_TO_A; s.rgbOp[2] = N3DS_TO_C; s.aOp[2] = N3DS_TO_A;
}

bool n3dsTevBuild(const N3dsTevIn& in, N3dsTevOut& out)
{
    memset(&out, 0, sizeof out);
    if (in.nStages < 1 || in.nStages > (in.lenient ? 16 : 6)) return false;    // PICA has six combiner stages
    if (!in.lenient)
        for (int i = 0; i < in.nStages; ++i)
            if (in.st[i].cout != 0 || in.st[i].aout != 0) return false;   // results kept in TEV registers for later stages: not expressible
    Ctx c{&in, 0, false, false, 0, 0, 0};
    int outN = 0;                                          // PICA stages emitted so far (a GX stage may need a helper stage before it)
    bool latchC = false, latchA = false; int latchRC = 0, latchRA = 0;
    auto latch = [&](int p) {                              // r48: this GX stage's register output -> combiner buffer after PICA stage p
        if (latchC) { if (p >= 0 && p <= 3) { out.bufRgb |= (uint8_t)(1u << p); c.bufC = latchRC; c.bufCStage = p; } else { ++c.approx; c.cz[latchRC & 3] = true; } }
        if (latchA) { if (p >= 0 && p <= 3) { out.bufA |= (uint8_t)(1u << p); c.bufA = latchRA; c.bufAStage = p; } else { ++c.approx; c.az[latchRA & 3] = true; } }
        latchC = latchA = false;
    };
    for (int i = 0; i < in.nStages; ++i) {
        c.stage = i;
        N3dsTevRaw sFix = in.st[i];
        /* r15 (Pikmin pink, redModel.mod): the Pikmin body is s0 = C0 (the Pikmin colour) x RASC on channel 0 (diffuse light), then s1 = CPREV + RASC on
           channel 1 = GX_COLOR1A1, the SPECULAR channel. The 3DS has a single (headlight) colour channel, so channel 1 came out as the white material colour
           and s1 added white to every pixel: red + white = pink. Channel 1 (and GX_COLOR_ZERO) rasterised colour is read as 0 here (no specular highlight). */
        /* r32 (Wollywog drawn black, log aeb75b34): some materials use channel 1 as a MULTIPLIER (texture x RASC), where reading it as 0 gives black.
           Only the added term (the d input: "previous + specular") is a highlight; in the a / b / c inputs channel 1 is read as ONE (no modulation)
           instead. GX_COLOR_ZERO (6) stays 0 everywhere. Alpha has no ONE argument and keeps the old 0 (the cases seen so far only use colour). */
        if (sFix.chan == 1 || sFix.chan == 3 || sFix.chan == 5 || sFix.chan == 6) {
            for (int k = 0; k < 4; ++k) {
                /* r33: "added" means the stage adds to something (d is not ZERO: previous + channel-1 term, the specular pattern, in ANY input
                   position - C(15,12,10,0) = PREV + RASC). Only a stage that starts from zero and multiplies by channel 1 reads it as ONE. */
                if (sFix.ci[k] == 10 || sFix.ci[k] == 11) sFix.ci[k] = (sFix.ci[3] != 15 || k == 3 || sFix.chan == 6) ? 15 : 12;
                if (sFix.ai[k] == 5) sFix.ai[k] = 7;
            }
        }
        const N3dsTevRaw& s = sFix;
        bool skipC = s.cout != 0, skipA = s.aout != 0;   // result goes to a TEV register (lenient mode only): PICA has none
        /* r48 (Yellow Wollywog body black, log 684fe42e: s1 = PREV + TEX x light -> REG2, s2 = C2 + specular): a stage whose result goes to a
           TEV register is computed normally and its output latched into the PICA combiner buffer; later reads of that register read the buffer.
           Allowed when the stage's output may replace PREVIOUS (no later stage reads the old PREVIOUS before a stage writes it again, and some
           later stage writes it - the final colour), it is one of PICA stages 0..3 (the buffer-update bits), and the buffer is free. */
        auto bufOk = [&](bool colour) -> bool {
            const int R = colour ? s.cout : s.aout;
            if (outN > 3) return false;
            const int held = colour ? c.bufC : c.bufA;
            if (held > 0 && held != R) {                    // still read later? then it must keep the buffer
                for (int j = i + 1; j < in.nStages; ++j) { const N3dsTevRaw& t = in.st[j];
                    for (int k = 0; k < 4; ++k) { if (colour ? (t.ci[k] == 2 + 2 * (held - 1)) : (t.ci[k] == 3 + 2 * (held - 1) || t.ai[k] == held)) return false; } }
            }
            bool rewritten = false;
            for (int j = i + 1; j < in.nStages && !rewritten; ++j) { const N3dsTevRaw& t = in.st[j];
                for (int k = 0; k < 4; ++k) {
                    if (colour && t.ci[k] == 0) return false;                         // reads the old CPREV
                    if (!colour && (t.ci[k] == 1 || t.ai[k] == 0)) return false;      // reads the old APREV
                }
                if ((colour ? t.cout : t.aout) == 0) rewritten = true;
            }
            return rewritten;
        };
        const bool bufCol = skipC && in.lenient && bufOk(true), bufAl = skipA && in.lenient && bufOk(false);
        if (bufCol) { skipC = false; latchC = true; latchRC = s.cout; }
        if (bufAl) { skipA = false; latchA = true; latchRA = s.aout; }
        if (skipC && skipA) {                                   // nothing for PREVIOUS: drop the stage, remember the registers as 0
            ++c.approx; c.cz[s.cout & 3] = true; c.az[s.aout & 3] = true;
            continue;
        }
        Arg ca[4], aa[4];
        c.curStage = outN;
        for (int k = 0; k < 4; ++k) { ca[k] = colorArg(s.ci[k], c); aa[k] = alphaArg(s.ai[k], c); }
        if (skipC || skipA) {                                   // one channel goes to a register: pass the other channel's PREVIOUS through
            ++c.approx;
            Arg zero; zero.zero = true; Arg one; one.one = true;
            if (skipC) { ca[0] = zero; ca[1] = colorArg(0, c); ca[2] = one; ca[3] = zero; c.cz[s.cout & 3] = true; }
            if (skipA) { aa[0] = zero; aa[1] = alphaArg(0, c); aa[2] = one; aa[3] = zero; c.az[s.aout & 3] = true; }
        }
        if (outN >= 6) { out.n = 0; return false; }
        if (s.cop != 0 || s.aop != 0) ++c.approx;              // subtract / compare ops: approximated as plain add
        /* r38: bias (+-0.5) and DIVIDE_2 are done exactly in an extra PICA stage after the GX stage (when one is free): (x + bias) * scale. They were
           dropped before, e.g. the Distant Spring water / gloss pass C(15,9,10,8) ADDHALF x2 = white sheen came out grey, and the Wollywog shell's
           DIVIDE_2 came out twice as bright. */
        const bool fixC = s.cbias == 1 || s.cbias == 2 || s.cscale == 3, fixA = s.abias == 1 || s.abias == 2 || s.ascale == 3;
        const bool canFix = (fixC || fixA) && !((s.cscale == 3 && s.cbias != 0) || (s.ascale == 3 && s.abias != 0));
        if ((fixC && !canFix) || (s.cbias == 3) || (s.abias == 3)) ++c.approx;
        if (fixA && !canFix) ++c.approx;
        // 1p: lerp(K1, K2, x) with two different constants in the colour and/or the alpha combiner -> two exact PICA stages (twoConstLerp).
        // The file-select sparkles are lerp(C0, C2, CPREV) / lerp(A0, A1, APREV): the old one-stage approximation used a single constant,
        // which made the whole quad one flat colour and alpha = the white squares.
        {
            const TwoConst tc = twoConstLerp(ca, false), ta = twoConstLerp(aa, true);
            if ((tc.on || ta.on) && outN + 2 <= 6) {
                Slots rgb, al; bool uc = false, ua = false; uint32_t kv = 0, kav = 0;
                if (tc.on) { memset(&rgb, 0, sizeof rgb); rgb.fn = N3DS_TFN_MODULATE; putSlot(rgb, 0, ca[2], false, false, c, uc, kv); uc = false;
                             rgb.src[1] = N3DS_TS_CONST; rgb.op[1] = N3DS_TO_C; }
                else solve(ca[0], ca[1], ca[2], ca[3], false, c, rgb, uc, kv);
                if (ta.on) { memset(&al, 0, sizeof al); al.fn = N3DS_TFN_MODULATE; putSlot(al, 0, aa[2], false, true, c, ua, kav); ua = false;
                             al.src[1] = N3DS_TS_CONST; al.op[1] = N3DS_TO_A; }
                else solve(aa[0], aa[1], aa[2], aa[3], true, c, al, ua, kav);
                N3dsTevStage& h = out.st[outN++];
                memset(&h, 0, sizeof h);
                for (int k = 0; k < 3; ++k) { h.rgbSrc[k] = rgb.src[k]; h.rgbOp[k] = rgb.op[k]; h.aSrc[k] = al.src[k]; h.aOp[k] = al.op[k]; }
                h.rgbFn = (uint8_t)rgb.fn; h.aFn = (uint8_t)al.fn;
                h.rgbScale = tc.on ? 1 : scaleOf(s.cscale, c); h.aScale = ta.on ? 1 : scaleOf(s.ascale, c);
                const uint32_t hr = tc.on ? tc.d : (uc ? kv & 0x00FFFFFFu : 0x00FFFFFFu);
                const uint32_t ha = ta.on ? ta.d : (ua ? (kav >> 24) & 255u : (uc ? (kv >> 24) & 255u : 255u));
                h.color = hr | (ha << 24);
                N3dsTevStage& o = out.st[outN++];
                memset(&o, 0, sizeof o);
                o.rgbSrc[0] = N3DS_TS_PREV; o.rgbOp[0] = N3DS_TO_C; o.aSrc[0] = N3DS_TS_PREV; o.aOp[0] = N3DS_TO_A;
                if (tc.on) {
                    o.rgbFn = tc.sub ? N3DS_TFN_SUB : N3DS_TFN_ADD;
                    if (tc.sub) { o.rgbSrc[0] = N3DS_TS_CONST; o.rgbSrc[1] = N3DS_TS_PREV; } else o.rgbSrc[1] = N3DS_TS_CONST;
                    o.rgbOp[1] = N3DS_TO_C;
                } else o.rgbFn = N3DS_TFN_REPLACE;
                if (ta.on) {
                    o.aFn = ta.sub ? N3DS_TFN_SUB : N3DS_TFN_ADD;
                    if (ta.sub) { o.aSrc[0] = N3DS_TS_CONST; o.aSrc[1] = N3DS_TS_PREV; } else o.aSrc[1] = N3DS_TS_CONST;
                    o.aOp[1] = N3DS_TO_A;
                } else o.aFn = N3DS_TFN_REPLACE;
                o.rgbScale = tc.on ? scaleOf(s.cscale, c) : 1; o.aScale = ta.on ? scaleOf(s.ascale, c) : 1;
                o.color = (tc.on ? tc.k1 : 0x00FFFFFFu) | ((ta.on ? ta.k1 : 255u) << 24);
                c.prevValid = true;
                latch(outN - 1);
                continue;
            }
        }
        // lerp(K0, K1, t) with two DIFFERENT constant colours (J2D/P2D "black/white" remap: C0 = black, C1 = white, c = texture).
        // A PICA stage has one constant, so spend an extra stage: first REPLACE(K0), then INTERPOLATE(K1, PREVIOUS, t).
        {
            const Arg &A = ca[0], &B = ca[1], &C = ca[2], &D = ca[3];
            if (D.zero && !A.zero && !A.one && !B.zero && !B.one && !C.zero && !C.one &&
                A.src == N3DS_TS_CONST && B.src == N3DS_TS_CONST && A.hasK && B.hasK && A.op == N3DS_TO_C && B.op == N3DS_TO_C &&
                (A.k & 0x00FFFFFFu) != (B.k & 0x00FFFFFFu) && C.src != N3DS_TS_CONST && C.src != N3DS_TS_PREV && outN + 2 <= 6) {
                N3dsTevStage& h = out.st[outN++];
                memset(&h, 0, sizeof h);
                h.rgbFn = N3DS_TFN_REPLACE; h.rgbSrc[0] = N3DS_TS_CONST; h.rgbOp[0] = N3DS_TO_C;
                h.aFn = N3DS_TFN_REPLACE;
                if (outN > 1) { h.aSrc[0] = N3DS_TS_PREV; h.aOp[0] = N3DS_TO_A; } else { h.aSrc[0] = N3DS_TS_CONST; h.aOp[0] = N3DS_TO_A; }
                h.rgbScale = h.aScale = 1;
                h.color = (A.k & 0x00FFFFFFu) | 0xFF000000u;
                ca[0].src = N3DS_TS_PREV; ca[0].op = N3DS_TO_C; ca[0].hasK = false; ca[0].k = 0;   // a is now "the previous stage" (= K0)
            }
        }
        Slots rgb, al; bool uc = false, ua = false; uint32_t kv = 0, kav = 0;
        solve(ca[0], ca[1], ca[2], ca[3], false, c, rgb, uc, kv);
        solve(aa[0], aa[1], aa[2], aa[3], true, c, al, ua, kav);
        if (outN >= 6) { out.n = 0; return false; }
        N3dsTevStage& o = out.st[outN++];
        c.prevValid = true;
        for (int k = 0; k < 3; ++k) { o.rgbSrc[k] = rgb.src[k]; o.rgbOp[k] = rgb.op[k]; o.aSrc[k] = al.src[k]; o.aOp[k] = al.op[k]; }
        o.rgbFn = (uint8_t)rgb.fn; o.aFn = (uint8_t)al.fn;
        const bool doFix = canFix && outN < 6;
        if (canFix && !doFix) ++c.approx;
        o.rgbScale = (doFix && fixC) ? 1 : scaleOf(s.cscale, c); o.aScale = (doFix && fixA) ? 1 : scaleOf(s.ascale, c);
        // constant colour: rgb from the colour combiner's constant, alpha from the alpha combiner's constant
        const uint32_t rgbK = uc ? kv & 0x00FFFFFFu : 0x00FFFFFFu;
        uint32_t aK = ua ? (kav >> 24) & 255u : (uc ? (kv >> 24) & 255u : 255u);
        /* r42: the colour combiner can read the constant's ALPHA (A0 / A1 / A2 / KONST alpha as a colour input). The alpha combiner then must not
           put its own value there. Seen: C(15,3,0,10) = A0 x previous on channel 1, whose alpha input (RASA of channel 1) is read as ZERO -> the
           alpha combiner stored 0 in the constant and A0 x previous came out BLACK (Wollywog / Puffy Blowhog bodies). Colour wins. */
        if (uc) {
            bool rgbReadsKA = false;
            for (int k = 0; k < 3; ++k) if (rgb.src[k] == N3DS_TS_CONST && (rgb.op[k] == N3DS_TO_A || rgb.op[k] == N3DS_TO_1MA)) rgbReadsKA = true;
            if (rgbReadsKA && aK != ((kv >> 24) & 255u)) {
                bool alphaReadsK = false;
                for (int k = 0; k < 3; ++k) if (al.src[k] == N3DS_TS_CONST) alphaReadsK = true;
                if (alphaReadsK) ++c.approx;
                aK = (kv >> 24) & 255u;
            }
        }
        o.color = rgbK | (aK << 24);
        if (doFix) {
            N3dsTevStage& f = out.st[outN++];
            memset(&f, 0, sizeof f);
            f.rgbSrc[0] = N3DS_TS_PREV; f.rgbOp[0] = N3DS_TO_C; f.aSrc[0] = N3DS_TS_PREV; f.aOp[0] = N3DS_TO_A;
            f.rgbSrc[1] = N3DS_TS_CONST; f.rgbOp[1] = N3DS_TO_C; f.aSrc[1] = N3DS_TS_CONST; f.aOp[1] = N3DS_TO_A;
            f.rgbOp[2] = N3DS_TO_C; f.aOp[2] = N3DS_TO_A;
            auto fixFn = [](int bias, int scale) -> uint8_t { return bias == 1 ? N3DS_TFN_ADD : bias == 2 ? N3DS_TFN_SUB : scale == 3 ? N3DS_TFN_MODULATE : N3DS_TFN_REPLACE; };
            f.rgbFn = fixC ? fixFn(s.cbias, s.cscale) : N3DS_TFN_REPLACE; f.aFn = fixA ? fixFn(s.abias, s.ascale) : N3DS_TFN_REPLACE;
            f.rgbScale = (fixC && s.cscale != 3) ? scaleOf(s.cscale, c) : 1; f.aScale = (fixA && s.ascale != 3) ? scaleOf(s.ascale, c) : 1;
            f.color = 0x80808080u;                         // 0.5 in every channel: + 0.5, - 0.5 or x 0.5
        }
        latch(outN - 1);
    }
    out.n = outN;
    out.approximations = c.approx;
    return true;
}
