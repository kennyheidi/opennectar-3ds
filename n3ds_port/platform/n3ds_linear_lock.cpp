// Serialises the libctru linear-heap allocator (linearAlloc / linearMemAlign / linearRealloc / linearFree / linearSpaceFree).
// libctru's linear allocator keeps an UNSYNCHRONISED free list. Since the render worker runs on core 2 it creates and frees citro3d
// textures (linearMemAlign / linearFree) while the game thread can hit the same allocator: n3ds_alloc_trace.cpp serves malloc() from the
// linear heap whenever the normal heap is full (which is nearly always), and frees those blocks on the game thread. Two threads inside
// the allocator at once corrupt the free list: random crashes or hangs, usually long after the cause. Same idea as melee-3ds's
// linear_lock.c (read for the problem statement; this is a from-scratch implementation).
// Hooked with the linker (-Wl,--wrap=...), see CMakeLists.txt. The lock is recursive because linearAlloc calls linearMemAlign.
#include <3ds.h>
#include <stddef.h>

extern "C" {
void* __real_linearAlloc(size_t);
void* __real_linearMemAlign(size_t, size_t);
void* __real_linearRealloc(void*, size_t);
void  __real_linearFree(void*);
u32   __real_linearSpaceFree(void);

static RecursiveLock sLinearLock = { 1, 0, 0 };    // statically initialised: the first allocation happens before any constructor can be relied on

void* __wrap_linearAlloc(size_t n)                  { RecursiveLock_Lock(&sLinearLock); void* p = __real_linearAlloc(n);         RecursiveLock_Unlock(&sLinearLock); return p; }
void* __wrap_linearMemAlign(size_t n, size_t align) { RecursiveLock_Lock(&sLinearLock); void* p = __real_linearMemAlign(n, align); RecursiveLock_Unlock(&sLinearLock); return p; }
void* __wrap_linearRealloc(void* m, size_t n)       { RecursiveLock_Lock(&sLinearLock); void* p = __real_linearRealloc(m, n);     RecursiveLock_Unlock(&sLinearLock); return p; }
void  __wrap_linearFree(void* m)                    { RecursiveLock_Lock(&sLinearLock); __real_linearFree(m);                     RecursiveLock_Unlock(&sLinearLock); }
u32   __wrap_linearSpaceFree(void)                  { RecursiveLock_Lock(&sLinearLock); u32 n = __real_linearSpaceFree();         RecursiveLock_Unlock(&sLinearLock); return n; }
}
