// HOST TEST ONLY: a small PICA200 vertex-shader interpreter for the instructions picasso emits for passthrough.v.pica
// (add dp3 dp4 mul sge slt max min rcp rsq mov mova nop end ifu jmpu call, sgei/slti; format-1 operands with swizzles, negation and a0/aL indexing).
// Semantics follow 3dbrew "Shader Instruction Set" and Citra's interpreter. Optional float24 rounding (16-bit mantissa).
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <map>

struct PicaShader {
    std::vector<uint32_t> code, opdesc;
    uint32_t mainOff = 0;
    std::map<std::string, int> unif;         // name -> location as shaderInstanceGetUniformLocation returns it (startReg - 0x10)
    struct Const { int type, reg; float v[4]; };
    std::vector<Const> consts;
    int outType[16];                         // output register -> semantic (0 pos, 2 color, 3 tc0, 5 tc1, 6 tc2), -1 none
    float c[96][4]; bool b[16]; float a0[2];
    bool f24 = true;

    static float f24dec(uint32_t w) {        // 1.7.16 float24 as stored in DVLE constants
        w &= 0xFFFFFF; if ((w & 0x7FFFFF) == 0) return (w >> 23) ? -0.0f : 0.0f;
        const uint32_t sign = w >> 23, exp = (w >> 16) & 0x7F, man = w & 0xFFFF;
        uint32_t f = sign << 31 | (exp - 63 + 127) << 23 | man << 7; float r; memcpy(&r, &f, 4); return r;
    }
    float R(float x) const {                 // round to float24 precision
        if (!f24 || x == 0.0f || !isfinite(x)) return x;
        uint32_t u; memcpy(&u, &x, 4); u = (u + 0x40) & ~0x7Fu; float r; memcpy(&r, &u, 4); return r;
    }
    bool load(const std::vector<uint8_t>& f) {
        auto u32 = [&](size_t o) { uint32_t v; memcpy(&v, &f[o], 4); return v; };
        auto u16 = [&](size_t o) { uint16_t v; memcpy(&v, &f[o], 2); return (uint32_t)v; };
        if (f.size() < 16 || memcmp(&f[0], "DVLB", 4)) return false;
        const uint32_t nDvle = u32(4); const size_t dvlp = 8 + 4 * nDvle;
        if (memcmp(&f[dvlp], "DVLP", 4)) return false;
        const uint32_t binOff = u32(dvlp + 8), binWords = u32(dvlp + 12), opOff = u32(dvlp + 16), opN = u32(dvlp + 20);
        for (uint32_t i = 0; i < binWords; ++i) code.push_back(u32(dvlp + binOff + 4 * i));
        for (uint32_t i = 0; i < opN; ++i) opdesc.push_back(u32(dvlp + opOff + 8 * i));
        const size_t dvle = u32(8);
        if (memcmp(&f[dvle], "DVLE", 4)) return false;
        mainOff = u32(dvle + 8);
        const uint32_t cOff = u32(dvle + 0x18), cN = u32(dvle + 0x1C), oOff = u32(dvle + 0x28), oN = u32(dvle + 0x2C);
        const uint32_t uOff = u32(dvle + 0x30), uN = u32(dvle + 0x34), sOff = u32(dvle + 0x38);
        for (int i = 0; i < 16; ++i) outType[i] = -1;
        for (uint32_t i = 0; i < cN; ++i) {
            const size_t e = dvle + cOff + 20 * i; Const k; k.type = (int)u16(e); k.reg = (int)u16(e + 2);
            for (int j = 0; j < 4; ++j) k.v[j] = k.type == 2 ? f24dec(u32(e + 4 + 4 * j)) : (float)u32(e + 4 + 4 * j);
            consts.push_back(k);
        }
        for (uint32_t i = 0; i < oN; ++i) { const size_t e = dvle + oOff + 8 * i; outType[u16(e + 2) & 15] = (int)u16(e); }
        for (uint32_t i = 0; i < uN; ++i) {
            const size_t e = dvle + uOff + 8 * i; const char* nm = (const char*)&f[dvle + sOff + u32(e)];
            unif[nm] = (int)u16(e + 4) - 0x10;
        }
        memset(c, 0, sizeof c); memset(b, 0, sizeof b);
        for (auto& k : consts) {
            if (k.type == 2 && k.reg < 96) for (int j = 0; j < 4; ++j) c[k.reg][j] = k.v[j];
            if (k.type == 0 && k.reg < 16) b[k.reg] = k.v[0] != 0;
        }
        return true;
    }
    int loc(const char* n) const { auto it = unif.find(n); return it == unif.end() ? -1 : it->second; }
    void setF(int reg, const float* v) { for (int j = 0; j < 4; ++j) c[reg][j] = R(v[j]); }
    void setB(int locv, bool v) { b[locv - 0x68] = v; }

    // run one vertex: in[16][4] -> out[16][4]
    void run(const float in[16][4], float out[16][4]) {
        float r[16][4]; memset(r, 0, sizeof r); memset(out, 0, sizeof(float) * 64); a0[0] = a0[1] = 0;
        struct Frame { uint32_t end, ret; };
        std::vector<Frame> st;
        uint32_t pc = mainOff; int guard = 0;
        auto srcv = [&](int reg, int idx, uint32_t sel, bool neg, float o[4], bool wide) {
            const float* p;
            if (reg < 0x10) p = in[reg];
            else if (reg < 0x20) p = r[reg - 0x10];
            else { int cr = reg - 0x20; if (wide && idx == 1) cr += (int)a0[0]; else if (wide && idx == 2) cr += (int)a0[1]; cr &= 127; p = cr < 96 ? c[cr] : nullptr; }
            for (int i = 0; i < 4; ++i) { const int s = (sel >> (2 * (3 - i))) & 3; float v = p ? p[s] : 1.0f; o[i] = neg ? -v : v; }
        };
        while (guard++ < 100000) {
            // flow-control returns
            while (!st.empty() && pc == st.back().end) { pc = st.back().ret; st.pop_back(); }
            const uint32_t ins = code[pc]; const uint32_t op = ins >> 26;
            if (op == 0x22) return;                                   // END
            if (op == 0x21) { ++pc; continue; }                       // NOP
            if (op == 0x27 || op == 0x26 || op == 0x2D) {             // IFU / CALLU / JMPU (format 3)
                const uint32_t num = ins & 0xFF, dst = (ins >> 10) & 0xFFF, bid = (ins >> 22) & 15;
                if (op == 0x27) {
                    if (b[bid]) st.push_back({dst, dst + num}), ++pc;
                    else { st.push_back({dst + num, dst + num}); pc = dst; }
                } else if (op == 0x26) { if (b[bid]) { st.push_back({dst + num, pc + 1}); pc = dst; } else ++pc; }
                else { const bool inv = (num & 1) != 0; if (b[bid] != inv) pc = dst; else ++pc; }
                continue;
            }
            if (op == 0x24) { const uint32_t num = ins & 0xFF, dst = (ins >> 10) & 0xFFF; st.push_back({dst + num, pc + 1}); pc = dst; continue; }
            if (op >= 0x20) { printf("pica_interp: unsupported opcode %02x at %u\n", op, pc); return; }
            // format 1 (format 1i for the inverted forms 0x18-0x1B: src1 is the 5-bit operand, src2 the 7-bit one that can be indexed)
            const uint32_t d = opdesc[ins & 0x7F];
            const bool inv = op >= 0x18 && op <= 0x1B;
            const int src2 = inv ? (int)((ins >> 7) & 0x7F) : (int)((ins >> 7) & 0x1F), src1 = inv ? (int)((ins >> 14) & 0x1F) : (int)((ins >> 12) & 0x7F);
            const int idx = (int)((ins >> 19) & 3), dst = (int)((ins >> 21) & 0x1F);
            float s1[4], s2[4], res[4];
            srcv(src1, inv ? 0 : idx, (d >> 5) & 0xFF, (d >> 4) & 1, s1, !inv);
            srcv(src2, inv ? idx : 0, (d >> 14) & 0xFF, (d >> 13) & 1, s2, inv);
            auto mulf = [&](float x, float y) { return (x == 0.0f || y == 0.0f) ? 0.0f : R(x * y); };   // PICA: 0 * inf = 0
            switch (op) {
            case 0x00: for (int i = 0; i < 4; ++i) res[i] = R(s1[i] + s2[i]); break;
            case 0x01: { float t = R(R(mulf(s1[0], s2[0]) + mulf(s1[1], s2[1])) + mulf(s1[2], s2[2])); for (int i = 0; i < 4; ++i) res[i] = t; break; }
            case 0x02: { float t = R(R(R(mulf(s1[0], s2[0]) + mulf(s1[1], s2[1])) + mulf(s1[2], s2[2])) + mulf(s1[3], s2[3])); for (int i = 0; i < 4; ++i) res[i] = t; break; }
            case 0x08: for (int i = 0; i < 4; ++i) res[i] = mulf(s1[i], s2[i]); break;
            case 0x09: case 0x1A: for (int i = 0; i < 4; ++i) res[i] = s1[i] >= s2[i] ? 1.0f : 0.0f; break;   // SGE / SGEI (r53)
            case 0x0A: case 0x1B: for (int i = 0; i < 4; ++i) res[i] = s1[i] < s2[i] ? 1.0f : 0.0f; break;    // SLT / SLTI (r53)
            case 0x0C: for (int i = 0; i < 4; ++i) res[i] = s1[i] > s2[i] ? s1[i] : s2[i]; break;
            case 0x0D: for (int i = 0; i < 4; ++i) res[i] = s1[i] < s2[i] ? s1[i] : s2[i]; break;
            case 0x0E: { float t = R(1.0f / s1[0]); for (int i = 0; i < 4; ++i) res[i] = t; break; }
            case 0x0F: { float t = R(1.0f / sqrtf(s1[0])); for (int i = 0; i < 4; ++i) res[i] = t; break; }
            case 0x12: { if (d & 8) a0[0] = (float)(int)s1[0]; if (d & 4) a0[1] = (float)(int)s1[1]; ++pc; continue; }
            case 0x13: for (int i = 0; i < 4; ++i) res[i] = s1[i]; break;
            default: printf("pica_interp: unsupported opcode %02x at %u\n", op, pc); return;
            }
            float* o = dst < 0x10 ? out[dst] : r[dst - 0x10];
            for (int i = 0; i < 4; ++i) if (d & (8 >> i)) o[i] = res[i];
            ++pc;
        }
        printf("pica_interp: runaway\n");
    }
};
