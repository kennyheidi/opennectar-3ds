#include "n3ds_texconv.h"
#include "ref_texconv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <random>
#include <chrono>
#include <vector>
int main()
{
    std::mt19937 rng(12345);
    int fails = 0, checks = 0;
    struct Sz { int w, h; } sizes[] = {{8,8},{16,16},{24,40},{64,64},{100,60},{1,1},{7,13},{256,256},{512,128},{200,200},{1024,64}};
    for (auto sz : sizes) {
        const size_t bytes = n3dsconv::gcDataSize(n3dsconv::CMPR, sz.w, sz.h);
        for (int variant = 0; variant < 3; ++variant) {
            std::vector<uint8_t> data(bytes);
            for (auto& b : data) b = (uint8_t)rng();
            if (variant == 1) for (size_t o = 0; o + 8 <= bytes; o += 8) { if (data[o] < data[o + 2]) std::swap(data[o], data[o + 2]); }           // mostly 4-colour (opaque) blocks
            if (variant == 2) for (size_t o = 0; o + 8 <= bytes; o += 8) { if (data[o] > data[o + 2]) std::swap(data[o], data[o + 2]); }           // mostly 3-colour + transparent
            for (int shift = 0; shift <= 3; ++shift) for (int box = 0; box < 2; ++box) for (int flip = 0; flip < 2; ++flip) for (int pica = 0; pica <= 5; ++pica) {
                const int sw = (sz.w + (1 << shift) - 1) >> shift, sh = (sz.h + (1 << shift) - 1) >> shift;
                const int potW = n3dsconv::potSize(sw), potH = n3dsconv::potSize(sh);
                const size_t nb = n3dsconv::picaBytes(pica, potW, potH);
                std::vector<uint8_t> a(nb, 0xCD), b(nb, 0xCD);
                n3dsconv::convertScaled(n3dsconv::CMPR, data.data(), sz.w, sz.h, nullptr, pica, potW, potH, flip != 0, a.data(), shift, box != 0);
                ref_n3dsconv::convertScaled(ref_n3dsconv::CMPR, data.data(), sz.w, sz.h, nullptr, pica, potW, potH, flip != 0, b.data(), shift, box != 0);
                ++checks;
                if (a != b) { if (++fails < 10) printf("MISMATCH %dx%d variant %d shift %d box %d flip %d pica %d\n", sz.w, sz.h, variant, shift, box, flip, pica); }
            }
        }
    }
    // timing: 1024x1024 CMPR -> 256x256 (shift 2) RGB565
    {
        const int W = 1024; std::vector<uint8_t> data(n3dsconv::gcDataSize(n3dsconv::CMPR, W, W)); for (auto& b : data) b = (uint8_t)rng();
        for (size_t o = 0; o + 8 <= data.size(); o += 8) if (data[o] < data[o + 2]) std::swap(data[o], data[o + 2]);
        std::vector<uint8_t> out(256 * 256 * 2);
        auto t0 = std::chrono::steady_clock::now();
        n3dsconv::convertScaled(n3dsconv::CMPR, data.data(), W, W, nullptr, n3dsconv::P_RGB565, 256, 256, false, out.data(), 2, true);
        auto t1 = std::chrono::steady_clock::now();
        ref_n3dsconv::convertScaled(ref_n3dsconv::CMPR, data.data(), W, W, nullptr, ref_n3dsconv::P_RGB565, 256, 256, false, out.data(), 2, true);
        auto t2 = std::chrono::steady_clock::now();
        printf("1024x1024 CMPR -> 256x256: new %.2f ms, old %.2f ms (host)\n", std::chrono::duration<double, std::milli>(t1 - t0).count(), std::chrono::duration<double, std::milli>(t2 - t1).count());
    }
    printf("%d checks, %d mismatches\n", checks, fails);
    return fails != 0;
}
