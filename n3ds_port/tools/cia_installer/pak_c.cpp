// C wrapper around the nectar.pak builder (tools/pakbuild/pak_builder.cpp), so the C installer can build / verify the pak itself.
#include "pak_c.h"
#include "pak_builder.h"
#include <string.h>

namespace {
struct Thunk { NpProgressFn fn; void* user; };

NpProgress toC(const PakProgress& p)
{
    NpProgress o;
    o.listed = p.listed; o.done = p.done; o.packed = p.packed;
    o.skippedBig = p.skippedBig; o.skippedExt = p.skippedExt; o.skippedErr = p.skippedErr;
    o.bytes = p.bytes; o.current = p.current;
    return o;
}

bool relay(const PakProgress& p, void* user)
{
    const Thunk* t = static_cast<const Thunk*>(user);
    const NpProgress c = toC(p);
    return t->fn ? t->fn(&c, t->user) != 0 : true;
}

void copyOut(const PakResult& r, NpResult* out)
{
    out->ok = r.ok ? 1 : 0;
    memcpy(out->error, r.error, sizeof out->error);
    out->error[sizeof out->error - 1] = 0;
    out->stats = toC(r.stats);
    memcpy(out->firstSkipped, r.firstSkipped, sizeof out->firstSkipped);
    out->firstSkipped[sizeof out->firstSkipped - 1] = 0;
}
} // namespace

extern "C" void np_build(const char* base, unsigned long long maxFileBytes, NpProgressFn fn, void* user, NpResult* out)
{
    Thunk t = { fn, user };
    copyOut(pak_build(base, (uint64_t)maxFileBytes, relay, &t), out);
}

extern "C" void np_verify(const char* base, NpProgressFn fn, void* user, NpResult* out)
{
    Thunk t = { fn, user };
    copyOut(pak_verify(base, relay, &t), out);
}
