/* bbport PS5: the console side of the runtime (src/compat_ps5.h): the guest TCB's GS base,
 * executable memory below 1 TiB for the game image, thread ids, randomness, the log.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "compat_ps5.h"

#include <ps5platform/kernel.h>
#include <ps5platform/klog.h>

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <pthread_np.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <machine/sysarch.h>
#include <unistd.h>
#include <signal.h>

/* ---- thread ids, randomness, mutexes ---- */
int bb_ps5_gettid(void) { return pthread_getthreadid_np(); }

void arc4random_buf(void *buffer, size_t length);
ssize_t bb_ps5_getrandom(void *buffer, size_t length, unsigned flags) {
    (void)flags;
    arc4random_buf(buffer, length);
    return (ssize_t)length;
}

void bb_ps5_recursive_mutex(pthread_mutex_t *mutex) {
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(mutex, &attributes);
    pthread_mutexattr_destroy(&attributes);
}

/* ---- the guest TCB: GS base ----
 * The loader rewrote the game's `mov reg, fs:[0]` TCB loads to gs:[0] (scripts/link_modules.py),
 * so each guest thread's GS base must be its TCB, whose first word points at itself. The
 * console's libkernel exports FreeBSD's sysarch; AMD64_SET_GSBASE is tried first, then the
 * WRGSBASE instruction (only if the kernel enabled FSGSBASE: a SIGILL otherwise, caught here
 * once). Each attempt is checked by reading gs:[0] back. */
static uint64_t read_gs0(void) {
    uint64_t value;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(value));
    return value;
}
enum { GS_UNKNOWN, GS_SYSARCH, GS_WRGSBASE, GS_NONE };
static _Atomic int gs_method = GS_UNKNOWN;
static _Thread_local sigjmp_buf *wrgsbase_probe;
static void wrgsbase_sigill(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)info; (void)context;
    if (wrgsbase_probe) siglongjmp(*wrgsbase_probe, 1);
    _exit(132);
}
static int try_wrgsbase(void *base) {
    struct sigaction catch_ill = {0}, previous;
    catch_ill.sa_sigaction = wrgsbase_sigill;
    catch_ill.sa_flags = SA_SIGINFO;
    sigemptyset(&catch_ill.sa_mask);
    sigaction(SIGILL, &catch_ill, &previous);
    sigjmp_buf here;
    volatile int ok = 0;
    if (!sigsetjmp(here, 1)) {
        wrgsbase_probe = &here;
        /* wrgsbase %rax, as bytes: no -mfsgsbase needed for one probe */
        __asm__ volatile(".byte 0xf3,0x48,0x0f,0xae,0xd8" ::"a"((uint64_t)(uintptr_t)base) : "memory");
        ok = 1;
    }
    wrgsbase_probe = NULL;
    sigaction(SIGILL, &previous, NULL);
    return ok;
}
int bb_ps5_set_gs(void *base) {
    const uint64_t expect = *(const uint64_t *)base;
    int method = atomic_load(&gs_method);
    if (method == GS_SYSARCH || method == GS_UNKNOWN) {
        void *value = base;
        int result = sysarch(AMD64_SET_GSBASE, &value);
        if (result == 0 && read_gs0() == expect) {
            if (method == GS_UNKNOWN) {
                atomic_store(&gs_method, GS_SYSARCH);
                printf("PS5: guest TCB through sysarch(AMD64_SET_GSBASE): ok\n");
            }
            return 0;
        }
        if (method == GS_SYSARCH) {
            fprintf(stderr, "PS5: sysarch(AMD64_SET_GSBASE) failed on a later thread (%d, errno %d)\n", result, errno);
            return -1;
        }
        fprintf(stderr, "PS5: sysarch(AMD64_SET_GSBASE) -> %d (errno %d), gs:[0]=0x%" PRIx64 " expected 0x%" PRIx64 "\n",
                result, errno, read_gs0(), expect);
    }
    if (method == GS_WRGSBASE || method == GS_UNKNOWN) {
        if (try_wrgsbase(base) && read_gs0() == expect) {
            if (method == GS_UNKNOWN) {
                atomic_store(&gs_method, GS_WRGSBASE);
                printf("PS5: guest TCB through WRGSBASE: ok\n");
            }
            return 0;
        }
        fprintf(stderr, "PS5: WRGSBASE unavailable\n");
    }
    atomic_store(&gs_method, GS_NONE);
    fprintf(stderr, "PS5: no way to set the GS base was granted to this title. Send this log: the "
                    "next build has to rewrite the game's TCB loads instead.\n");
    return -1;
}

/* ---- executable memory below 1 TiB ----
 * The game image (about 100 MiB with its libc and Fios2 modules), the import traps and guest
 * stacks: direct memory in a 4 GiB range reserved between the platform heap (0x20_0000_0000,
 * 16 GiB) and RADV's region (0x40_0000_0000). Mapped read-write; execute comes with the loader's
 * protection change (execute at map time is refused). An unmapped page follows each block. */
#define LOW_BASE UINT64_C(0x2800000000)
#define LOW_SPAN UINT64_C(0x100000000)
static pthread_mutex_t low_lock = PTHREAD_MUTEX_INITIALIZER;
static uintptr_t low_base, low_next, low_end;
void *bb_ps5_low_map(size_t size, int prot) {
    (void)prot;
    const size_t unit = PS5_KERNEL_DIRECT_ALIGNMENT;
    size = (size + unit - 1) & ~(unit - 1);
    pthread_mutex_lock(&low_lock);
    if (!low_base) {
        for (uint64_t hint = LOW_BASE; hint < UINT64_C(0x4000000000) && !low_base; hint += LOW_SPAN) {
            void *at = (void *)(uintptr_t)hint;
            int32_t result = sceKernelReserveVirtualRange(&at, LOW_SPAN, 0, unit);
            if (result == 0 && (uintptr_t)at == hint) { low_base = hint; break; }
            if (result == 0) sceKernelMunmap(at, LOW_SPAN);
        }
        if (!low_base) { pthread_mutex_unlock(&low_lock); fputs("PS5: no range for the game image\n", stderr); return NULL; }
        low_next = low_base; low_end = low_base + LOW_SPAN;
        printf("PS5: image and stack range at 0x%" PRIxPTR "\n", low_base);
    }
    if (low_next + size + unit > low_end) { pthread_mutex_unlock(&low_lock); fputs("PS5: image range full\n", stderr); return NULL; }
    int64_t start = -1;
    int32_t result = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, unit,
                                                   PS5_KERNEL_DIRECT_TYPE_CPU, &start);
    if (result) {
        pthread_mutex_unlock(&low_lock);
        fprintf(stderr, "PS5: direct memory for %zu bytes refused (0x%08x)\n", size, (unsigned)result);
        return NULL;
    }
    void *at = (void *)low_next;
    result = sceKernelMapDirectMemory(&at, size, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE,
                                      PS5_KERNEL_MAP_FIXED, start, PS5_KERNEL_PAGE_SIZE);
    if (result || (uintptr_t)at != low_next) {
        if (!result) sceKernelMunmap(at, size);
        sceKernelReleaseDirectMemory(start, size);
        pthread_mutex_unlock(&low_lock);
        fprintf(stderr, "PS5: mapping %zu bytes at 0x%" PRIxPTR " failed (0x%08x)\n", size, low_next, (unsigned)result);
        return NULL;
    }
    low_next += size + unit; /* the unit after it stays reserved and unmapped */
    pthread_mutex_unlock(&low_lock);
    memset(at, 0, size); /* direct memory comes back with what it held */
    return at;
}

/* ---- libc names no module a title loads exports (RADV imports them) ----
 * Bound by ps5/tools/link-title.sh (--defsym) where the SDK's platform layer has no ps5_ one. */
int ps5_mkstemps(char *path, int suffix_length);
int bb_ps5_mkstemp(char *path) { return ps5_mkstemps(path, 0); }
ssize_t bb_ps5_readlink(const char *path, char *buffer, size_t size) {
    (void)path; (void)buffer; (void)size;
    errno = EINVAL; /* nothing on the console is a symbolic link RADV asks about */
    return -1;
}

/* ---- named host threads (the GPU library's), for the heartbeat's stack dumps ---- */
#define HOST_THREADS 64
static struct { pthread_t thread; char name[48]; } host_threads[HOST_THREADS];
static int host_thread_count;
static pthread_mutex_t host_thread_lock = PTHREAD_MUTEX_INITIALIZER;
void bb_ps5_register_host_thread(const char *name) {
    pthread_mutex_lock(&host_thread_lock);
    int slot = -1;
    for (int i = 0; i < host_thread_count; ++i)
        if (pthread_equal(host_threads[i].thread, pthread_self())) slot = i;
    if (slot < 0 && host_thread_count < HOST_THREADS) slot = host_thread_count++;
    if (slot >= 0) {
        host_threads[slot].thread = pthread_self();
        snprintf(host_threads[slot].name, sizeof(host_threads[slot].name), "%s", name ? name : "?");
    }
    pthread_mutex_unlock(&host_thread_lock);
}
const char *bb_ps5_host_thread_name(void) {
    for (int i = 0; i < host_thread_count; ++i)
        if (pthread_equal(host_threads[i].thread, pthread_self())) return host_threads[i].name;
    return NULL;
}
void bb_ps5_signal_host_threads(int sig) {
    pthread_mutex_lock(&host_thread_lock);
    for (int i = 0; i < host_thread_count; ++i) pthread_kill(host_threads[i].thread, sig);
    pthread_mutex_unlock(&host_thread_lock);
}
