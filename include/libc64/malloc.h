#ifndef MALLOC_H
#define MALLOC_H

#include "types.h"

#ifdef TARGET_3DS
/* The game's arena allocator (src/static/libc64/malloc.c on the MallocInit block).
 * Only the files that include this header use it. The PC build maps these to the
 * system malloc and leaves the arena unused, which the 3DS cannot afford. */
#define malloc libc64_malloc
#define free libc64_free
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern void* malloc(size_t size);
extern void free(void* ptr);

extern void MallocInit(void* base, size_t len);
extern void MallocCleanup();
extern int  MallocIsInitalized();
extern void GetFreeArena(size_t* max_size, size_t* free_size, size_t* alloc_size);
extern int CheckArena(); /* @unused */
extern void DisplayArena();

#ifdef __cplusplus
}
#endif

#endif
