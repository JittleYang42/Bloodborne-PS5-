/* Linux pthread-backed opaque PS4 mutex handles. No guest threads yet. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
typedef struct { int type; } GuestAttr;
#ifdef __PROSPERO__
/* bbport PS5: guest mutexes and condition variables without one libkernel object each. The game
 * creates tens of thousands (71,684 mutexes and 21,547 condition variables two minutes in); a
 * pthread object each took libkernel's internal memory until "[ScePthread/System] Internal
 * Memory is running out" and a failed lock ended the game. Test build 6 waited with _umtx_op
 * instead, and its waits did not block on the console: workers woke with no job queued and
 * called garbage. Now every wait parks on one of 256 ordinary pthread mutex/condition pairs,
 * made once and chosen by the guest object's address: 512 libkernel objects in all. */
#include <stdatomic.h>
#include <pthread_np.h>
#define PARK_BUCKETS 256
typedef struct { pthread_mutex_t lock; pthread_cond_t wake; } ParkBucket;
static ParkBucket park[PARK_BUCKETS];
static pthread_once_t park_once = PTHREAD_ONCE_INIT;
static void park_init(void) {
    for (int i = 0; i < PARK_BUCKETS; ++i) {
        pthread_mutex_init(&park[i].lock, NULL);
        pthread_cond_init(&park[i].wake, NULL);
    }
}
static ParkBucket *bucket_of(const void *object) {
    pthread_once(&park_once, park_init);
    uintptr_t key = (uintptr_t)object >> 4;
    key ^= key >> 9; key ^= key >> 17;
    return &park[key % PARK_BUCKETS];
}
typedef struct {
    _Atomic uint32_t state;   /* 0 free, 1 locked, 2 locked with waiters */
    _Atomic int owner;        /* thread id, 0 when free */
    uint32_t recursion;       /* extra holds of a recursive mutex */
    int type;                 /* 1 errorcheck (default), 2 recursive, 3 normal, 4 adaptive */
} GuestMutex;
/* Waiters and pending wakeups, both under the bucket's lock: a signal wakes exactly one waiter
 * that was waiting when it came, a broadcast all of them, and nothing wakes a waiter otherwise. */
typedef struct { uint32_t waiters, tokens; } GuestCond;
static int self_id(void) { return pthread_getthreadid_np(); }
/* Blocks while *word == value (or until `end`, CLOCK_REALTIME); may return early. */
static void word_wait(_Atomic uint32_t *word, uint32_t value, const struct timespec *end) {
    ParkBucket *b = bucket_of(word);
    pthread_mutex_lock(&b->lock);
    if (atomic_load(word) == value) {
        if (end) pthread_cond_timedwait(&b->wake, &b->lock, end);
        else pthread_cond_wait(&b->wake, &b->lock);
    }
    pthread_mutex_unlock(&b->lock);
}
static void word_wake(_Atomic uint32_t *word) {
    ParkBucket *b = bucket_of(word);
    pthread_mutex_lock(&b->lock);
    pthread_cond_broadcast(&b->wake); /* the bucket is shared: every waiter checks its own word */
    pthread_mutex_unlock(&b->lock);
}
static int deadline_passed(const struct timespec *end) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return now.tv_sec > end->tv_sec || (now.tv_sec == end->tv_sec && now.tv_nsec >= end->tv_nsec);
}
/* Takes the lock word (Drepper's three-state mutex); `end` NULL waits for ever. */
static int lock_word(GuestMutex *m, const struct timespec *end) {
    uint32_t c = 0;
    if (atomic_compare_exchange_strong(&m->state, &c, 1)) return 0;
    for (int spin = 0; spin < 100; ++spin) {
        __builtin_ia32_pause();
        c = 0;
        if (atomic_compare_exchange_weak(&m->state, &c, 1)) return 0;
    }
    if (c != 2) c = atomic_exchange(&m->state, 2);
    while (c != 0) {
        if (end && deadline_passed(end)) return ETIMEDOUT;
        word_wait(&m->state, 2, end);
        c = atomic_exchange(&m->state, 2);
    }
    return 0;
}
static void unlock_word(GuestMutex *m) {
    if (atomic_fetch_sub(&m->state, 1) != 1) {
        atomic_store(&m->state, 0);
        word_wake(&m->state);
    }
}
static int ps5_lock(GuestMutex *m, const struct timespec *end, int try_only) {
    const int me = self_id();
    if (atomic_load_explicit(&m->owner, memory_order_relaxed) == me) {
        if (m->type == 2) { ++m->recursion; return 0; }
        if (m->type != 3) return try_only ? EBUSY : EDEADLK;
    }
    if (try_only) {
        uint32_t c = 0;
        if (!atomic_compare_exchange_strong(&m->state, &c, 1)) return EBUSY;
    } else {
        int e = lock_word(m, end);
        if (e) return e;
    }
    atomic_store_explicit(&m->owner, me, memory_order_relaxed);
    return 0;
}
static int ps5_unlock(GuestMutex *m) {
    if (m->type != 3 && atomic_load_explicit(&m->owner, memory_order_relaxed) != self_id()) return EPERM;
    if (m->recursion) { --m->recursion; return 0; }
    atomic_store_explicit(&m->owner, 0, memory_order_relaxed);
    unlock_word(m);
    return 0;
}
static int ps5_cond_wait(GuestCond *c, GuestMutex *m, const struct timespec *end) {
    if (m->type != 3 && atomic_load_explicit(&m->owner, memory_order_relaxed) != self_id()) return EPERM;
    ParkBucket *b = bucket_of(c);
    /* Counted as a waiter before the mutex is released: a signal sent after that release (and
     * before this thread sleeps) leaves its token, so it is not lost. */
    pthread_mutex_lock(&b->lock);
    ++c->waiters;
    pthread_mutex_unlock(&b->lock);
    const uint32_t recursion = m->recursion;
    m->recursion = 0;
    atomic_store_explicit(&m->owner, 0, memory_order_relaxed);
    unlock_word(m);
    int result = 0;
    pthread_mutex_lock(&b->lock);
    while (!c->tokens) {
        int e = end ? pthread_cond_timedwait(&b->wake, &b->lock, end) : pthread_cond_wait(&b->wake, &b->lock);
        if (e == ETIMEDOUT && !c->tokens) { result = ETIMEDOUT; break; }
    }
    if (!result) --c->tokens;
    --c->waiters;
    if (c->tokens > c->waiters) c->tokens = c->waiters;
    pthread_mutex_unlock(&b->lock);
    lock_word(m, NULL);
    atomic_store_explicit(&m->owner, self_id(), memory_order_relaxed);
    m->recursion = recursion;
    return result;
}
static void ps5_cond_signal(GuestCond *c, int all) {
    ParkBucket *b = bucket_of(c);
    pthread_mutex_lock(&b->lock);
    if (all) c->tokens = c->waiters;
    else if (c->tokens < c->waiters) ++c->tokens;
    pthread_cond_broadcast(&b->wake);
    pthread_mutex_unlock(&b->lock);
}
#else
typedef struct { pthread_mutex_t native; } GuestMutex;
#endif
static size_t created, locks, unlocks;
static int32_t orbis_error(int e) {
    if (!e) return 0;
    unsigned code;
    switch (e) {
    case EPERM: code=1; break;
    case ENOMEM: code=12; break;
    case EBUSY: code=16; break;
    case EINVAL: code=22; break;
    case EAGAIN: code=35; break;
    case EDEADLK: code=11; break;
    default: fprintf(stderr, "STOP: unmapped host pthread error %d\n", e); exit(21);
    }
    return (int32_t)(UINT32_C(0x80020000) | code);
}
static ABI int32_t attr_init(GuestAttr **out) {
    if (!out) return orbis_error(EINVAL);
    GuestAttr *attr = malloc(sizeof(*attr));
    if (!attr) return orbis_error(ENOMEM);
    attr->type = 1; *out = attr; return 0;
}
static ABI int32_t attr_type(GuestAttr **attr, int type) {
    if (!attr || !*attr || type < 1 || type > 4) return orbis_error(EINVAL);
    (*attr)->type = type; return 0;
}
/* Priority protocols need a PS4 scheduler; the value is validated and recorded only. */
static ABI int32_t attr_protocol(GuestAttr **attr, int protocol) {
    if (!attr || !*attr || protocol < 0 || protocol > 2) return orbis_error(EINVAL);
    return 0;
}
static ABI int32_t attr_destroy(GuestAttr **attr) {
    if (!attr || !*attr) return orbis_error(EINVAL);
    free(*attr); *attr = NULL; return 0;
}
static ABI int32_t mutex_init(GuestMutex **out, GuestAttr **attr, const char *name) {
    (void)name;
    if (!out || (attr && !*attr)) return orbis_error(EINVAL);
    int type = attr ? (*attr)->type : 1;
    if (type < 1 || type > 4) return orbis_error(EINVAL);
    GuestMutex *mutex = malloc(sizeof(*mutex));
    if (!mutex) return orbis_error(ENOMEM);
#ifdef __PROSPERO__
    memset(mutex, 0, sizeof(*mutex));
    mutex->type = type;
    *out = mutex; ++created; return 0;
#else
    pthread_mutexattr_t native_attr;
    int e = pthread_mutexattr_init(&native_attr);
    if (e) { free(mutex); return orbis_error(e); }
    int native_type = type == 2 ? PTHREAD_MUTEX_RECURSIVE : type == 3 ? PTHREAD_MUTEX_NORMAL : PTHREAD_MUTEX_ERRORCHECK;
    e = pthread_mutexattr_settype(&native_attr, native_type);
    if (!e) e = pthread_mutex_init(&mutex->native, &native_attr);
    pthread_mutexattr_destroy(&native_attr);
    if (e) { free(mutex); return orbis_error(e); }
    *out = mutex; ++created; return 0;
#endif
}
static pthread_mutex_t static_init = PTHREAD_MUTEX_INITIALIZER;
/* Statically initialized handles (0/1) are created once, even under contention. */
static int32_t ensure_mutex(GuestMutex **mutex) {
    if (!mutex) return orbis_error(EINVAL);
    uintptr_t value = __atomic_load_n((uintptr_t *)mutex, __ATOMIC_ACQUIRE);
    if (value == 2) return orbis_error(EINVAL);
    if (value >= 2) return 0;
    pthread_mutex_lock(&static_init);
    int32_t e = 0;
    if ((uintptr_t)*mutex < 2) {
        GuestMutex *created_mutex = NULL;
        e = mutex_init(&created_mutex, NULL, NULL);
        if (!e) __atomic_store_n(mutex, created_mutex, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&static_init);
    return e;
}
static ABI int32_t mutex_lock(GuestMutex **mutex) {
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
#ifdef __PROSPERO__
    e = orbis_error(ps5_lock(*mutex, NULL, 0));
#else
    e = orbis_error(pthread_mutex_lock(&(*mutex)->native));
#endif
    if (!e) ++locks;
    return e;
}
static ABI int32_t mutex_trylock(GuestMutex **mutex) {
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
#ifdef __PROSPERO__
    e = orbis_error(ps5_lock(*mutex, NULL, 1));
#else
    e = orbis_error(pthread_mutex_trylock(&(*mutex)->native));
#endif
    if (!e) ++locks;
    return e;
}
static ABI int32_t mutex_unlock(GuestMutex **mutex) {
    if (!mutex || (uintptr_t)*mutex == 2) return orbis_error(EINVAL);
    if ((uintptr_t)*mutex < 2) return orbis_error(EPERM);
#ifdef __PROSPERO__
    int32_t e = orbis_error(ps5_unlock(*mutex));
#else
    int32_t e = orbis_error(pthread_mutex_unlock(&(*mutex)->native));
#endif
    if (!e) ++unlocks;
    return e;
}
static ABI int32_t mutex_destroy(GuestMutex **mutex) {
    if (!mutex || (uintptr_t)*mutex == 2) return orbis_error(EINVAL);
    if ((uintptr_t)*mutex < 2) return 0;
#ifdef __PROSPERO__
    int e = atomic_load(&(*mutex)->state) ? EBUSY : 0;
#else
    int e = pthread_mutex_destroy(&(*mutex)->native);
#endif
    if (!e) { free(*mutex); *mutex = (GuestMutex *)(uintptr_t)2; }
    return orbis_error(e);
}
static int deadline_after(struct timespec *end, uint64_t usec) {
    if (clock_gettime(CLOCK_REALTIME, end)) return errno;
    uint64_t ns = (uint64_t)end->tv_nsec + (usec % 1000000) * 1000;
    end->tv_sec += (time_t)(usec / 1000000 + ns / 1000000000);
    end->tv_nsec = (long)(ns % 1000000000);
    return 0;
}
static int32_t timed_error(int e) { return e == ETIMEDOUT ? (int32_t)UINT32_C(0x8002003c) : orbis_error(e); }
static ABI int32_t mutex_timedlock(GuestMutex **mutex, uint32_t usec) {
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
    struct timespec end;
    if (deadline_after(&end, usec)) return orbis_error(EINVAL);
#ifdef __PROSPERO__
    e = timed_error(ps5_lock(*mutex, &end, 0));
#else
    e = timed_error(pthread_mutex_timedlock(&(*mutex)->native, &end));
#endif
    if (!e) ++locks;
    return e;
}
/* Condition variables: opaque handles over host pthread_cond (CLOCK_REALTIME,
 * like FreeBSD's default), sharing the host mutex behind GuestMutex. */
#ifndef __PROSPERO__
typedef struct { pthread_cond_t native; } GuestCond;
#endif
static size_t conds, waits, wakeups;
static ABI int32_t cond_init(GuestCond **out, void **attr, const char *name) {
    (void)attr; (void)name;
    if (!out) return orbis_error(EINVAL);
    GuestCond *c = malloc(sizeof(*c));
    if (!c) return orbis_error(ENOMEM);
#ifdef __PROSPERO__
    c->waiters = c->tokens = 0;
    int e = 0;
#else
    int e = pthread_cond_init(&c->native, NULL);
#endif
    if (e) { free(c); return orbis_error(e); }
    *out = c; ++conds; return 0;
}
static int32_t ensure_cond(GuestCond **cond) {
    if (!cond) return orbis_error(EINVAL);
    if (__atomic_load_n((uintptr_t *)cond, __ATOMIC_ACQUIRE) >= 2) return 0;
    pthread_mutex_lock(&static_init);
    int32_t e = 0;
    if ((uintptr_t)*cond < 2) {
        GuestCond *c = NULL;
        e = cond_init(&c, NULL, NULL);
        if (!e) __atomic_store_n(cond, c, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&static_init);
    return e;
}
static ABI int32_t cond_destroy(GuestCond **cond) {
    if (!cond) return orbis_error(EINVAL);
    if ((uintptr_t)*cond < 2) return 0;
#ifdef __PROSPERO__
    int e = 0;
#else
    int e = pthread_cond_destroy(&(*cond)->native);
#endif
    if (!e) { free(*cond); *cond = NULL; }
    return orbis_error(e);
}
static ABI int32_t cond_wait(GuestCond **cond, GuestMutex **mutex) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    if (!mutex || (uintptr_t)*mutex < 3) return orbis_error(EINVAL);
    ++waits;
#ifdef __PROSPERO__
    return orbis_error(ps5_cond_wait(*cond, *mutex, NULL));
#else
    return orbis_error(pthread_cond_wait(&(*cond)->native, &(*mutex)->native));
#endif
}
static int32_t cond_wait_until(GuestCond **cond, GuestMutex **mutex, const struct timespec *end) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    if (!mutex || (uintptr_t)*mutex < 3 || !end) return orbis_error(EINVAL);
    ++waits;
#ifdef __PROSPERO__
    return timed_error(ps5_cond_wait(*cond, *mutex, end));
#else
    return timed_error(pthread_cond_timedwait(&(*cond)->native, &(*mutex)->native, end));
#endif
}
static ABI int32_t cond_timedwait(GuestCond **cond, GuestMutex **mutex, uint32_t usec) {
    struct timespec end;
    if (deadline_after(&end, usec)) return orbis_error(EINVAL);
    return cond_wait_until(cond, mutex, &end);
}
static ABI int32_t cond_signal(GuestCond **cond) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
#ifdef __PROSPERO__
    ++wakeups; ps5_cond_signal(*cond, 0); return 0;
#else
    ++wakeups; return orbis_error(pthread_cond_signal(&(*cond)->native));
#endif
}
static ABI int32_t cond_broadcast(GuestCond **cond) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
#ifdef __PROSPERO__
    ++wakeups; ps5_cond_signal(*cond, 1); return 0;
#else
    ++wakeups; return orbis_error(pthread_cond_broadcast(&(*cond)->native));
#endif
}
/* PS4 struct timespec is {int64 sec, int64 nsec}, identical to Linux x86-64. */
typedef struct { int64_t sec, nsec; } GuestTimespec;
static ABI int32_t cond_abs_timedwait(GuestCond **cond, GuestMutex **mutex, const GuestTimespec *abs) {
    if (!abs || abs->nsec < 0 || abs->nsec >= 1000000000) return orbis_error(EINVAL);
    struct timespec end = {.tv_sec = (time_t)abs->sec, .tv_nsec = (long)abs->nsec};
    return cond_wait_until(cond, mutex, &end);
}
/* POSIX pthread APIs return positive guest errno values, unlike scePthread. */
static int32_t posix_result(int32_t result) { return result ? (int32_t)((uint32_t)result & 0xffff) : 0; }
static ABI int32_t posix_attr_init(GuestAttr **out) { return posix_result(attr_init(out)); }
static ABI int32_t posix_attr_type(GuestAttr **attr,int type) { return posix_result(attr_type(attr,type)); }
static ABI int32_t posix_attr_destroy(GuestAttr **attr) { return posix_result(attr_destroy(attr)); }
static ABI int32_t posix_mutex_init(GuestMutex **out,GuestAttr **attr) { return posix_result(mutex_init(out,attr,NULL)); }
static ABI int32_t posix_mutex_destroy(GuestMutex **m) { return posix_result(mutex_destroy(m)); }
static ABI int32_t posix_mutex_lock(GuestMutex **m) { return posix_result(mutex_lock(m)); }
static ABI int32_t posix_mutex_trylock(GuestMutex **m) { return posix_result(mutex_trylock(m)); }
static ABI int32_t posix_mutex_unlock(GuestMutex **m) { return posix_result(mutex_unlock(m)); }
static ABI int32_t posix_cond_init(GuestCond **c, void **a) { return posix_result(cond_init(c, a, NULL)); }
static ABI int32_t posix_cond_destroy(GuestCond **c) { return posix_result(cond_destroy(c)); }
static ABI int32_t posix_cond_wait(GuestCond **c, GuestMutex **m) { return posix_result(cond_wait(c, m)); }
static ABI int32_t posix_cond_timedwait(GuestCond **c, GuestMutex **m, const GuestTimespec *t) { return posix_result(cond_abs_timedwait(c, m, t)); }
static ABI int32_t posix_cond_signal(GuestCond **c) { return posix_result(cond_signal(c)); }
static ABI int32_t posix_cond_broadcast(GuestCond **c) { return posix_result(cond_broadcast(c)); }
uintptr_t runtime_mutex_resolve(const char *name) {
    if (!strcmp(name,"0TyVk4MSLt0#I#J")) return (uintptr_t)posix_cond_init;
    if (!strcmp(name,"RXXqi4CtF8w#I#J")) return (uintptr_t)posix_cond_destroy;
    if (!strcmp(name,"Op8TBGY5KHg#I#J")) return (uintptr_t)posix_cond_wait;
    if (!strcmp(name,"27bAgiJmOh0#I#J")) return (uintptr_t)posix_cond_timedwait;
    if (!strcmp(name,"2MOy+rUfuhQ#I#J")) return (uintptr_t)posix_cond_signal;
    if (!strcmp(name,"mkx2fVhNMsg#I#J")) return (uintptr_t)posix_cond_broadcast;
    if (!strcmp(name,"2Tb92quprl0#p#J")) return (uintptr_t)cond_init;
    if (!strcmp(name,"g+PZd2hiacg#p#J")) return (uintptr_t)cond_destroy;
    if (!strcmp(name,"WKAXJ4XBPQ4#p#J")) return (uintptr_t)cond_wait;
    if (!strcmp(name,"BmMjYxmew1w#p#J")) return (uintptr_t)cond_timedwait;
    if (!strcmp(name,"kDh-NfxgMtE#p#J")) return (uintptr_t)cond_signal;
    if (!strcmp(name,"JGgj7Uvrl+A#p#J")) return (uintptr_t)cond_broadcast;
    if (!strcmp(name,"IafI2PxcPnQ#p#J")) return (uintptr_t)mutex_timedlock;
    if (!strcmp(name,"dQHWEsJtoE4#I#J")) return (uintptr_t)posix_attr_init;
    if (!strcmp(name,"mDmgMOGVUqg#I#J")) return (uintptr_t)posix_attr_type;
    if (!strcmp(name,"HF7lK46xzjY#I#J")) return (uintptr_t)posix_attr_destroy;
    if (!strcmp(name,"ttHNfU+qDBU#I#J")) return (uintptr_t)posix_mutex_init;
    if (!strcmp(name,"ltCfaGr2JGE#I#J")) return (uintptr_t)posix_mutex_destroy;
    if (!strcmp(name,"7H0iTOciTLo#I#J")) return (uintptr_t)posix_mutex_lock;
    if (!strcmp(name,"K-jXhbt2gn4#I#J")) return (uintptr_t)posix_mutex_trylock;
    if (!strcmp(name,"2Z+PpY6CaJg#I#J")) return (uintptr_t)posix_mutex_unlock;
    if (!strcmp(name, "F8bUHwAG284#p#J")) return (uintptr_t)attr_init;
    if (!strcmp(name, "iMp8QpE+XO4#p#J")) return (uintptr_t)attr_type;
    if (!strcmp(name, "smWEktiyyG0#p#J")) return (uintptr_t)attr_destroy;
    if (!strcmp(name, "1FGvU0i9saQ#p#J")) return (uintptr_t)attr_protocol;
    if (!strcmp(name, "cmo1RIYva9o#p#J")) return (uintptr_t)mutex_init;
    if (!strcmp(name, "9UK1vLZQft4#p#J")) return (uintptr_t)mutex_lock;
    if (!strcmp(name, "upoVrzMHFeE#p#J")) return (uintptr_t)mutex_trylock;
    if (!strcmp(name, "tn3VlD0hG60#p#J")) return (uintptr_t)mutex_unlock;
    if (!strcmp(name, "2Of0f+3mhhE#p#J")) return (uintptr_t)mutex_destroy;
    return 0;
}
void runtime_mutex_report(void) {
    printf("Runtime: mutexes created=%zu, locks=%zu, unlocks=%zu\n", created, locks, unlocks);
    printf("Runtime: condition variables created=%zu, waits=%zu, wakeups=%zu\n", conds, waits, wakeups);
}

