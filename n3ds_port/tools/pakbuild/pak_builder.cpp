// See pak_builder.h. Output is format-compatible with tools/make_pak.py (same keys, hashes, filters, alignment); the byte layout can differ
// slightly (this version reserves index slots for files it later skips as too big), which the reader does not care about.
#include "pak_builder.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <algorithm>
#include <string>
#include <vector>

namespace {

struct Item { std::string key, full; };
struct Entry { uint64_t hash; uint32_t off, size; };

const uint64_t kMax4G = 0xFFFFFFFFull;

bool skipName(const char* n)
{
    if (n[0] == '.' && n[1] == '_') return true;
    return strcmp(n, ".DS_Store") == 0 || strcmp(n, "Thumbs.db") == 0 || strcmp(n, "desktop.ini") == 0;
}
bool skipExt(const char* n)
{
    const char* dot = strrchr(n, '.');
    if (!dot) return false;
    std::string e(dot);
    for (char& c : e) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return e == ".stx" || e == ".thp" || e == ".pak";
}
std::string lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); return s; }

// Directory walk in the same order as make_pak.py's os.walk: this folder's files (sorted by name), then each sub-folder (sorted) recursively.
bool walk(const std::string& dir, const std::string& rel, std::vector<Item>& out, PakResult& r, PakProgressFn fn, void* user)
{
    DIR* d = opendir(dir.c_str());
    if (!d) { snprintf(r.error, sizeof r.error, "cannot open folder %s", dir.c_str()); return false; }
    std::vector<std::string> files, dirs;
    while (struct dirent* e = readdir(d)) {
        const char* n = e->d_name;
        if (strcmp(n, ".") == 0 || strcmp(n, "..") == 0) continue;
        bool isDir = false, known = false;
#ifdef DT_DIR
        if (e->d_type == DT_DIR) { isDir = true; known = true; }
        else if (e->d_type == DT_REG) { known = true; }
#endif
        if (!known) {
            struct stat st;
            if (stat((dir + "/" + n).c_str(), &st) != 0) continue;
            isDir = S_ISDIR(st.st_mode);
        }
        (isDir ? dirs : files).push_back(n);
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    std::sort(dirs.begin(), dirs.end());
    for (const std::string& n : files) {
        if (skipName(n.c_str()) || skipExt(n.c_str())) { ++r.stats.skippedExt; continue; }
        out.push_back(Item{ lower("assets/" + rel + n), dir + "/" + n });
        if (fn && (out.size() & 63) == 0) { r.stats.listed = (unsigned)out.size(); if (!fn(r.stats, user)) { snprintf(r.error, sizeof r.error, "cancelled"); return false; } }
    }
    for (const std::string& n : dirs) {
        if (rel == "dataDir/" && lower(n) == "snddata") continue;     // sound banks/streams: read by the audio code directly, never via DVDOpen
        if (!walk(dir + "/" + n, rel + n + "/", out, r, fn, user)) return false;
    }
    return true;
}

void put32(unsigned char* b, uint32_t v) { b[0] = (unsigned char)v; b[1] = (unsigned char)(v >> 8); b[2] = (unsigned char)(v >> 16); b[3] = (unsigned char)(v >> 24); }

bool writeZeros(FILE* f, uint64_t n)
{
    static const unsigned char z[256] = {0};
    while (n) { const size_t k = n > sizeof z ? sizeof z : (size_t)n; if (fwrite(z, 1, k, f) != k) return false; n -= k; }
    return true;
}

} // namespace

uint64_t pak_key_hash(const char* p)
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

PakResult pak_build(const char* base, uint64_t maxFileBytes, PakProgressFn fn, void* user)
{
    PakResult r;
    memset(&r, 0, sizeof r);
    const std::string b(base);
    const std::string outPath = b + "/nectar.pak", tmpPath = b + "/nectar.pak.tmp";

    std::vector<Item> items;
    if (!walk(b + "/assets/dataDir", "dataDir/", items, r, fn, user)) return r;
    r.stats.listed = (unsigned)items.size();
    if (items.empty()) { snprintf(r.error, sizeof r.error, "no files under %s/assets/dataDir", base); return r; }

    const uint64_t dataStart = (16 + 16ull * items.size() + 31) & ~31ull;
    FILE* out = fopen(tmpPath.c_str(), "wb");
    if (!out) { snprintf(r.error, sizeof r.error, "cannot create %s", tmpPath.c_str()); return r; }
    std::vector<char> obuf(256 * 1024);
    setvbuf(out, obuf.data(), _IOFBF, obuf.size());
    std::vector<unsigned char> buf(256 * 1024);
    std::vector<Entry> entries;
    entries.reserve(items.size());
    uint64_t pos = dataStart;
    bool ok = writeZeros(out, dataStart);
    auto fail = [&](const char* what, const std::string& name) {
        snprintf(r.error, sizeof r.error, "%s: %s", what, name.c_str());
        r.stats.current = nullptr;               // points into `items`, which dies with this function
        fclose(out); remove(tmpPath.c_str());
        return r;
    };
    if (!ok) return fail("write failed (SD card full?)", tmpPath);

    for (const Item& it : items) {
        r.stats.current = it.key.c_str();
        FILE* in = fopen(it.full.c_str(), "rb");
        long sz = -1;
        if (in && fseek(in, 0, SEEK_END) == 0) { sz = ftell(in); fseek(in, 0, SEEK_SET); }
        if (!in || sz < 0) {                     // unreadable (odd name, locked, damaged): leave it loose, keep going
            if (in) fclose(in);
            if (!r.firstSkipped[0]) snprintf(r.firstSkipped, sizeof r.firstSkipped, "%s", it.full.c_str());
            ++r.stats.skippedErr; ++r.stats.done;
            continue;
        }
        if ((uint64_t)sz > maxFileBytes) { fclose(in); ++r.stats.skippedBig; ++r.stats.done; continue; }
        const uint64_t padded = ((uint64_t)sz + 31) & ~31ull;
        if (pos + padded > kMax4G) { fclose(in); return fail("the pak would pass 4 GB (FAT32 limit); lower the size limit", it.full); }
        uint64_t left = (uint64_t)sz;
        while (left) {
            const size_t k = left > buf.size() ? buf.size() : (size_t)left;
            if (fread(buf.data(), 1, k, in) != k) { fclose(in); return fail("read error in the middle of", it.full); }
            if (fwrite(buf.data(), 1, k, out) != k) { fclose(in); return fail("write failed (SD card full?)", tmpPath); }
            left -= k;
        }
        fclose(in);
        if (!writeZeros(out, padded - (uint64_t)sz)) return fail("write failed (SD card full?)", tmpPath);
        entries.push_back(Entry{ pak_key_hash(it.key.c_str()), (uint32_t)pos, (uint32_t)sz });
        pos += padded;
        ++r.stats.packed; ++r.stats.done; r.stats.bytes = pos;
        if (fn && !fn(r.stats, user)) return fail("cancelled", "nothing changed");
    }

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& c) { return a.hash < c.hash; });
    for (size_t i = 1; i < entries.size(); ++i)
        if (entries[i].hash == entries[i - 1].hash) return fail("hash collision between two file names (rename one)", "see make_pak.py");

    // header + index at the front (the reserved area is sized for every listed file; slots of skipped files stay zero, after the index)
    unsigned char hd[16];
    memcpy(hd, "NPAK", 4); put32(hd + 4, 1); put32(hd + 8, (uint32_t)entries.size()); put32(hd + 12, 0);
    if (fflush(out) != 0 || fseek(out, 0, SEEK_SET) != 0 || fwrite(hd, 1, 16, out) != 16) return fail("write failed", tmpPath);
    for (const Entry& e : entries) {
        unsigned char rec[16];
        put32(rec, (uint32_t)e.hash); put32(rec + 4, (uint32_t)(e.hash >> 32)); put32(rec + 8, e.off); put32(rec + 12, e.size);
        if (fwrite(rec, 1, 16, out) != 16) return fail("write failed", tmpPath);
    }
    if (fclose(out) != 0) { remove(tmpPath.c_str()); snprintf(r.error, sizeof r.error, "closing %s failed (SD card full?)", tmpPath.c_str()); return r; }
    remove(outPath.c_str());
    if (rename(tmpPath.c_str(), outPath.c_str()) != 0) { snprintf(r.error, sizeof r.error, "cannot rename %s to nectar.pak", tmpPath.c_str()); return r; }
    r.stats.bytes = pos;
    r.stats.current = nullptr;
    r.ok = true;
    return r;
}

PakResult pak_verify(const char* base, PakProgressFn fn, void* user)
{
    PakResult r;
    memset(&r, 0, sizeof r);
    const std::string b(base);
    FILE* pak = fopen((b + "/nectar.pak").c_str(), "rb");
    if (!pak) { snprintf(r.error, sizeof r.error, "no %s/nectar.pak", base); return r; }
    unsigned char hd[16];
    if (fread(hd, 1, 16, pak) != 16 || memcmp(hd, "NPAK", 4) != 0) { fclose(pak); snprintf(r.error, sizeof r.error, "nectar.pak: bad header"); return r; }
    const uint32_t n = (uint32_t)hd[8] | (uint32_t)hd[9] << 8 | (uint32_t)hd[10] << 16 | (uint32_t)hd[11] << 24;
    std::vector<Entry> idx(n);
    for (uint32_t i = 0; i < n; ++i) {
        unsigned char rec[16];
        if (fread(rec, 1, 16, pak) != 16) { fclose(pak); snprintf(r.error, sizeof r.error, "nectar.pak: index cut short"); return r; }
        uint64_t h = 0; for (int k = 7; k >= 0; --k) h = h << 8 | rec[k];
        idx[i].hash = h;
        idx[i].off = (uint32_t)rec[8] | (uint32_t)rec[9] << 8 | (uint32_t)rec[10] << 16 | (uint32_t)rec[11] << 24;
        idx[i].size = (uint32_t)rec[12] | (uint32_t)rec[13] << 8 | (uint32_t)rec[14] << 16 | (uint32_t)rec[15] << 24;
    }
    std::vector<Item> items;
    if (!walk(b + "/assets/dataDir", "dataDir/", items, r, fn, user)) { fclose(pak); return r; }
    r.stats.listed = (unsigned)items.size();
    std::vector<unsigned char> a(64 * 1024), c(64 * 1024);
    for (const Item& it : items) {
        r.stats.current = it.key.c_str();
        ++r.stats.done;
        const uint64_t h = pak_key_hash(it.key.c_str());
        auto e = std::lower_bound(idx.begin(), idx.end(), h, [](const Entry& x, uint64_t v) { return x.hash < v; });
        if (e == idx.end() || e->hash != h) { if (fn && !fn(r.stats, user)) break; continue; }      // not packed (too big etc.): read loose by the game
        ++r.stats.packed;
        bool same = false;
        FILE* in = fopen(it.full.c_str(), "rb");
        if (in) {
            fseek(in, 0, SEEK_END);
            const long sz = ftell(in);
            fseek(in, 0, SEEK_SET);
            same = sz == (long)e->size && fseek(pak, (long)e->off, SEEK_SET) == 0;
            for (uint64_t left = e->size; same && left; ) {
                const size_t k = left > a.size() ? a.size() : (size_t)left;
                same = fread(a.data(), 1, k, in) == k && fread(c.data(), 1, k, pak) == k && memcmp(a.data(), c.data(), k) == 0;
                left -= k;
            }
            fclose(in);
        }
        if (!same) { ++r.stats.skippedErr; if (!r.firstSkipped[0]) snprintf(r.firstSkipped, sizeof r.firstSkipped, "%s", it.full.c_str()); }
        if (fn && !fn(r.stats, user)) { fclose(pak); r.stats.current = nullptr; snprintf(r.error, sizeof r.error, "cancelled"); return r; }
    }
    fclose(pak);
    r.stats.current = nullptr;
    r.ok = r.stats.skippedErr == 0;
    if (!r.ok) snprintf(r.error, sizeof r.error, "%u packed files differ from the loose files", r.stats.skippedErr);
    return r;
}
