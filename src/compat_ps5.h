/* bbport PS5: what the runtime takes from Linux and the PS5's FreeBSD-based libkernel lacks
 * or names differently. Included by runtime.h when __PROSPERO__ (the payload SDK's clang). */
#ifndef BB_COMPAT_PS5_H
#define BB_COMPAT_PS5_H
#ifdef __PROSPERO__
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <pthread_np.h>

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000 /* never passed to the kernel: runtime_low_map is ours */
#endif
#ifndef CLOCK_REALTIME_COARSE
#define CLOCK_REALTIME_COARSE CLOCK_REALTIME_FAST
#endif
#ifndef CLOCK_MONOTONIC_COARSE
#define CLOCK_MONOTONIC_COARSE CLOCK_MONOTONIC_FAST
#endif
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC_PRECISE
#endif
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME CLOCK_UPTIME
#endif
#ifndef CLOCK_PROCESS_CPUTIME_ID
#define CLOCK_PROCESS_CPUTIME_ID CLOCK_PROF
#endif

#ifdef __cplusplus
extern "C" {
#endif
#define pthread_setname_np(thread, name) (pthread_set_name_np((thread), (name)), 0)
/* A thread's id, as gettid() on Linux. */
int bb_ps5_gettid(void);
#define gettid() bb_ps5_gettid()
/* getrandom(2) on arc4random_buf (bound to the platform layer's). */
ssize_t bb_ps5_getrandom(void *buffer, size_t length, unsigned flags);
#define getrandom(b, n, f) bb_ps5_getrandom((b), (n), (f))
/* GS base of the calling thread: the guest TCB (runtime_thread.c). 0 on success. */
int bb_ps5_set_gs(void *base);
/* Executable, read-write memory in direct memory below 1 TiB (the game image, its import
 * traps, guest stacks). NULL on failure. */
void *bb_ps5_low_map(size_t size, int prot);
/* klog + log file (ps5/src/ps5_main.c). */
void bb_ps5_log_line(const char *text);
/* The title's folder ("/app0" on the console). */
const char *bb_ps5_app_root(void);
/* Recursive mutexes have no static initializer in FreeBSD's libthr. */
void bb_ps5_recursive_mutex(pthread_mutex_t *mutex);
#ifdef __cplusplus
}
#endif
#endif
#endif
