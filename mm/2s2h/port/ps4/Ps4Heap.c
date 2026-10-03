// PS4: give the C heap its own big block of memory.
//
// The C library (musl) gets all of its heap from anonymous mmap() calls. On PS4 those are served
// from the application's "flexible memory", a pool of only ~255 MiB that Piglet also keeps its
// textures in. mmap()/munmap() are therefore replaced here: anonymous read/write mappings are
// carved out of one arena, everything else is forwarded to the kernel.
//
// The arena comes from, in order of preference:
//  1. system flexible memory (a separate pool this kind of application may map, the same thing
//     the Super Mario 64 port does), which leaves all of the direct memory to Piglet;
//  2. direct memory, of which the application only has 768 MiB in total.

#ifdef __PS4__

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <orbis/libkernel.h>

#define PS4_PAGE_SIZE 0x4000ULL
#define PS4_ARENA_ALIGN 0x200000ULL
#define PS4_SYSTEM_ARENA_SIZE (1024ULL * 1024 * 1024)
#define PS4_MAP_FIXED 0x10

// Not (properly) declared by the OpenOrbis headers.
int32_t Ps4Heap_MapNamedSystemFlexibleMemory(void** address, size_t length, int32_t protection, int32_t flags,
                                             const char* name) __asm__("sceKernelMapNamedSystemFlexibleMemory");
// Leaves the rest of the direct memory to Piglet (video memory) and the system libraries.
#define PS4_ARENA_MAX_SIZE (1024ULL * 1024 * 1024)
#define PS4_ARENA_MIN_SIZE (256ULL * 1024 * 1024)
// Allocations of at least this many pages (1 MiB) are placed top-down.
#define PS4_LARGE_PAGES 64

static uint8_t* sArenaBase = NULL;
static size_t sArenaPages = 0;
// One byte per page: 0 = free, 1 = in use. Lives in the first pages of the arena itself.
static uint8_t* sPageMap = NULL;
static size_t sSearchHint = 0;
static size_t sPagesInUse = 0;
static size_t sPagesInUsePeak = 0;
static int sInitDone = 0;
// 0 = none, 1 = system flexible memory, 2 = direct memory
static int sArenaKind = 0;
static volatile int sLock = 0;

static void Ps4Heap_Lock(void) {
    while (__sync_lock_test_and_set(&sLock, 1)) {
        while (sLock) {
            __builtin_ia32_pause();
        }
    }
}

static void Ps4Heap_Unlock(void) {
    __sync_lock_release(&sLock);
}

static void Ps4Heap_Init(void) {
    sInitDone = 1;

    const size_t directMemorySize = sceKernelGetDirectMemorySize();
    size_t size = PS4_SYSTEM_ARENA_SIZE;

    {
        void* address = NULL;
        if (sceKernelReserveVirtualRange(&address, size, 0, PS4_PAGE_SIZE) == 0 && address != NULL) {
            if (Ps4Heap_MapNamedSystemFlexibleMemory(&address, size, ORBIS_KERNEL_PROT_CPU_RW, PS4_MAP_FIXED,
                                                     "2S2H heap") == 0) {
                sArenaBase = (uint8_t*)address;
                sArenaKind = 1;
            } else {
                sceKernelMunmap(address, size);
            }
        }
    }

    if (sArenaBase == NULL) {
        size = PS4_ARENA_MAX_SIZE;
    }

    while (sArenaBase == NULL && size >= PS4_ARENA_MIN_SIZE) {
        off_t physicalAddress = 0;
        // Prefer the top of the physical range: Piglet and the system libraries allocate from the
        // bottom and some of them are picky about where their memory sits.
        int32_t allocated = -1;
        if (directMemorySize > size) {
            allocated = sceKernelAllocateDirectMemory((off_t)(directMemorySize - size), (off_t)directMemorySize, size,
                                                      PS4_ARENA_ALIGN, ORBIS_KERNEL_WB_ONION, &physicalAddress);
        }
        if (allocated != 0) {
            allocated = sceKernelAllocateDirectMemory(0, (off_t)directMemorySize, size, PS4_ARENA_ALIGN,
                                                      ORBIS_KERNEL_WB_ONION, &physicalAddress);
        }
        if (allocated == 0) {
            void* address = NULL;
            if (sceKernelMapDirectMemory(&address, size, ORBIS_KERNEL_PROT_CPU_RW, 0, physicalAddress,
                                         PS4_ARENA_ALIGN) == 0) {
                sArenaBase = (uint8_t*)address;
                sArenaKind = 2;
                break;
            }
            sceKernelReleaseDirectMemory(physicalAddress, size);
        }
        size /= 2;
    }

    if (sArenaBase == NULL) {
        return;
    }

    sArenaPages = size / PS4_PAGE_SIZE;
    const size_t mapPages = (sArenaPages + PS4_PAGE_SIZE - 1) / PS4_PAGE_SIZE;
    sPageMap = sArenaBase;
    memset(sPageMap, 0, mapPages * PS4_PAGE_SIZE);
    memset(sPageMap, 1, mapPages);
    sSearchHint = mapPages;
    sPagesInUse = mapPages;
    sPagesInUsePeak = mapPages;
}

static void* Ps4Heap_Alloc(size_t length) {
    const size_t pages = (length + PS4_PAGE_SIZE - 1) / PS4_PAGE_SIZE;
    void* result = NULL;

    Ps4Heap_Lock();

    if (!sInitDone) {
        Ps4Heap_Init();
    }

    if (sArenaBase != NULL && pages != 0 && pages <= sArenaPages) {
        size_t first = (size_t)-1;

        if (pages >= PS4_LARGE_PAGES) {
            // Large blocks (textures, archives, the heap extensions themselves) are taken from the
            // top of the arena so they don't get interleaved with the small short-lived ones.
            size_t run = 0;
            for (size_t i = sArenaPages; i-- > 0;) {
                if (sPageMap[i] != 0) {
                    run = 0;
                    continue;
                }
                if (++run == pages) {
                    first = i;
                    break;
                }
            }
        } else {
            // Next fit from the bottom: start where the last search ended, wrap around once.
            for (int pass = 0; pass < 2 && first == (size_t)-1; pass++) {
                const size_t start = (pass == 0) ? sSearchHint : 0;
                const size_t end = (pass == 0) ? sArenaPages : sSearchHint + pages;
                size_t run = 0;

                for (size_t i = start; i < end && i < sArenaPages; i++) {
                    if (sPageMap[i] != 0) {
                        run = 0;
                        continue;
                    }
                    if (++run == pages) {
                        first = i + 1 - pages;
                        sSearchHint = i + 1;
                        break;
                    }
                }
            }
        }

        if (first != (size_t)-1) {
            memset(sPageMap + first, 1, pages);
            sPagesInUse += pages;
            if (sPagesInUse > sPagesInUsePeak) {
                sPagesInUsePeak = sPagesInUse;
            }
            result = sArenaBase + first * PS4_PAGE_SIZE;
        }
    }

    Ps4Heap_Unlock();

    if (result != NULL) {
        // Anonymous mappings are guaranteed to read back as zero (calloc relies on it).
        memset(result, 0, pages * PS4_PAGE_SIZE);
    }

    return result;
}

static int Ps4Heap_Contains(const void* address) {
    return sArenaBase != NULL && (const uint8_t*)address >= sArenaBase &&
           (const uint8_t*)address < sArenaBase + sArenaPages * PS4_PAGE_SIZE;
}

static void Ps4Heap_Free(void* address, size_t length) {
    const size_t first = (size_t)((uint8_t*)address - sArenaBase) / PS4_PAGE_SIZE;
    size_t pages = (length + PS4_PAGE_SIZE - 1) / PS4_PAGE_SIZE;

    Ps4Heap_Lock();
    if (first + pages > sArenaPages) {
        pages = sArenaPages - first;
    }
    for (size_t i = first; i < first + pages; i++) {
        if (sPageMap[i] != 0) {
            sPageMap[i] = 0;
            sPagesInUse--;
        }
    }
    Ps4Heap_Unlock();
}

void* mmap(void* address, size_t length, int protection, int flags, int fd, off_t offset) {
    if (address == NULL && fd == -1 && (flags & MAP_ANON) != 0 && (flags & MAP_FIXED) == 0 &&
        protection == (PROT_READ | PROT_WRITE)) {
        void* result = Ps4Heap_Alloc(length);
        if (result != NULL) {
            return result;
        }
    }

    void* mapped = NULL;
    const int32_t result = sceKernelMmap(address, length, protection, flags, fd, offset, &mapped);
    if (result < 0) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return mapped;
}

int munmap(void* address, size_t length) {
    if (Ps4Heap_Contains(address)) {
        Ps4Heap_Free(address, length);
        return 0;
    }

    if (sceKernelMunmap(address, length) < 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

// 0 = no arena (plain mmap), 1 = system flexible memory, 2 = direct memory
int Ps4Heap_GetKind(void) {
    return sArenaKind;
}

// For the log: how much of the arena is (and was at most) in use, in bytes.
void Ps4Heap_GetStats(size_t* arenaSize, size_t* inUse, size_t* peak) {
    *arenaSize = sArenaPages * PS4_PAGE_SIZE;
    *inUse = sPagesInUse * PS4_PAGE_SIZE;
    *peak = sPagesInUsePeak * PS4_PAGE_SIZE;
}

#endif // __PS4__
