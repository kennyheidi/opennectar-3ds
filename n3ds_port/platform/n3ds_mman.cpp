// sysNew.cpp maps anonymous zero pages for allocations >= 64 MB. There is no mmap on the 3DS; the
// shim header sys/mman.h redirects mmap/munmap here. A block that big will almost certainly fail on the
// console -- returning MAP_FAILED lets the game's own fallback path run, and we log it.
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
extern "C" void n3ds_log(const char* fmt, ...);

extern "C" void* n3ds_mmap(void*, size_t len, int, int, int, long)
{
    void* p = calloc(1, len);
    if (!p) { n3ds_log("[n3ds_mmap] FAILED for %lu KB", (unsigned long)(len / 1024)); return (void*)-1; }
    return p;
}
extern "C" int n3ds_munmap(void* addr, size_t) { free(addr); return 0; }
