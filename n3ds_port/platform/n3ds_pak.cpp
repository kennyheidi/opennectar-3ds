// nectar.pak: the game's small asset files packed into ONE file on the SD card (built on the PC by tools/make_pak.py).
//
// Why: opening a file on the 3DS SD card costs ~35 ms (log 9e6e4c8d, [dvdprof]: "50 opens = 1783 ms"), and a level load opens hundreds of them.
// With the pak the card sees one fopen at boot; every later DVDOpen of a packed file is an index lookup and the reads are plain seek+read
// inside the already-open pak.
//
// How: the DVD layer (build-time patch of dvd_stubs.cpp, see CMakeLists.txt) calls n3ds_dvd_fopen() instead of fopen(). For a path that is
// in the pak it returns a handle into a small pool of "views" (a window of the pak); the patched DVD layer calls n3ds_fseek/ftell/fread/fclose,
// which serve views here and pass real FILE*s straight to stdio. (No fopencookie/funopen: devkitARM's newlib does not expose them.) A path that is not in the pak (or no pak / nectar.ini pak = 0) is opened from the loose files exactly as before.
//
// File format (little endian), version 1:
//   "NPAK" | u32 version=1 | u32 count | u32 reserved
//   count x { u64 fnv1a64(normalized path) | u32 offset | u32 size }   sorted by hash
//   file data
// normalized path = the path as fopen() would get it ("assets/dataDir/..."), lower case, '\' -> '/', leading "./" and '/' removed.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>

extern "C" void n3ds_log(const char* fmt, ...);
extern "C" int n3ds_pak_enabled(void);   // n3ds_config.cpp: nectar.ini pak (default 1)
extern "C" int n3ds_pak_verify(void);    // n3ds_config.cpp: nectar.ini pak_verify (default 0)

namespace {

struct Entry { uint64_t hash; uint32_t off, size; };

FILE* sPak = nullptr;
Entry* sEntries = nullptr;
uint32_t sCount = 0;
bool sTried = false;
std::mutex sInitMtx, sReadMtx;
unsigned sHits = 0, sMisses = 0;

uint64_t pathHash(const char* p)
{
    while (p[0] == '.' && (p[1] == '/' || p[1] == '\\')) p += 2;
    while (*p == '/' || *p == '\\') ++p;
    uint64_t h = 1469598103934665603ULL;
    for (; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        h ^= c; h *= 1099511628211ULL;
    }
    return h;
}

uint32_t rd32(const unsigned char* b) { return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }

void openPak()
{
    std::lock_guard<std::mutex> g(sInitMtx);
    if (sTried) return;
    sTried = true;
    if (!n3ds_pak_enabled()) { n3ds_log("[pak] disabled in nectar.ini (pak = 0): loose files only"); return; }
    FILE* f = fopen("nectar.pak", "rb");
    if (!f) { n3ds_log("[pak] no nectar.pak next to nectar.ini: loose files only (build one with tools/make_pak.py to cut level load times)"); return; }
    unsigned char hd[16];
    if (fread(hd, 1, 16, f) != 16 || memcmp(hd, "NPAK", 4) != 0 || rd32(hd + 4) != 1) {
        n3ds_log("[pak] nectar.pak has a bad header or version: ignored"); fclose(f); return;
    }
    const uint32_t n = rd32(hd + 8);
    if (n == 0 || n > 200000) { n3ds_log("[pak] nectar.pak: implausible entry count %u: ignored", (unsigned)n); fclose(f); return; }
    Entry* e = (Entry*)malloc((size_t)n * sizeof(Entry));
    unsigned char* raw = (unsigned char*)malloc((size_t)n * 16);
    if (!e || !raw || fread(raw, 16, n, f) != n) {
        n3ds_log("[pak] nectar.pak: cannot load the index (%u entries): ignored", (unsigned)n);
        free(e); free(raw); fclose(f); return;
    }
    for (uint32_t i = 0; i < n; ++i) {
        const unsigned char* r = raw + (size_t)i * 16;
        e[i].hash = (uint64_t)rd32(r) | ((uint64_t)rd32(r + 4) << 32);
        e[i].off = rd32(r + 8); e[i].size = rd32(r + 12);
        if (i && e[i].hash < e[i - 1].hash) { n3ds_log("[pak] nectar.pak index is not sorted: ignored"); free(e); free(raw); fclose(f); return; }
    }
    free(raw);
    setvbuf(f, nullptr, _IONBF, 0);          // every read is a positioned read into the caller's own buffer stack; no double buffering
    sPak = f; sEntries = e; sCount = n;
    n3ds_log("[pak] nectar.pak: %u files indexed (%u KB of index)", (unsigned)n, (unsigned)((size_t)n * sizeof(Entry) / 1024));
}

const Entry* find(uint64_t h)
{
    uint32_t lo = 0, hi = sCount;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (sEntries[mid].hash < h) lo = mid + 1; else hi = mid;
    }
    return (lo < sCount && sEntries[lo].hash == h) ? &sEntries[lo] : nullptr;
}

// ---- views: windows onto the pak, handed out as fake FILE* (pointers into this pool, so they can never be confused with a real FILE) ----
struct View { uint32_t base, size; int64_t pos; bool used; FILE* loose; char path[192]; };   // loose/path: pak_verify only
const int kViews = 256;                       // DVD files open at the same time (the game keeps a handful)
View sViews[kViews];
std::mutex sViewMtx;

View* asView(FILE* f)
{
    View* v = reinterpret_cast<View*>(f);
    return (v >= sViews && v < sViews + kViews) ? v : nullptr;
}

// ---- pak_verify = 1: every byte served from the pak is compared with the loose file; on a difference the LOOSE data is used and logged ----
std::mutex sVerifyMtx;
unsigned sVerReads = 0, sVerBad = 0, sVerLogged = 0;
unsigned long long sVerBytes = 0;
unsigned char sVerBuf[32 * 1024];

void verifyLog(const char* fmt, const char* path, unsigned a, unsigned b, unsigned c, unsigned d)
{
    if (sVerLogged >= 40) return;
    ++sVerLogged;
    n3ds_log(fmt, path, a, b, c, d);
}

void verifyOpen(View& v)
{
    v.loose = fopen(v.path, "rb");
    std::lock_guard<std::mutex> g(sVerifyMtx);
    if (!v.loose) { verifyLog("[pak] VERIFY: %s is in the pak (%u bytes) but the loose file cannot be opened%s%u%u%u", v.path, v.size, 0, 0, 0); return; }
    fseek(v.loose, 0, SEEK_END);
    const long ls = ftell(v.loose);
    if (ls != (long)v.size) { ++sVerBad; verifyLog("[pak] VERIFY SIZE MISMATCH %s: pak %u bytes, loose %u bytes (offset in pak %u)%u", v.path, v.size, (unsigned)ls, v.base, 0); }
}

// dst[0..got) came from the pak at file offset pos; compare with the loose file and replace on a difference
void verifyRead(View& v, unsigned char* dst, size_t got, int64_t pos)
{
    if (!v.loose || !got) return;
    std::lock_guard<std::mutex> g(sVerifyMtx);
    ++sVerReads; sVerBytes += got;
    for (size_t done = 0; done < got; ) {
        const size_t k = got - done > sizeof sVerBuf ? sizeof sVerBuf : got - done;
        size_t lg = 0;
        if (fseek(v.loose, (long)(pos + (int64_t)done), SEEK_SET) == 0) lg = fread(sVerBuf, 1, k, v.loose);
        if (lg != k || memcmp(sVerBuf, dst + done, k) != 0) {
            size_t at = 0; while (at < lg && sVerBuf[at] == dst[done + at]) ++at;
            ++sVerBad;
            verifyLog("[pak] VERIFY MISMATCH %s: read at %u (+%u of %u bytes) differs at byte %u -> using the loose data", v.path,
                      (unsigned)pos, (unsigned)done, (unsigned)got, (unsigned)(pos + (int64_t)done + (int64_t)at));
            if (lg) memcpy(dst + done, sVerBuf, lg);
        }
        done += k;
    }
}

} // namespace

extern "C" FILE* n3ds_dvd_fopen(const char* path)
{
    if (!sTried) openPak();
    if (sPak && path) {
        const Entry* e = find(pathHash(path));
        if (e) {
            std::lock_guard<std::mutex> g(sViewMtx);
            for (int i = 0; i < kViews; ++i) {
                if (!sViews[i].used) {
                    sViews[i] = View{ e->off, e->size, 0, true, nullptr, {0} };
                    snprintf(sViews[i].path, sizeof sViews[i].path, "%s", path);
                    if (n3ds_pak_verify()) verifyOpen(sViews[i]);
                    ++sHits;
                    return reinterpret_cast<FILE*>(&sViews[i]);
                }
            }
            // pool exhausted: fall through to the loose file
        }
        ++sMisses;
    }
    return fopen(path, "rb");
}

extern "C" int n3ds_fseek(FILE* f, long off, int whence)
{
    View* v = asView(f);
    if (!v) return fseek(f, off, whence);
    int64_t np = off;
    if (whence == SEEK_CUR) np += v->pos; else if (whence == SEEK_END) np += (int64_t)v->size;
    if (np < 0) return -1;
    v->pos = np;
    return 0;
}

extern "C" long n3ds_ftell(FILE* f)
{
    View* v = asView(f);
    return v ? (long)v->pos : ftell(f);
}

extern "C" size_t n3ds_fread(void* dst, size_t sz, size_t n, FILE* f)
{
    View* v = asView(f);
    if (!v) return fread(dst, sz, n, f);
    if (sz == 0 || n == 0 || v->pos >= (int64_t)v->size) return 0;
    size_t bytes = sz * n;
    if ((int64_t)bytes > (int64_t)v->size - v->pos) bytes = (size_t)((int64_t)v->size - v->pos);
    size_t got = 0;
    {
        // NEVER let the SD card read land directly in the caller's buffer. The pak FILE is unbuffered, so stdio hands the read straight to
        // FSFILE_Read, and the data arrives by DMA into physical memory behind the ARM11 caches. Game buffers are not cache-line aligned:
        // the first/last 32-byte line also holds live neighbouring data, and when the CPU later writes back its stale copy of that line it
        // overwrites freshly read bytes -> corrupted models (garbage triangles, NaN matrices) and heap neighbours (pak = 1 graphics bugs,
        // while nectar_pakbuild's verify said PAK OK). Loose files never had this: buffered stdio always reads into the FILE's own buffer and
        // memcpy's. So: read into a private, cache-line aligned bounce buffer, then copy.
        static unsigned char sBounce[64 * 1024] __attribute__((aligned(64)));
        std::lock_guard<std::mutex> g(sReadMtx);
        unsigned char* out = (unsigned char*)dst;
        while (got < bytes) {
            const size_t k = bytes - got > sizeof sBounce ? sizeof sBounce : bytes - got;
            if (fseek(sPak, (long)(v->base + (uint32_t)(v->pos + (int64_t)got)), SEEK_SET) != 0) break;
            const size_t r = fread(sBounce, 1, k, sPak);
            memcpy(out + got, sBounce, r);
            got += r;
            if (r != k) break;
        }
    }
    if (v->loose) verifyRead(*v, (unsigned char*)dst, got, v->pos);
    v->pos += (int64_t)got;
    return got / sz;
}

extern "C" int n3ds_fclose(FILE* f)
{
    View* v = asView(f);
    if (!v) return fclose(f);
    if (v->loose) { fclose(v->loose); v->loose = nullptr; }
    std::lock_guard<std::mutex> g(sViewMtx);
    v->used = false;
    return 0;
}

// Counters for the log: files served from the pak / opened loose since boot.
extern "C" void n3ds_pak_stats(unsigned* hits, unsigned* misses) { *hits = sHits; *misses = sMisses; }
// pak_verify summary: reads compared, KB compared, mismatches (size or content)
extern "C" void n3ds_pak_verify_stats(unsigned* reads, unsigned* kb, unsigned* bad) { *reads = sVerReads; *kb = (unsigned)(sVerBytes / 1024); *bad = sVerBad; }
