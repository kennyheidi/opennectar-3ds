// newlib (devkitARM) has no mman.h. sysNew.cpp maps anonymous pages for large allocations;
// n3ds_port/platform provides these two calls on top of the linear/app heap.
#ifndef N3DS_MMAN_SHIM_H
#define N3DS_MMAN_SHIM_H
#include <stddef.h>
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define MAP_NORESERVE 0x4000
#define MAP_FAILED ((void*)-1)
#ifdef __cplusplus
extern "C" {
#endif
void* n3ds_mmap(void* addr, size_t len, int prot, int flags, int fd, long off);
int   n3ds_munmap(void* addr, size_t len);
#ifdef __cplusplus
}
#endif
#define mmap  n3ds_mmap
#define munmap n3ds_munmap
#endif
