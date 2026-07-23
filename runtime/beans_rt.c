// beans native runtime — reference-counted heap + cycle collector.
// Every heap value has a 16-byte header just before its payload:
//   { long long rc, long long meta }   (rc ops turn atomic at first spawn)
// meta bits 0-2 = kind, bits 3-60 = per-kind shape payload:
//   0 leaf | 1 fixed (bitmask of pointer slots) | 2 list (elem_ptr)
//   3 map (key_ptr | val_ptr<<1) | 4 chan (elem_ptr) | 5 mutex (inner_ptr)
//   6 OS resource (shape bit 0: 0 = file — drop closes the fd,
//                               1 = mmap — drop unmaps)
//   7 arena (elem_ptr)
// meta bits 61-62 = collector color, bit 63 = in the root buffer.
// String constants carry an immortal header emitted by the compiler.
//
// Cycles: plain RC can't free A<->B. The collector is Bacon-Rajan trial
// deletion (Nim's ORC family): a decrement that doesn't reach zero parks the
// object as a possible cycle root; a collection trial-deletes each root's
// subgraph, restores whatever still has external counts, frees the rest.
// It only runs when no worker threads are live (checked in beans_alloc and
// at exit), so the mutator is exactly one thread: ourselves, between
// statements. Everything is iterative — a million-node ring must not
// overflow the C stack.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BEANS_IMMORTAL (1LL << 62)

// rc layout: bits 0-47 the count, bits 48-59 the allocation size class
// (0 = plain malloc), bit 62 immortal. Retain/release preserve the class
// bits by adding/subtracting 1; every test of the COUNT must mask with
// RC_COUNT, and class 4095 * 16 bytes stays far under the immortal bit.
#define RC_CLS_SHIFT 48
#define RC_CLS_MAX 4095LL
#define RC_COUNT(v) ((v) & ((1LL << RC_CLS_SHIFT) - 1))
// rc bit 61: this object's class chain has a deinit — user code runs when the
// count hits zero. Lives in the rc word (not meta) so pointer-mask walkers and
// shell frees never see it; retain/release arithmetic can't carry into it.
#define RC_FIN (1LL << 61)

// meta layout
#define CC_SHAPE ((1LL << 61) - 1)
#define CC_COLOR (3LL << 61)
#define CC_BLACK 0LL
#define CC_GRAY (1LL << 61)
#define CC_WHITE (2LL << 61)
#define CC_PURPLE (3LL << 61)
#define CC_BUF ((long long)(1ULL << 63))

typedef struct {
    long long rc;
    long long meta;
} BHead;

static BHead* head_of(void* p) { return (BHead*)((char*)p - 16); }

// counts are plain until the first thread spawns (cc_mt flips before
// pthread_create, so no object is ever touched by two threads while the
// flag is 0); after that retain/release use atomic ops. The collector
// keeps plain ops either way — it only runs with zero workers live.
static int cc_mt;
static int cc_is_mt(void) {
    return __atomic_load_n(&cc_mt, __ATOMIC_RELAXED);
}
static void cc_enable_mt(void) {
    __atomic_store_n(&cc_mt, 1, __ATOMIC_RELAXED);
}
static long long cc_color(BHead* h) { return h->meta & CC_COLOR; }
static void cc_set_color(BHead* h, long long c) { h->meta = (h->meta & ~CC_COLOR) | c; }

static _Atomic long long cc_threads;  // live worker threads; collect only at 0
static _Atomic int cc_pending;
static int cc_collecting;
static void cc_collect(int force);

// vtable slot of deinit, emitted by codegen (-1 when no class has one).
// Deinit runs inside a release cascade, where allocation used to be
// impossible — beans_in_deinit keeps the collector out of that window,
// because a mid-destroy object must never be walked.
extern long long beans_deinit_sel;
// NOT thread-local: a TLS read compiles to a _tlv_get_addr call. A shared
// flag is exactly as correct — the collector only runs with zero worker
// threads, so "any thread is mid-deinit" is the right gate anyway. Plain
// int + __atomic builtins (an _Atomic type rejects __atomic_add_fetch).
static int beans_in_deinit;

// Profile builds recompile the emitted runtime with -DBEANS_ARC_STATS.
// Normal benchmark binaries do not contain these counters, so measuring
// ownership traffic cannot change the timed result.
#ifdef BEANS_ARC_STATS
static unsigned long long arc_allocations;
static unsigned long long arc_allocated_bytes;
static unsigned long long arc_retain_calls;
static unsigned long long arc_release_calls;
static unsigned long long arc_release_nodes;
static unsigned long long arc_freed_shells;
static unsigned long long arc_possible_roots;
static unsigned long long arc_collections;
static unsigned long long arc_cycle_objects;
#define ARC_ADD(name, value) \
    __atomic_add_fetch(&(name), (unsigned long long)(value), __ATOMIC_RELAXED)
static void arc_report(void) {
    fprintf(stderr,
            "beans arc stats: allocations=%llu allocated_bytes=%llu "
            "retains=%llu releases=%llu release_nodes=%llu frees=%llu "
            "possible_roots=%llu collections=%llu cycle_objects=%llu\n",
            (unsigned long long)arc_allocations,
            (unsigned long long)arc_allocated_bytes,
            (unsigned long long)arc_retain_calls,
            (unsigned long long)arc_release_calls,
            (unsigned long long)arc_release_nodes,
            (unsigned long long)arc_freed_shells,
            (unsigned long long)arc_possible_roots,
            (unsigned long long)arc_collections,
            (unsigned long long)arc_cycle_objects);
}
__attribute__((constructor)) static void arc_setup(void) { atexit(arc_report); }
#else
#define ARC_ADD(name, value) ((void)0)
#endif

// deinit, before the children go — outlined and cold: the indirect call must
// stay out of beans_release's hot loop or the optimizer treats every
// iteration as clobbered (that cost 50% on the churn bench). Count up to 1
// and FIN off first: user code in there may retain and release self without
// re-entering death, and death can't run twice (husk and collector paths see
// FIN already gone). Count back to 0 after: the husk filter frees a parked
// shell only when RC_COUNT is 0, so the bump must not outlive the call (it
// leaked a buffered object's shell once).
__attribute__((noinline, cold, preserve_most)) static void beans_do_deinit(
    void* p, BHead* h, long long nrc) {
    if (cc_is_mt()) __atomic_store_n(&h->rc, (nrc + 1) & ~RC_FIN, __ATOMIC_RELAXED);
    else h->rc = (nrc + 1) & ~RC_FIN;
    void** descriptor = *(void***)p;
    void (**methods)(void*) = (void (**)(void*))descriptor;
    __atomic_add_fetch(&beans_in_deinit, 1, __ATOMIC_RELAXED);
    methods[beans_deinit_sel + 1](p);
    __atomic_sub_fetch(&beans_in_deinit, 1, __ATOMIC_RELAXED);
    if (cc_is_mt()) __atomic_store_n(&h->rc, nrc & ~RC_FIN, __ATOMIC_RELAXED);
    else h->rc = nrc & ~RC_FIN;
}

// segregated per-thread freelists over 64KB slabs: one calloc per slab,
// then carve; a free pushes the block on the freeing thread's list. Slabs
// are registered globally so the leaks tool sees every allocation as
// reachable; blocks stranded on a dead worker's freelist sit inside a
// registered slab (wasted until exit, never a leak). BEANS_NO_POOL=1
// routes everything through plain calloc/free so `leaks` can see
// individual beans objects again when hunting a real leak.
#define POOL_CLASSES 64 // pooled sizes 16..1008 bytes; bigger goes to malloc
#define POOL_SLAB (64 << 10)
static _Thread_local void* pool_free[POOL_CLASSES];
static _Thread_local char* pool_cur;
static _Thread_local char* pool_end;
static void** pool_slabs;
static long long pool_slab_len, pool_slab_cap;
static pthread_mutex_t pool_mu = PTHREAD_MUTEX_INITIALIZER;
static int pool_off;
__attribute__((constructor)) static void pool_setup(void) {
    pool_off = getenv("BEANS_NO_POOL") != NULL;
}

void beans_panic(const char* msg, long long line, long long col);
void* beans_alloc(long long size, long long meta) {
    ARC_ADD(arc_allocations, 1);
    ARC_ADD(arc_allocated_bytes, size);
    // allocation is the one safe point: never inside a release cascade,
    // and every stored reference is already counted (a deinit body is the
    // exception — cc_collect itself bails while one runs, so this exact
    // condition stays byte-identical to keep clang's fast-path layout)
    if (cc_pending && !cc_collecting && cc_threads == 0) cc_collect(0);
    size_t total = (16 + (size_t)size + 15) & ~(size_t)15;
    long long cls = (long long)(total >> 4);
    BHead* h;
    if (cls < POOL_CLASSES && !pool_off) {
        if (pool_free[cls]) {
            h = pool_free[cls];
            pool_free[cls] = *(void**)h;
            memset(h, 0, total); // recycled block; callers expect zeroed slots
        } else {
            if (!pool_cur || pool_cur + total > pool_end) {
                pool_cur = calloc(1, POOL_SLAB);
                if (!pool_cur) beans_panic("out of memory", 0, 0);
                pool_end = pool_cur + POOL_SLAB;
                pthread_mutex_lock(&pool_mu);
                if (pool_slab_len == pool_slab_cap) {
                    pool_slab_cap = pool_slab_cap ? pool_slab_cap * 2 : 64;
                    pool_slabs = realloc(pool_slabs,
                                         (size_t)pool_slab_cap * sizeof(void*));
                }
                pool_slabs[pool_slab_len++] = pool_cur;
                pthread_mutex_unlock(&pool_mu);
            }
            h = (BHead*)pool_cur; // virgin slab memory, already zero
            pool_cur += total;
        }
        h->rc = 1 | (cls << RC_CLS_SHIFT);
    } else {
        h = calloc(1, total);
        if (!h) beans_panic("out of memory", 0, 0);
        h->rc = 1;
    }
    h->meta = meta;
    return (char*)h + 16;
}

void beans_retain(void* p) {
    if (!p) return;
    ARC_ADD(arc_retain_calls, 1);
    BHead* h = head_of(p);
    if (cc_is_mt()) {
        if (__atomic_load_n(&h->rc, __ATOMIC_RELAXED) >= BEANS_IMMORTAL) return;
        __atomic_add_fetch(&h->rc, 1, __ATOMIC_RELAXED);
    } else {
        if (h->rc >= BEANS_IMMORTAL) return;
        h->rc += 1;
    }
}

void beans_release(void* p);

typedef struct {
    long long* data;
    long long len, cap;
    // Generic lists keep the original data/len/cap prefix because generated
    // code reads those hot fields directly. Wide inline elements use stride
    // bytes and ptr_mask marks owned ARC pointers inside each element.
    long long stride, ptr_mask;
} BList;
typedef struct {
    long long* data;
    long long len, cap;
    long long stride, ptr_mask, cycle_mask;
} BArena;
typedef struct {
    long long* data; // key,value interleaved — len stays at
    long long len, cap; // offset 8: map.len() is a direct field load in IR
    // open-addressed index over data: (hash hi32 << 32) | (pos+2), 0 empty,
    // 1 tombstone. NULL until the map outgrows a linear scan.
    unsigned long long* idx;
    long long icap, tombs;
    // OrderedMap removal leaves stable holes. Plain Map swap-removes and keeps
    // used == len. deadbits NULL means there are no holes.
    long long used;
    unsigned long long* deadbits;
    long long ordered;
    // Wide values live in a parallel flat buffer. Keeping data interleaved for
    // the common one-slot case preserves its hot lookup/update layout.
    void* wide_values;
    long long value_stride;
    long long value_ptr_mask;
    long long value_cycle_mask;
} BMap;
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t can_send, can_recv;
    long long* q;
    long long head, count, cap;
    int closed;
    long long stride, ptr_mask;
} BChan;
typedef struct {
    pthread_mutex_t m;
    long long inner;
} BMutex;

// one shape-walker for destruction and all collector phases
static void cc_walk(void* p, long long meta, void (*fn)(void*, void*), void* ctx) {
    long long kind = meta & 7;
    long long extra = (meta & CC_SHAPE) >> 3;
    if (kind == 1) { // fixed: marked 8-byte slots
        for (int i = 0; i < 58 && (extra >> i); i++) {
            if ((extra >> i) & 1) {
                void* c = *(void**)((char*)p + 8 * i);
                if (c) fn(c, ctx);
            }
        }
    } else if (kind == 2) {
        BList* l = p;
        if (extra & 1) {
            long long stride = l->stride ? l->stride : 8;
            for (long long i = 0; i < l->len; i++) {
                char* element = (char*)l->data + i * stride;
                for (int slot = 0; slot < 58 && (l->ptr_mask >> slot); slot++) {
                    if (!((l->ptr_mask >> slot) & 1)) continue;
                    void* child = *(void**)(element + slot * 8);
                    if (child) fn(child, ctx);
                }
            }
        }
    } else if (kind == 3) {
        BMap* m = p;
        for (long long i = 0; i < m->used; i++) { // holes are zeroed: null-skip
            if ((extra & 1) && m->data[i * 2]) fn((void*)m->data[i * 2], ctx);
            if (!(extra & 2)) continue;
            if (!m->wide_values) {
                if (m->data[i * 2 + 1]) fn((void*)m->data[i * 2 + 1], ctx);
                continue;
            }
            char* value = (char*)m->wide_values + i * m->value_stride;
            for (int slot = 0; slot < 58 && (m->value_ptr_mask >> slot); slot++) {
                if (!((m->value_ptr_mask >> slot) & 1)) continue;
                void* child = *(void**)(value + slot * 8);
                if (child) fn(child, ctx);
            }
        }
    } else if (kind == 4) {
        BChan* c = p;
        if (extra & 1) {
            for (long long i = 0; i < c->count; i++) {
                char* value = (char*)c->q +
                              ((c->head + i) % c->cap) * c->stride;
                for (int slot = 0; slot < 58 && (c->ptr_mask >> slot); slot++) {
                    if (!((c->ptr_mask >> slot) & 1)) continue;
                    void* child = *(void**)(value + slot * 8);
                    if (child) fn(child, ctx);
                }
            }
        }
    } else if (kind == 5) {
        BMutex* mu = p;
        if ((extra & 1) && mu->inner) fn((void*)mu->inner, ctx);
    } else if (kind == 7) {
        BArena* arena = p;
        if (extra & 1) {
            long long stride = arena->stride ? arena->stride : 8;
            for (long long i = 0; i < arena->len; i++) {
                char* value = (char*)arena->data + i * stride;
                for (int slot = 0; slot < 58 && (arena->ptr_mask >> slot); slot++) {
                    if (!((arena->ptr_mask >> slot) & 1)) continue;
                    void* child = *(void**)(value + slot * 8);
                    if (child) fn(child, ctx);
                }
            }
        }
    }
}

typedef struct {
    long long fd;
    long long closed;
} BFile;
typedef struct {
    char* p;
    long long len;
    long long fd;
    long long writable;
    long long closed;
} BMMap;

typedef struct {
    long long strong;
    long long weak;
    long long value;
    long long value_ptr;
} BSharedCtrl;
typedef struct { BSharedCtrl* ctrl; } BSharedHandle;

// kind 6, extra 2 = strong handle, extra 3 = weak handle. The control block
// owns exactly one payload reference while any strong handle exists. Return
// that child when the final strong shell dies so beans_release can keep its
// existing iterative cascade.
static void* shared_shell_drop(void* p, long long extra) {
    BSharedCtrl* ctrl = ((BSharedHandle*)p)->ctrl;
    if (extra & 1) {
        if (__atomic_fetch_sub(&ctrl->weak, 1, __ATOMIC_ACQ_REL) == 1) free(ctrl);
        return NULL;
    }
    void* child = NULL;
    if (__atomic_fetch_sub(&ctrl->strong, 1, __ATOMIC_ACQ_REL) == 1) {
        if (ctrl->value_ptr) child = (void*)ctrl->value;
        ctrl->value = 0;
        if (__atomic_fetch_sub(&ctrl->weak, 1, __ATOMIC_ACQ_REL) == 1) free(ctrl);
    }
    return child;
}

// free the box and its side allocations WITHOUT touching child refs
static void* cc_free_shell(void* p, long long meta) {
    ARC_ADD(arc_freed_shells, 1);
    long long kind = meta & 7;
    long long extra = (meta & CC_SHAPE) >> 3;
    void* deferred_child = NULL;
    if (kind == 2) free(((BList*)p)->data);
    else if (kind == 7) free(((BArena*)p)->data);
    else if (kind == 3) {
        free(((BMap*)p)->data);
        free(((BMap*)p)->wide_values);
        free(((BMap*)p)->idx);
        free(((BMap*)p)->deadbits);
    } else if (kind == 4) {
        BChan* c = p;
        pthread_cond_destroy(&c->can_send);
        pthread_cond_destroy(&c->can_recv);
        pthread_mutex_destroy(&c->m);
        free(c->q);
    } else if (kind == 5) {
        pthread_mutex_destroy(&((BMutex*)p)->m);
    } else if (kind == 6 && (extra & 2)) {
        deferred_child = shared_shell_drop(p, extra);
    } else if (kind == 6) { // OS resource — dropping the last ref is the safety
        // close whatever is still open at the OS level, whether the handle was
        // never closed or its close was deferred while threads ran (fd/p left
        // valid, the logical `closed` flag already set). The last ref is gone,
        // so no thread can be mid-op here — releasing now is safe.
        if ((meta & CC_SHAPE) >> 3 & 1) { // shape bit 0: 0 = file, 1 = mmap
            BMMap* m = p;
            if (m->p) munmap(m->p, (size_t)m->len);
            if (m->fd >= 0) close((int)m->fd);
        } else {
            BFile* f = p; // net; close() / f.close() is the real API
            if (f->fd >= 0) close((int)f->fd);
        }
    }
    BHead* h = head_of(p);
    long long cls = (h->rc >> RC_CLS_SHIFT) & RC_CLS_MAX;
    if (cls) {
        *(void**)h = pool_free[cls];
        pool_free[cls] = h;
    } else {
        free(h);
    }
    return deferred_child;
}

// explicit work stack, shared by release cascades and all collector phases
typedef struct {
    void** v;
    long long len, cap;
    void** local;
} CCStack;
static void cc_push(CCStack* s, void* p) {
    if (s->len == s->cap) {
        long long next = s->cap ? s->cap * 2 : 4096;
        void** grown = malloc((size_t)next * sizeof(void*));
        if (s->len) memcpy(grown, s->v, (size_t)s->len * sizeof(void*));
        if (s->v != s->local) free(s->v);
        s->v = grown;
        s->cap = next;
    }
    s->v[s->len++] = p;
}
static void cc_visit_push(void* c, void* ctx) {
    BHead* h = head_of(c);
    long long rc = cc_is_mt() ? __atomic_load_n(&h->rc, __ATOMIC_RELAXED) : h->rc;
    if (rc >= BEANS_IMMORTAL) return;
    cc_push(ctx, c);
}

// Release cascades overwhelmingly walk fixed class objects. Keep this path
// direct: the collector still uses the generic callback walker, but ordinary
// ARC death should not pay an indirect call for every child.
static inline void cc_release_children(void* p, long long meta, CCStack* st) {
    long long kind = meta & 7;
    long long extra = (meta & CC_SHAPE) >> 3;
    if (kind != 1) {
        cc_walk(p, meta, cc_visit_push, st);
        return;
    }
    for (int i = 0; i < 58 && (extra >> i); i++) {
        if (!((extra >> i) & 1)) continue;
        void* child = *(void**)((char*)p + 8 * i);
        if (!child) continue;
        BHead* h = head_of(child);
        long long rc = cc_is_mt() ? __atomic_load_n(&h->rc, __ATOMIC_RELAXED)
                                  : h->rc;
        if (rc < BEANS_IMMORTAL) cc_push(st, child);
    }
}

// ---- possible-root buffer ----
static void** cc_roots;
static long long cc_len, cc_cap;
static long long cc_threshold = 256;
static pthread_mutex_t cc_mu = PTHREAD_MUTEX_INITIALIZER;

static void cc_possible_root(void* p) {
    BHead* h = head_of(p);
    long long old = __atomic_fetch_or(&h->meta, CC_PURPLE | CC_BUF, __ATOMIC_RELAXED);
    if (old & CC_BUF) return; // already parked
    ARC_ADD(arc_possible_roots, 1);
    // like the counts, the buffer goes unlocked until the first spawn: one
    // thread exists, and after cc_mt flips every park takes the mutex
    if (cc_is_mt()) pthread_mutex_lock(&cc_mu);
    if (cc_len == cc_cap) {
        cc_cap = cc_cap ? cc_cap * 2 : 1024;
        cc_roots = realloc(cc_roots, (size_t)cc_cap * sizeof(void*));
    }
    cc_roots[cc_len++] = p;
    if (cc_len >= cc_threshold) cc_pending = 1;
    if (cc_is_mt()) pthread_mutex_unlock(&cc_mu);
}

// iterative: a dropped million-node chain pushes children on an explicit
// stack instead of recursing the C stack. The stack stays empty (no malloc)
// unless a death actually cascades.
void beans_release(void* p) {
    if (!p) return;
    ARC_ADD(arc_release_calls, 1);
    void* local[64];
    CCStack st = {local, 0, 64, local};
    void* cur = p;
    for (;;) {
        ARC_ADD(arc_release_nodes, 1);
        BHead* h = head_of(cur);
        long long rc0 = cc_is_mt() ? __atomic_load_n(&h->rc, __ATOMIC_RELAXED) : h->rc;
        if (rc0 < BEANS_IMMORTAL) {
            long long nrc = cc_is_mt() ? __atomic_sub_fetch(&h->rc, 1, __ATOMIC_ACQ_REL)
                                  : (h->rc -= 1);
            if (RC_COUNT(nrc) == 0) {
                long long meta = h->meta;
                // FIN is only ever set on class objects, so it alone decides
                if (__builtin_expect(nrc & RC_FIN, 0)) {
                    beans_do_deinit(cur, h, nrc);
                    meta = h->meta; // colors can move while user code runs
                }
                cc_release_children(cur, meta, &st);
                if (meta & CC_BUF) {
                    // parked — the buffer still points here, so the collector
                    // frees the shell later; mark black: this is a dead husk
                    __atomic_and_fetch(&h->meta, ~CC_COLOR, __ATOMIC_RELAXED);
                } else {
                    void* child = cc_free_shell(cur, meta);
                    if (child) cc_push(&st, child);
                }
            } else {
                // could this shape sit on a cycle? leaves, pointer-free
                // containers, and objects with an empty pointer mask never can
                // — a cycle member needs an outgoing edge — which keeps
                // int-field churn off the buffer
                long long meta = h->meta;
                long long kind = meta & 7;
                int cyclic = (kind == 1 && ((meta & CC_SHAPE) >> 3) != 0) ||
                             (kind == 3 && (meta & (3LL << 3))) ||
                             ((kind == 2 || kind == 4 || kind == 5 || kind == 7) &&
                              (meta & (1LL << 3)));
                if (cyclic) cc_possible_root(cur);
            }
        }
        if (!st.len) break;
        cur = st.v[--st.len];
    }
    if (st.v != local) free(st.v);
}

// Checked Beans treats Box/Arena handles as move-only. Wide values keep their
// real byte layout; pointer masks let the common collector walk their nested
// ARC fields without a type-specific destructor.
static void release_masked_value(void* value, long long ptr_mask) {
    for (int slot = 58; slot-- > 0;) {
        if (!((ptr_mask >> slot) & 1)) continue;
        void* child = *(void**)((char*)value + slot * 8);
        if (child) beans_release(child);
    }
}
void* beans_raw_alloc(long long count, long long size, long long line, long long col) {
    if (count < 0) beans_panic("negative raw allocation count", line, col);
    if (size <= 0 || count > (1LL << 58) / size)
        beans_panic("raw allocation too large", line, col);
    void* p = calloc((size_t)count, (size_t)size);
    if (!p && count) beans_panic("out of memory", line, col);
    return p;
}
void beans_raw_free(void* p) { free(p); }
void beans_raw_copy(void* destination, void* source, long long count, long long size,
                    long long line, long long col) {
    if (count < 0) beans_panic("negative raw copy count", line, col);
    if (size <= 0 || count > (1LL << 58) / size)
        beans_panic("raw copy too large", line, col);
    if (count && (!destination || !source))
        beans_panic("null raw pointer copy", line, col);
    memmove(destination, source, (size_t)count * (size_t)size);
}
void beans_raw_zero(void* destination, long long count, long long size,
                    long long line, long long col) {
    if (count < 0) beans_panic("negative raw zero count", line, col);
    if (size <= 0 || count > (1LL << 58) / size)
        beans_panic("raw zero too large", line, col);
    if (count && !destination) beans_panic("null raw pointer zero", line, col);
    memset(destination, 0, (size_t)count * (size_t)size);
}

void* beans_box_new(long long value, long long value_ptr) {
    long long* box = beans_alloc(8, 1 | (value_ptr << 3));
    box[0] = value;
    return box;
}
long long beans_box_get(void* p) { return ((long long*)p)[0]; }
void beans_box_set(void* p, long long value) {
    long long* box = p;
    if (((head_of(p)->meta & CC_SHAPE) >> 3) & 1) {
        void* old = (void*)box[0];
        if (old) beans_release(old);
    }
    box[0] = value;
}
void* beans_box_new_typed(void* value, long long size, long long ptr_mask,
                          long long cycle_mask) {
    if (size <= 0 || size > (1LL << 30))
        beans_panic("invalid box value size", 0, 0);
    void* box = beans_alloc(size, 1 | (ptr_mask << 3));
    memcpy(box, value, (size_t)size);
    if (cycle_mask) cc_possible_root(box);
    return box;
}
void beans_box_get_typed(void* box, void* out, long long size) {
    memcpy(out, box, (size_t)size);
}
void beans_box_set_typed(void* box, void* value, long long size,
                         long long ptr_mask, long long cycle_mask) {
    release_masked_value(box, ptr_mask);
    memcpy(box, value, (size_t)size);
    if (cycle_mask) cc_possible_root(box);
}

void* beans_shared_new(long long value, long long value_ptr) {
    BSharedCtrl* ctrl = calloc(1, sizeof(BSharedCtrl));
    if (!ctrl) beans_panic("out of memory", 0, 0);
    ctrl->strong = 1;
    ctrl->weak = 1; // implicit weak held until strong reaches zero
    ctrl->value = value;
    ctrl->value_ptr = value_ptr;
    BSharedHandle* handle = beans_alloc(sizeof(BSharedHandle), 6 | (2LL << 3));
    handle->ctrl = ctrl;
    return handle;
}
long long beans_shared_get(void* p) {
    return ((BSharedHandle*)p)->ctrl->value;
}
void* beans_shared_new_typed(void* value, long long size, long long ptr_mask) {
    if (size <= 0 || size > (1LL << 30))
        beans_panic("invalid shared value size", 0, 0);
    void* payload = beans_alloc(size, 1 | (ptr_mask << 3));
    memcpy(payload, value, (size_t)size);
    return beans_shared_new((long long)payload, 1);
}
void beans_shared_get_typed(void* p, void* out, long long size) {
    void* payload = (void*)((BSharedHandle*)p)->ctrl->value;
    memcpy(out, payload, (size_t)size);
}
void* beans_shared_downgrade(void* p) {
    BSharedCtrl* ctrl = ((BSharedHandle*)p)->ctrl;
    __atomic_add_fetch(&ctrl->weak, 1, __ATOMIC_RELAXED);
    BSharedHandle* weak = beans_alloc(sizeof(BSharedHandle), 6 | (3LL << 3));
    weak->ctrl = ctrl;
    return weak;
}
void* beans_weak_upgrade(void* p) {
    BSharedCtrl* ctrl = ((BSharedHandle*)p)->ctrl;
    long long strong = __atomic_load_n(&ctrl->strong, __ATOMIC_ACQUIRE);
    while (strong > 0) {
        if (__atomic_compare_exchange_n(&ctrl->strong, &strong, strong + 1, 1,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            BSharedHandle* handle =
                beans_alloc(sizeof(BSharedHandle), 6 | (2LL << 3));
            handle->ctrl = ctrl;
            return handle;
        }
    }
    return NULL;
}
long long beans_weak_expired(void* p) {
    BSharedCtrl* ctrl = ((BSharedHandle*)p)->ctrl;
    return __atomic_load_n(&ctrl->strong, __ATOMIC_ACQUIRE) == 0;
}

void* beans_arena_new(long long capacity, long long elem_ptr,
                      long long line, long long col) {
    if (capacity < 0) {
        char msg[96];
        snprintf(msg, sizeof msg, "negative arena capacity %lld", capacity);
        beans_panic(msg, line, col);
    }
    if (capacity > (1LL << 58)) beans_panic("arena capacity too large", line, col);
    BArena* arena = beans_alloc(sizeof(BArena), 7 | (elem_ptr << 3));
    arena->stride = 8;
    arena->ptr_mask = elem_ptr;
    if (capacity) {
        arena->data = calloc((size_t)capacity, sizeof(long long));
        if (!arena->data) beans_panic("out of memory", line, col);
    }
    arena->cap = capacity;
    return arena;
}
void* beans_arena_new_typed(long long capacity, long long stride,
                            long long ptr_mask, long long cycle_mask,
                            long long line, long long col) {
    if (capacity < 0) {
        char msg[96];
        snprintf(msg, sizeof msg, "negative arena capacity %lld", capacity);
        beans_panic(msg, line, col);
    }
    if (stride <= 0 || stride > (1LL << 30))
        beans_panic("invalid arena element size", line, col);
    if (capacity > (1LL << 58) / stride)
        beans_panic("arena capacity too large", line, col);
    BArena* arena = beans_alloc(sizeof(BArena), 7 | ((ptr_mask != 0) << 3));
    arena->stride = stride;
    arena->ptr_mask = ptr_mask;
    arena->cycle_mask = cycle_mask;
    if (capacity) {
        arena->data = calloc((size_t)capacity, (size_t)stride);
        if (!arena->data) beans_panic("out of memory", line, col);
    }
    arena->cap = capacity;
    return arena;
}
long long beans_arena_put(void* p, long long value) {
    BArena* arena = p;
    if (arena->len == arena->cap) {
        long long next = arena->cap ? arena->cap * 2 : 8;
        long long* data = realloc(arena->data, (size_t)next * sizeof(long long));
        if (!data) beans_panic("out of memory", 0, 0);
        arena->data = data;
        arena->cap = next;
    }
    long long handle = arena->len++;
    arena->data[handle] = value;
    return handle;
}
long long beans_arena_put_typed(void* p, void* value) {
    BArena* arena = p;
    if (arena->len == arena->cap) {
        long long next = arena->cap ? arena->cap * 2 : 8;
        if (next > (1LL << 58) / arena->stride)
            beans_panic("arena capacity too large", 0, 0);
        void* data = realloc(arena->data, (size_t)next * (size_t)arena->stride);
        if (!data) beans_panic("out of memory", 0, 0);
        arena->data = data;
        arena->cap = next;
    }
    long long handle = arena->len++;
    memcpy((char*)arena->data + handle * arena->stride, value,
           (size_t)arena->stride);
    if (arena->cycle_mask) cc_possible_root(arena);
    return handle;
}
long long beans_arena_get(void* p, long long handle, long long* ok) {
    BArena* arena = p;
    if (handle < 0 || handle >= arena->len) {
        *ok = 0;
        return 0;
    }
    *ok = 1;
    return arena->data[handle];
}
long long beans_arena_get_typed(void* p, long long handle, void* out) {
    BArena* arena = p;
    if (handle < 0 || handle >= arena->len) return 0;
    memcpy(out, (char*)arena->data + handle * arena->stride,
           (size_t)arena->stride);
    return 1;
}
long long beans_arena_at(void* p, long long handle, long long line, long long col) {
    BArena* arena = p;
    if (handle < 0 || handle >= arena->len) {
        char msg[112];
        snprintf(msg, sizeof msg, "arena handle %lld out of range (len %lld)",
                 handle, arena->len);
        beans_panic(msg, line, col);
    }
    return arena->data[handle];
}
void beans_arena_at_typed(void* p, long long handle, void* out,
                          long long line, long long col) {
    BArena* arena = p;
    if (handle < 0 || handle >= arena->len) {
        char msg[112];
        snprintf(msg, sizeof msg, "arena handle %lld out of range (len %lld)",
                 handle, arena->len);
        beans_panic(msg, line, col);
    }
    memcpy(out, (char*)arena->data + handle * arena->stride,
           (size_t)arena->stride);
}
long long beans_arena_len(void* p) { return ((BArena*)p)->len; }
void beans_arena_clear(void* p) {
    BArena* arena = p;
    if (arena->ptr_mask) {
        long long stride = arena->stride ? arena->stride : 8;
        for (long long i = 0; i < arena->len; i++) {
            release_masked_value((char*)arena->data + i * stride,
                                 arena->ptr_mask);
        }
    }
    arena->len = 0;
}

// ---- the collector (single mutator: us) ----

static void cc_visit_dec_push(void* c, void* ctx) {
    BHead* h = head_of(c);
    if (h->rc >= BEANS_IMMORTAL) return;
    h->rc -= 1; // trial deletion: one decrement per internal edge
    cc_push(ctx, c);
}
static void cc_mark_gray(void* root, CCStack* st) {
    cc_push(st, root);
    while (st->len) {
        void* p = st->v[--st->len];
        BHead* h = head_of(p);
        if (cc_color(h) == CC_GRAY) continue;
        cc_set_color(h, CC_GRAY);
        cc_walk(p, h->meta, cc_visit_dec_push, st);
    }
}

static void cc_visit_inc_push(void* c, void* ctx) {
    BHead* h = head_of(c);
    if (h->rc >= BEANS_IMMORTAL) return;
    h->rc += 1; // undo the trial deletion along this edge
    if (cc_color(h) != CC_BLACK) {
        cc_set_color(h, CC_BLACK);
        cc_push(ctx, c);
    }
}
static void cc_scan_black(void* root, CCStack* st) {
    cc_set_color(head_of(root), CC_BLACK);
    cc_push(st, root);
    while (st->len) {
        void* p = st->v[--st->len];
        cc_walk(p, head_of(p)->meta, cc_visit_inc_push, st);
    }
}

static void cc_scan(void* root, CCStack* st, CCStack* aux) {
    cc_push(st, root);
    while (st->len) {
        void* p = st->v[--st->len];
        BHead* h = head_of(p);
        if (cc_color(h) != CC_GRAY) continue;
        if (RC_COUNT(h->rc) > 0) {
            cc_scan_black(p, aux); // externally referenced — restore it all
        } else {
            cc_set_color(h, CC_WHITE);
            cc_walk(p, h->meta, cc_visit_push, st);
        }
    }
}

static void cc_collect_white(void* root, CCStack* st, CCStack* dead) {
    cc_push(st, root);
    while (st->len) {
        void* p = st->v[--st->len];
        BHead* h = head_of(p);
        if (cc_color(h) != CC_WHITE || (h->meta & CC_BUF)) continue;
        cc_set_color(h, CC_BLACK); // visited; prevents duplicate frees
        cc_walk(p, h->meta, cc_visit_push, st);
        cc_push(dead, p);
    }
}

static long long cc_walk_min = 256; // adaptive gate for trial deletion

static void cc_collect(int force) {
    if (cc_collecting) return;
    // a deinit body is user code running mid-cascade: its allocations must
    // not start a collection — a mid-destroy object must never be walked.
    // cc_pending stays set, so the next allocation after the cascade retries.
    if (__atomic_load_n(&beans_in_deinit, __ATOMIC_RELAXED)) return;
    ARC_ADD(arc_collections, 1);
    cc_collecting = 1;
    if (cc_is_mt()) pthread_mutex_lock(&cc_mu);

    // keep only live purple candidates; zombies (released while parked)
    // just need their shells freed, everything else drops out
    long long n = 0;
    for (long long i = 0; i < cc_len; i++) {
        void* p = cc_roots[i];
        BHead* h = head_of(p);
        if (cc_color(h) == CC_PURPLE && RC_COUNT(h->rc) > 0) {
            cc_roots[n++] = p;
        } else {
            __atomic_and_fetch(&h->meta, ~CC_BUF, __ATOMIC_RELAXED);
            if (RC_COUNT(h->rc) == 0) {
                void* child = cc_free_shell(p, h->meta);
                if (child) beans_release(child);
            }
        }
    }
    cc_len = n;

    // The filter above is the cheap half and just ran: husk shells free at
    // a steady cadence, so they can never pile past survivors + 256. Trial
    // deletion is the expensive half — it walks everything reachable from
    // the survivors — so it only runs once enough purple candidates pile
    // up, and it backs off hard when a walk frees nothing: a live tree
    // that gets borrow-pinned on every visit must not be re-walked every
    // few hundred allocations (that made binary-trees 10x slower than Go).
    if (cc_len && (force || cc_len >= cc_walk_min)) {
        CCStack st = {0, 0, 0}, aux = {0, 0, 0}, dead = {0, 0, 0};
        for (long long i = 0; i < cc_len; i++) cc_mark_gray(cc_roots[i], &st);
        for (long long i = 0; i < cc_len; i++) cc_scan(cc_roots[i], &st, &aux);
        for (long long i = 0; i < cc_len; i++) {
            BHead* h = head_of(cc_roots[i]);
            __atomic_and_fetch(&h->meta, ~CC_BUF, __ATOMIC_RELAXED);
            cc_collect_white(cc_roots[i], &st, &dead);
        }
        cc_len = 0;
        ARC_ADD(arc_cycle_objects, dead.len);
        // nothing was freed while walking, so no stale pointer was ever
        // read; now the whole white set goes at once
        for (long long i = 0; i < dead.len; i++) {
            void* child = cc_free_shell(dead.v[i], head_of(dead.v[i])->meta);
            if (child) beans_release(child);
        }
        cc_walk_min = dead.len ? 256
                               : (cc_walk_min * 4 > (1LL << 18) ? (1LL << 18)
                                                                : cc_walk_min * 4);
        free(st.v);
        free(aux.v);
        free(dead.v);
    }
    // geometric re-arm: survivors stay parked, so the next filter may scan
    // them again — amortized O(1) per park only if the buffer must grow by
    // its own size first. Husk shells thus wait at most 2·survivors + 256
    // parks, which keeps RSS flat in practice (husk-heavy programs have few
    // long-lived survivors)
    cc_threshold = cc_len * 2 + 256;
    cc_pending = 0;
    if (cc_is_mt()) pthread_mutex_unlock(&cc_mu);
    cc_collecting = 0;
}

static void cc_at_exit(void) {
    if (cc_threads == 0) cc_collect(1); // forced: leaks must see 0 at exit
}
__attribute__((constructor)) static void cc_setup(void) { atexit(cc_at_exit); }

void beans_panic(const char* msg, long long line, long long col) {
    fflush(stdout); // ordered output: buffered stdout before the stderr panic
    fprintf(stderr, "runtime panic at %lld:%lld: %s\n", line, col, msg);
    exit(3);
}
// list bounds — message matches the interpreter's, index and length included
void beans_panic_index(long long i, long long len, long long has_len,
                       long long line, long long col) {
    char b[96];
    if (has_len) snprintf(b, sizeof b, "list index %lld out of range (len %lld)", i, len);
    else snprintf(b, sizeof b, "list index %lld out of range", i);
    beans_panic(b, line, col);
}
void beans_panic_array_index(long long i, long long len,
                             long long line, long long col) {
    char b[96];
    snprintf(b, sizeof b, "array index %lld out of range (len %lld)", i, len);
    beans_panic(b, line, col);
}
void beans_panic_slice_index(long long i, long long len,
                             long long line, long long col) {
    char b[96];
    snprintf(b, sizeof b, "slice index %lld out of range (len %lld)", i, len);
    beans_panic(b, line, col);
}

// ---- strings (leaf allocations) ----
// a string's byte length lives in its meta shape bits (kind 0 uses none of
// bits 3-60), so len is O(1) and never strlen. Read through beans_slen —
// masking with CC_SHAPE is mandatory, colors share the word.
static long long beans_slen(char* s) { return (head_of(s)->meta & CC_SHAPE) >> 3; }
static char* str_make(const char* p, long long n);
static char* rc_strdup(const char* s) {
    size_t n = strlen(s);
    char* r = beans_alloc((long long)n + 1, (long long)n << 3);
    memcpy(r, s, n + 1);
    return r;
}
// hand-rolled digits: snprintf("%lld") was ~1/3 of the string-build loop in
// the strings bench (vfprintf machinery per call); this matches its output
// byte for byte
static long long uint_digits(unsigned long long v) {
    long long n = 1;
    while (v >= 10) { v /= 10; n += 1; }
    return n;
}
static char* write_uint(char* out, unsigned long long v) {
    long long n = uint_digits(v);
    char* end = out + n;
    char* p = end;
    do {
        *--p = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    return end;
}
static long long int_digits(long long v) {
    unsigned long long u =
        v < 0 ? (unsigned long long)-(v + 1) + 1 : (unsigned long long)v;
    return uint_digits(u) + (v < 0);
}
static char* write_int(char* out, long long v) {
    unsigned long long u =
        v < 0 ? (unsigned long long)-(v + 1) + 1 : (unsigned long long)v;
    if (v < 0) *out++ = '-';
    return write_uint(out, u);
}
char* beans_from_int(long long v) {
    char b[24];
    char* e = b + sizeof b;
    char* p = e;
    unsigned long long u =
        v < 0 ? (unsigned long long)-(v + 1) + 1 : (unsigned long long)v;
    do {
        *--p = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (v < 0) *--p = '-';
    return str_make(p, e - p);
}
char* beans_from_uint(unsigned long long v) {
    char b[24];
    char* e = b + sizeof b;
    char* p = e;
    do {
        *--p = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    return str_make(p, e - p);
}
char* beans_from_float(double v) {
    char b[48];
    snprintf(b, sizeof b, "%.10g", v);
    return rc_strdup(b);
}
char* beans_from_bool(int v) { return rc_strdup(v ? "true" : "false"); }
char* beans_concat(char* a, char* b) {
    size_t la = (size_t)beans_slen(a), lb = (size_t)beans_slen(b);
    char* r = beans_alloc((long long)(la + lb + 1), (long long)(la + lb) << 3);
    memcpy(r, a, la);
    memcpy(r + la, b, lb + 1);
    return r;
}
char* beans_interpolate(long long n, ...) {
    va_list ap;
    va_start(ap, n);
    long long total = 0;
    for (long long i = 0; i < n; i++) {
        long long kind = va_arg(ap, long long);
        if (kind == 0) {
            total += beans_slen(va_arg(ap, char*));
        } else if (kind == 1) {
            total += int_digits(va_arg(ap, long long));
        } else if (kind == 2) {
            total += uint_digits(va_arg(ap, unsigned long long));
        } else if (kind == 3) {
            char b[48];
            total += snprintf(b, sizeof b, "%.10g", va_arg(ap, double));
        } else {
            total += va_arg(ap, int) ? 4 : 5;
        }
    }
    va_end(ap);
    char* r = beans_alloc(total + 1, total << 3);
    char* w = r;
    va_start(ap, n);
    for (long long i = 0; i < n; i++) {
        long long kind = va_arg(ap, long long);
        if (kind == 0) {
            char* part = va_arg(ap, char*);
            long long len = beans_slen(part);
            memcpy(w, part, (size_t)len);
            w += len;
        } else if (kind == 1) {
            w = write_int(w, va_arg(ap, long long));
        } else if (kind == 2) {
            w = write_uint(w, va_arg(ap, unsigned long long));
        } else if (kind == 3) {
            char b[48];
            int len = snprintf(b, sizeof b, "%.10g", va_arg(ap, double));
            memcpy(w, b, (size_t)len);
            w += len;
        } else {
            int value = va_arg(ap, int);
            const char* text = value ? "true" : "false";
            long long len = value ? 4 : 5;
            memcpy(w, text, (size_t)len);
            w += len;
        }
    }
    va_end(ap);
    return r;
}
// strings carry their byte length and may legally hold NUL (\0 escapes,
// File.read) — every consumer here is length-based; C-string fns like fputs,
// strcmp, and strstr would silently stop at the first NUL and diverge from
// the interpreter
static char* str_make(const char* p, long long n);
void beans_println(char* s) {
    fwrite(s, 1, (size_t)beans_slen(s), stdout);
    fputc('\n', stdout);
}
void beans_print(char* s) { fwrite(s, 1, (size_t)beans_slen(s), stdout); }
// std::string semantics: bytes compare unsigned over the shorter length,
// ties break on length
int beans_str_cmp(char* a, char* b) {
    long long la = beans_slen(a), lb = beans_slen(b);
    long long n = la < lb ? la : lb;
    int c = n ? memcmp(a, b, (size_t)n) : 0;
    if (c) return c;
    return la < lb ? -1 : la > lb ? 1 : 0;
}
long long beans_str_len(char* s) { return beans_slen(s); }
char* beans_str_last(char* s, long long n) {
    long long len = beans_slen(s);
    if (n < 0) n = 0;
    if (n > len) n = len;
    return str_make(s + (len - n), n);
}
// leftmost match: memchr for the first byte (SIMD in libc), memcmp for the
// tail. memcmp-at-every-offset made contains/replace/split the hot spot of
// the strings bench, 2x behind Go's bytealg search.
static long long str_search(const char* s, long long n, const char* sub,
                            long long m, long long from) {
    if (m > n - from) return -1;
    if (m == 0) return from;
    const char* end = s + n;
    const char* p = s + from;
    for (;;) {
        long long room = (end - p) - (m - 1);
        if (room <= 0) return -1;
        const char* hit = memchr(p, sub[0], (size_t)room);
        if (!hit) return -1;
        if (m == 1 || memcmp(hit + 1, sub + 1, (size_t)(m - 1)) == 0) {
            return hit - s;
        }
        p = hit + 1;
    }
}
long long beans_str_contains(char* s, char* sub) {
    long long n = beans_slen(s), m = beans_slen(sub);
    if (m == 0) return 1;
    return str_search(s, n, sub, m, 0) >= 0;
}

// a ready-made Error box: [vtable null][-1][msg][kind] — the exact 32-byte
// layout codegen's make_error builds; meta 97 marks slots 2 and 3 as pointers
static void* mk_error(const char* msg, const char* kind) {
    long long* e = beans_alloc(32, 97);
    e[1] = -1;
    e[2] = (long long)rc_strdup(msg);
    e[3] = (long long)rc_strdup(kind);
    return e;
}
// like mk_error, but msg is already an rc string carrying its exact byte
// length — user text can hold NUL and must not pass through strlen
static void* mk_error_own(char* msg_rc, const char* kind) {
    long long* e = beans_alloc(32, 97);
    e[1] = -1;
    e[2] = (long long)msg_rc;
    e[3] = (long long)rc_strdup(kind);
    return e;
}

// fallible-builtin ABI: 16 bytes so C and IR both return it in registers.
// err null = ok(val); err set = a ready Error object the caller boxes.
typedef struct {
    long long val;
    void* err;
} BRes;

static BRes parse_fail(const char* s, const char* what) {
    // s is the beans receiver string — splice it by its stored length so an
    // embedded NUL keeps the message byte-identical to the interpreter's
    const char* p1 = "can't read '";
    const char* p2 = "' as ";
    long long ls = beans_slen((char*)s);
    size_t l1 = strlen(p1), l2 = strlen(p2), lw = strlen(what);
    long long total = (long long)l1 + ls + (long long)l2 + (long long)lw;
    char* m = beans_alloc(total + 1, total << 3);
    char* w = m;
    memcpy(w, p1, l1);
    w += l1;
    memcpy(w, s, (size_t)ls);
    w += ls;
    memcpy(w, p2, l2);
    w += l2;
    memcpy(w, what, lw);
    return (BRes){0, mk_error_own(m, "")};
}

BRes beans_str_to_int(char* s) {
    char* end = NULL;
    long long v = strtoll(s, &end, 10);
    if (end == s || *end != '\0') return parse_fail(s, "int");
    return (BRes){v, NULL};
}

BRes beans_str_to_float(char* s) {
    char* end = NULL;
    double d = strtod(s, &end);
    if (end == s || *end != '\0') return parse_fail(s, "float");
    BRes r;
    r.err = NULL;
    memcpy(&r.val, &d, 8);
    return r;
}

// Option-shaped ABI: has 0 = none
typedef struct {
    long long val;
    long long has;
} BOpt;

// explicit-length string maker; the terminator byte is already zero
// because every allocation path hands back zeroed memory
static char* str_make(const char* p, long long n) {
    char* r = beans_alloc(n + 1, n << 3);
    memcpy(r, p, (size_t)n);
    return r;
}

long long beans_str_is_empty(char* s) { return beans_slen(s) == 0; }
char* beans_str_first(char* s, long long n) {
    long long len = beans_slen(s);
    if (n < 0) n = 0;
    if (n > len) n = len;
    return str_make(s, n);
}
long long beans_str_starts_with(char* s, char* p) {
    long long pl = beans_slen(p);
    return pl <= beans_slen(s) && memcmp(s, p, (size_t)pl) == 0;
}
long long beans_str_ends_with(char* s, char* p) {
    long long n = beans_slen(s), pl = beans_slen(p);
    return pl <= n && memcmp(s + n - pl, p, (size_t)pl) == 0;
}
// empty needle: find says 0, rfind says len — the C++ side agrees
BOpt beans_str_find(char* s, char* sub) {
    long long n = beans_slen(s), m = beans_slen(sub);
    if (m == 0) return (BOpt){0, 1};
    long long i = str_search(s, n, sub, m, 0);
    if (i >= 0) return (BOpt){i, 1};
    return (BOpt){0, 0};
}
BOpt beans_str_rfind(char* s, char* sub) {
    long long n = beans_slen(s), m = beans_slen(sub);
    if (m == 0) return (BOpt){n, 1};
    for (long long i = n - m; i >= 0; i--) {
        if (memcmp(s + i, sub, (size_t)m) == 0) return (BOpt){i, 1};
    }
    return (BOpt){0, 0};
}
char* beans_str_slice(char* s, long long from, long long to, long long line,
                      long long col) {
    long long n = beans_slen(s);
    if (from < 0 || to < from || to > n) {
        char m[96];
        snprintf(m, sizeof m, "slice %lld..%lld out of range (len %lld)", from, to, n);
        beans_panic(m, line, col);
    }
    return str_make(s + from, to - from);
}
long long beans_str_byte_at(char* s, long long i, long long line, long long col) {
    long long n = beans_slen(s);
    if (i < 0 || i >= n) {
        char m[80];
        snprintf(m, sizeof m, "byte index %lld out of range (len %lld)", i, n);
        beans_panic(m, line, col);
    }
    return (long long)(unsigned char)s[i];
}
long long beans_str_find_byte(char* s, long long byte, long long from,
                              long long line, long long col) {
    long long n = beans_slen(s);
    if (byte < 0 || byte > 255) {
        char m[64];
        snprintf(m, sizeof m, "byte %lld out of range", byte);
        beans_panic(m, line, col);
    }
    if (from < 0 || from > n) {
        char m[96];
        snprintf(m, sizeof m, "find start %lld out of range (len %lld)", from, n);
        beans_panic(m, line, col);
    }
    void* found = memchr(s + from, (unsigned char)byte, (size_t)(n - from));
    return found ? (long long)((char*)found - s) : -1;
}
long long beans_str_range_equals(char* s, long long from, long long to,
                                  char* other, long long line, long long col) {
    long long n = beans_slen(s);
    if (from < 0 || to < from || to > n) {
        char m[96];
        snprintf(m, sizeof m, "range %lld..%lld out of range (len %lld)",
                 from, to, n);
        beans_panic(m, line, col);
    }
    long long length = to - from;
    return length == beans_slen(other) &&
           memcmp(s + from, other, (size_t)length) == 0;
}
long long beans_str_parse_int_range_or(char* s, long long from, long long to,
                                        long long fallback, long long line,
                                        long long col) {
    long long n = beans_slen(s);
    if (from < 0 || to < from || to > n) {
        char m[96];
        snprintf(m, sizeof m, "range %lld..%lld out of range (len %lld)",
                 from, to, n);
        beans_panic(m, line, col);
    }
    long long at = from;
    int negative = 0;
    if (at < to && (s[at] == '+' || s[at] == '-')) {
        negative = s[at] == '-';
        at += 1;
    }
    if (at == to) return fallback;
    unsigned long long value = 0;
    for (; at < to; at++) {
        unsigned char c = (unsigned char)s[at];
        if (c < '0' || c > '9') return fallback;
        value = value * 10 + (unsigned long long)(c - '0');
    }
    if (negative) value = 0 - value;
    return (long long)value;
}
static int str_is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
char* beans_str_trim(char* s) {
    long long b = 0, e = beans_slen(s);
    while (b < e && str_is_ws(s[b])) b++;
    while (e > b && str_is_ws(s[e - 1])) e--;
    return str_make(s + b, e - b);
}
char* beans_str_trim_start(char* s) {
    long long b = 0, e = beans_slen(s);
    while (b < e && str_is_ws(s[b])) b++;
    return str_make(s + b, e - b);
}
char* beans_str_trim_end(char* s) {
    long long e = beans_slen(s);
    while (e > 0 && str_is_ws(s[e - 1])) e--;
    return str_make(s, e);
}
char* beans_str_to_upper(char* s) {
    long long n = beans_slen(s);
    char* r = str_make(s, n);
    for (long long i = 0; i < n; i++) {
        if (r[i] >= 'a' && r[i] <= 'z') r[i] = (char)(r[i] - 'a' + 'A');
    }
    return r;
}
char* beans_str_to_lower(char* s) {
    long long n = beans_slen(s);
    char* r = str_make(s, n);
    for (long long i = 0; i < n; i++) {
        if (r[i] >= 'A' && r[i] <= 'Z') r[i] = (char)(r[i] - 'A' + 'a');
    }
    return r;
}
char* beans_str_replace(char* s, char* old, char* nw) {
    long long n = beans_slen(s), m = beans_slen(old), rl = beans_slen(nw);
    if (m == 0) return str_make(s, n); // replacing nothing changes nothing
    long long count = 0;
    for (long long i = str_search(s, n, old, m, 0); i >= 0;
         i = str_search(s, n, old, m, i + m)) {
        count++;
    }
    if (count == 0) return str_make(s, n);
    long long outn = n + count * (rl - m);
    char* out = beans_alloc(outn + 1, outn << 3);
    char* w = out;
    long long i = 0;
    for (;;) {
        long long j = str_search(s, n, old, m, i);
        if (j < 0) break;
        memcpy(w, s + i, (size_t)(j - i));
        w += j - i;
        memcpy(w, nw, (size_t)rl);
        w += rl;
        i = j + m;
    }
    memcpy(w, s + i, (size_t)(n - i));
    return out;
}
char* beans_str_repeat(char* s, long long n, long long line, long long col) {
    if (n < 0) {
        char m[64];
        snprintf(m, sizeof m, "negative repeat count %lld", n);
        beans_panic(m, line, col);
    }
    long long len = beans_slen(s);
    long long outn = len * n;
    char* out = beans_alloc(outn + 1, outn << 3);
    for (long long i = 0; i < n; i++) memcpy(out + i * len, s, (size_t)len);
    return out;
}
long long beans_f64_round(double v) { return llround(v); }

// ---- lists ----
static long long list_stride(BList* l) { return l->stride ? l->stride : 8; }
static void list_retain_element(BList* l, void* element) {
    for (int slot = 0; slot < 58 && (l->ptr_mask >> slot); slot++) {
        if (!((l->ptr_mask >> slot) & 1)) continue;
        void* child = *(void**)((char*)element + slot * 8);
        if (child) beans_retain(child);
    }
}
static void list_release_element(BList* l, void* element) {
    for (int slot = 0; slot < 58 && (l->ptr_mask >> slot); slot++) {
        if (!((l->ptr_mask >> slot) & 1)) continue;
        void* child = *(void**)((char*)element + slot * 8);
        if (child) beans_release(child);
    }
}
BList* beans_list_new_typed(long long stride, long long ptr_mask) {
    if (stride <= 0 || stride > (1LL << 30))
        beans_panic("invalid list element size", 0, 0);
    BList* l = beans_alloc(sizeof(BList), 2 | ((ptr_mask != 0) << 3));
    l->cap = 4;
    l->stride = stride;
    l->ptr_mask = ptr_mask;
    l->data = calloc(4, (size_t)stride);
    if (!l->data) beans_panic("out of memory", 0, 0);
    return l;
}
BList* beans_list_new(long long elem_ptr) {
    return beans_list_new_typed(8, elem_ptr);
}
void beans_list_push(BList* l, long long v) {
    if (l->len == l->cap) {
        l->cap *= 2;
        l->data = realloc(l->data, (size_t)l->cap * (size_t)list_stride(l));
        if (!l->data) beans_panic("out of memory", 0, 0);
    }
    l->data[l->len++] = v;
}
void beans_list_push_typed(BList* l, const void* value) {
    long long stride = list_stride(l);
    if (l->len == l->cap) {
        l->cap *= 2;
        l->data = realloc(l->data, (size_t)l->cap * (size_t)stride);
        if (!l->data) beans_panic("out of memory", 0, 0);
    }
    memcpy((char*)l->data + l->len * stride, value, (size_t)stride);
    l->len += 1;
}
void beans_list_reserve(BList* l, long long capacity, long long line, long long col) {
    if (capacity < 0) {
        char b[64];
        snprintf(b, sizeof b, "negative reserve capacity %lld", capacity);
        beans_panic(b, line, col);
    }
    if (capacity > (1LL << 58)) beans_panic("reserve capacity too large", line, col);
    if (capacity <= l->cap) return;
    long long cap = l->cap;
    while (cap < capacity && cap <= (1LL << 60)) cap *= 2;
    if (cap < capacity) cap = capacity;
    l->data = realloc(l->data, (size_t)cap * (size_t)list_stride(l));
    if (!l->data) beans_panic("out of memory", line, col);
    l->cap = cap;
}

// ---- class hierarchy (table emitted by the compiler) ----
extern long long beans_class_parents[];
long long beans_is_a(long long id, long long target) {
    while (id >= 0) {
        if (id == target) return 1;
        id = beans_class_parents[id];
    }
    return 0;
}

// ---- list search helpers (kind: 0 int-ish, 1 f64, 2 string, 3 decimal,
// 4 unordered — everything compares equal, so sort keeps the original order
// and min/max return the first element, like the interpreter's value_less
// returning false) ----
struct BDec;
int beans_dec_cmp(struct BDec* a, struct BDec* b);
static int slot_cmp(long long a, long long b, long long kind) {
    if (kind == 1) {
        double x, y;
        memcpy(&x, &a, 8);
        memcpy(&y, &b, 8);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    if (kind == 2) return beans_str_cmp((char*)a, (char*)b);
    if (kind == 3) return beans_dec_cmp((struct BDec*)a, (struct BDec*)b);
    if (kind == 4) return 0;
    if (kind == 5) {
        unsigned long long x = (unsigned long long)a;
        unsigned long long y = (unsigned long long)b;
        return x < y ? -1 : x > y ? 1 : 0;
    }
    if (kind == 6) {
        unsigned aa = (unsigned)a, bb = (unsigned)b;
        float x, y;
        memcpy(&x, &aa, 4);
        memcpy(&y, &bb, 4);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    return a < b ? -1 : a > b ? 1 : 0;
}
// content equality for strings — length header first, bytes second; strcmp
// would stop at an embedded NUL and lie
long long beans_str_eq(char* a, char* b) {
    long long n = beans_slen(a);
    return n == beans_slen(b) && memcmp(a, b, (size_t)n) == 0;
}
// equality kinds (separate lattice from the ordering kinds above), matching
// the interpreter's value_eq arm for arm: 0 raw slot (ints, bools, pointer
// identity), 1 f64 by IEEE value (NaN equals nothing), 2 string content,
// 3 decimal value, 4 caller-supplied structural eq (enums, Bytes), 5 never
// equal (maps and resource handles — value_eq's default arm)
static long long slot_eq(long long a, long long b, long long kind,
                         long long (*eq)(long long, long long)) {
    if (kind == 0) return a == b;
    if (kind == 1) {
        double x, y;
        memcpy(&x, &a, 8);
        memcpy(&y, &b, 8);
        return x == y;
    }
    if (kind == 2) return beans_str_eq((char*)a, (char*)b);
    if (kind == 3) return beans_dec_cmp((struct BDec*)a, (struct BDec*)b) == 0;
    if (kind == 4) return eq(a, b) != 0;
    if (kind == 6) {
        unsigned aa = (unsigned)a, bb = (unsigned)b;
        float x, y;
        memcpy(&x, &aa, 4);
        memcpy(&y, &bb, 4);
        return x == y;
    }
    return 0;
}
// hashes for the map index, one per equality kind. The contract is only that
// slot_eq-equal keys hash equal; the interpreter hashes differently and that
// is fine — nothing observable depends on hash values, iteration walks data.
static unsigned long long beans_mix64(unsigned long long x) {
    // One multiply is enough for an in-process table: unlike a persisted
    // cryptographic hash, this only needs to spread sequential integers and
    // aligned pointers across a power-of-two index. The old Murmur finalizer
    // used two multiplies and six dependent operations on every lookup.
    x ^= x >> 32;
    x *= 0xd6e8feb86659fd93ULL;
    x ^= x >> 32;
    return x;
}
long long beans_slot_mix(long long v) { return (long long)beans_mix64((unsigned long long)v); }
long long beans_f64_hash(long long v) {
    double x;
    memcpy(&x, &v, 8);
    if (x == 0.0) return (long long)beans_mix64(0); // -0.0 == 0.0
    return (long long)beans_mix64((unsigned long long)v);
}
long long beans_f32_hash(long long v) {
    unsigned bits = (unsigned)v;
    float x;
    memcpy(&x, &bits, 4);
    if (x == 0.0f) return (long long)beans_mix64(0);
    return (long long)beans_mix64(bits);
}
long long beans_str_hash(char* s) {
    long long n = beans_slen(s);
    unsigned long long h = 1469598103934665603ULL;
    for (long long i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return (long long)beans_mix64(h);
}
long long beans_dec_hash(struct BDec* d);
long long beans_bytes_hash(BList* b) {
    unsigned long long h = 1469598103934665603ULL;
    for (long long i = 0; i < b->len; i++) {
        h ^= ((unsigned char*)b->data)[i];
        h *= 1099511628211ULL;
    }
    return (long long)beans_mix64(h);
}
static unsigned long long slot_hash(long long v, long long kind,
                                    long long (*hf)(long long)) {
    if (kind == 1) return (unsigned long long)beans_f64_hash(v);
    if (kind == 2) return (unsigned long long)beans_str_hash((char*)v);
    if (kind == 3) return (unsigned long long)beans_dec_hash((struct BDec*)v);
    if (kind == 4) return (unsigned long long)hf(v);
    if (kind == 6) return (unsigned long long)beans_f32_hash(v);
    return beans_mix64((unsigned long long)v); // raw, and never-equal keys
}
long long beans_list_max(BList* l, long long kind, long long* ok) {
    *ok = l->len > 0;
    if (!*ok) return 0;
    long long best = l->data[0];
    for (long long i = 1; i < l->len; i++) {
        if (slot_cmp(l->data[i], best, kind) > 0) best = l->data[i];
    }
    return best;
}
long long beans_list_contains(BList* l, long long v, long long kind, void* eq) {
    for (long long i = 0; i < l->len; i++) {
        if (slot_eq(l->data[i], v, kind, (long long (*)(long long, long long))eq)) return 1;
    }
    return 0;
}
long long beans_list_min(BList* l, long long kind, long long* ok) {
    *ok = l->len > 0;
    if (!*ok) return 0;
    long long best = l->data[0];
    for (long long i = 1; i < l->len; i++) {
        if (slot_cmp(l->data[i], best, kind) < 0) best = l->data[i];
    }
    return best;
}
long long beans_list_index(BList* l, long long v, long long kind, long long* ok,
                           void* eq) {
    for (long long i = 0; i < l->len; i++) {
        if (slot_eq(l->data[i], v, kind, (long long (*)(long long, long long))eq)) {
            *ok = 1;
            return i;
        }
    }
    *ok = 0;
    return 0;
}
void beans_list_insert(BList* l, long long i, long long v, long long line,
                       long long col) {
    if (i < 0 || i > l->len) {
        char b[96];
        snprintf(b, sizeof b, "insert at %lld out of range (len %lld)", i, l->len);
        beans_panic(b, line, col);
    }
    if (l->len == l->cap) {
        l->cap *= 2;
        l->data = realloc(l->data, (size_t)l->cap * 8);
    }
    memmove(l->data + i + 1, l->data + i, (size_t)(l->len - i) * 8);
    l->data[i] = v;
    l->len += 1;
}
void beans_list_insert_typed(BList* l, long long i, const void* value,
                             long long line, long long col) {
    if (i < 0 || i > l->len) {
        char b[96];
        snprintf(b, sizeof b, "insert at %lld out of range (len %lld)", i, l->len);
        beans_panic(b, line, col);
    }
    long long stride = list_stride(l);
    if (l->len == l->cap) {
        l->cap *= 2;
        l->data = realloc(l->data, (size_t)l->cap * (size_t)stride);
        if (!l->data) beans_panic("out of memory", line, col);
    }
    char* at = (char*)l->data + i * stride;
    memmove(at + stride, at, (size_t)(l->len - i) * (size_t)stride);
    memcpy(at, value, (size_t)stride);
    l->len += 1;
}
long long beans_list_remove(BList* l, long long i, long long line, long long col) {
    if (i < 0 || i >= l->len) {
        char b[96];
        snprintf(b, sizeof b, "list index %lld out of range (len %lld)", i, l->len);
        beans_panic(b, line, col);
    }
    long long v = l->data[i];
    memmove(l->data + i, l->data + i + 1, (size_t)(l->len - i - 1) * 8);
    l->len -= 1;
    return v; // the caller now owns the moved-out ref
}
void beans_list_remove_typed(BList* l, long long i, void* out, long long line,
                             long long col) {
    if (i < 0 || i >= l->len) {
        char b[96];
        snprintf(b, sizeof b, "list index %lld out of range (len %lld)", i, l->len);
        beans_panic(b, line, col);
    }
    long long stride = list_stride(l);
    char* at = (char*)l->data + i * stride;
    memcpy(out, at, (size_t)stride);
    memmove(at, at + stride, (size_t)(l->len - i - 1) * (size_t)stride);
    l->len -= 1;
}
void beans_list_reverse(BList* l) {
    long long stride = list_stride(l);
    if (stride != 8) {
        void* tmp = malloc((size_t)stride);
        if (!tmp) beans_panic("out of memory", 0, 0);
        for (long long i = 0, j = l->len - 1; i < j; i++, j--) {
            char* a = (char*)l->data + i * stride;
            char* b = (char*)l->data + j * stride;
            memcpy(tmp, a, (size_t)stride);
            memcpy(a, b, (size_t)stride);
            memcpy(b, tmp, (size_t)stride);
        }
        free(tmp);
        return;
    }
    for (long long i = 0, j = l->len - 1; i < j; i++, j--) {
        long long t = l->data[i];
        l->data[i] = l->data[j];
        l->data[j] = t;
    }
}
void beans_list_clear(BList* l) {
    // last element first — deinit made death order observable, and the
    // interpreter's vector teardown destroys back to front
    if (l->ptr_mask) {
        long long stride = list_stride(l);
        for (long long i = l->len; i-- > 0;) {
            list_release_element(l, (char*)l->data + i * stride);
        }
    }
    l->len = 0;
}
BList* beans_list_slice(BList* l, long long from, long long to, long long line,
                        long long col) {
    if (from < 0 || to < from || to > l->len) {
        char b[96];
        snprintf(b, sizeof b, "slice %lld..%lld out of range (len %lld)", from, to,
                 l->len);
        beans_panic(b, line, col);
    }
    long long n = to - from;
    long long stride = list_stride(l);
    BList* r = beans_list_new_typed(stride, l->ptr_mask);
    r->len = n;
    r->cap = n > 4 ? n : 4;
    free(r->data);
    r->data = malloc((size_t)r->cap * (size_t)stride);
    if (!r->data) beans_panic("out of memory", line, col);
    memcpy(r->data, (char*)l->data + from * stride, (size_t)n * (size_t)stride);
    if (r->ptr_mask)
        for (long long i = 0; i < n; i++)
            list_retain_element(r, (char*)r->data + i * stride);
    return r;
}
BList* beans_list_clone(BList* l) {
    long long stride = list_stride(l);
    BList* r = beans_list_new_typed(stride, l->ptr_mask);
    r->len = l->len;
    r->cap = l->len > 4 ? l->len : 4;
    free(r->data);
    r->data = malloc((size_t)r->cap * (size_t)stride);
    if (!r->data) beans_panic("out of memory", 0, 0);
    memcpy(r->data, l->data, (size_t)l->len * (size_t)stride);
    if (r->ptr_mask)
        for (long long i = 0; i < r->len; i++)
            list_retain_element(r, (char*)r->data + i * stride);
    return r;
}

// bottom-up stable merge — structurally identical to the interpreter's
// stable_merge, so both backends produce the same order for ANY predicate,
// even one that is not a strict weak ordering
static long long sort_less(long long x, long long y, long long kind, void* thunk,
                           void* box) {
    if (thunk) return ((long long (*)(void*, long long, long long))thunk)(box, x, y);
    return slot_cmp(x, y, kind) < 0;
}
static void list_merge_sort(long long* a, long long n, long long kind, void* thunk,
                            void* box) {
    if (n < 2) return;
    long long* buf = malloc((size_t)n * 8);
    for (long long w = 1; w < n; w *= 2) {
        for (long long lo = 0; lo < n; lo += 2 * w) {
            long long mid = lo + w < n ? lo + w : n;
            long long hi = lo + 2 * w < n ? lo + 2 * w : n;
            if (mid >= hi) continue;
            long long i = lo, j = mid, o = lo;
            while (i < mid && j < hi) {
                if (!sort_less(a[j], a[i], kind, thunk, box)) buf[o++] = a[i++];
                else buf[o++] = a[j++];
            }
            while (i < mid) buf[o++] = a[i++];
            while (j < hi) buf[o++] = a[j++];
            memcpy(a + lo, buf + lo, (size_t)(hi - lo) * 8);
        }
    }
    free(buf);
}
// Signed integer slots have a cheaper stable path: one to four 16-bit passes
// after subtracting the observed minimum. Narrow real-world ranges therefore
// do one pass while the full signed range still takes four.
// This avoids a comparator branch for every merge comparison and keeps equal
// values in input order. bool uses the same path (its slots are 0/1).
static void list_radix_sort_int(long long* a, long long n) {
    if (n < 2) return;
    long long minimum = a[0], maximum = a[0];
    for (long long i = 1; i < n; i++) {
        if (a[i] < minimum) minimum = a[i];
        if (a[i] > maximum) maximum = a[i];
    }
    unsigned long long span =
        (unsigned long long)maximum - (unsigned long long)minimum;
    int passes = span <= 0xffffULL ? 1 : span <= 0xffffffffULL ? 2 : 4;
    long long* buf = malloc((size_t)n * 8);
    long long* src = a;
    long long* dst = buf;
    size_t* count = malloc(65536 * sizeof(size_t));
    size_t* at = malloc(65536 * sizeof(size_t));
    for (int pass = 0; pass < passes; pass++) {
        memset(count, 0, 65536 * sizeof(size_t));
        int shift = pass * 16;
        for (long long i = 0; i < n; i++) {
            unsigned long long key =
                (unsigned long long)src[i] - (unsigned long long)minimum;
            count[(key >> shift) & 65535]++;
        }
        size_t sum = 0;
        for (int word = 0; word < 65536; word++) {
            at[word] = sum;
            sum += count[word];
        }
        for (long long i = 0; i < n; i++) {
            unsigned long long key =
                (unsigned long long)src[i] - (unsigned long long)minimum;
            dst[at[(key >> shift) & 65535]++] = src[i];
        }
        long long* swap = src;
        src = dst;
        dst = swap;
    }
    if (src != a) memcpy(a, src, (size_t)n * 8);
    free(count);
    free(at);
    free(buf);
}
void beans_list_sort(BList* l, long long kind) {
    if (kind == 0) list_radix_sort_int(l->data, l->len);
    else list_merge_sort(l->data, l->len, kind, NULL, NULL);
}
void beans_list_sort_by(BList* l, void* thunk, void* box) {
    list_merge_sort(l->data, l->len, 0, thunk, box);
}
void beans_list_sort_by_key(BList* l, void* thunk, void* box) {
    long long n = l->len;
    if (n < 2) return;
    long long* keys = malloc((size_t)n * 8);
    long long* val_buf = malloc((size_t)n * 8);
    size_t* count = malloc(65536 * sizeof(size_t));
    size_t* at = malloc(65536 * sizeof(size_t));
    long long (*key_fn)(void*, long long) =
        (long long (*)(void*, long long))thunk;
    long long minimum = 0, maximum = 0;
    for (long long i = 0; i < n; i++) {
        keys[i] = key_fn(box, l->data[i]);
        if (i == 0 || keys[i] < minimum) minimum = keys[i];
        if (i == 0 || keys[i] > maximum) maximum = keys[i];
    }
    unsigned long long span =
        (unsigned long long)maximum - (unsigned long long)minimum;
    int passes = span <= 0xffffULL ? 1 : span <= 0xffffffffULL ? 2 : 4;
    long long* key_buf = passes > 1 ? malloc((size_t)n * 8) : NULL;
    long long* key_src = keys;
    long long* key_dst = key_buf;
    long long* val_src = l->data;
    long long* val_dst = val_buf;
    for (int pass = 0; pass < passes; pass++) {
        memset(count, 0, 65536 * sizeof(size_t));
        int shift = pass * 16;
        for (long long i = 0; i < n; i++) {
            unsigned long long key =
                (unsigned long long)key_src[i] - (unsigned long long)minimum;
            count[(key >> shift) & 65535]++;
        }
        size_t sum = 0;
        for (int word = 0; word < 65536; word++) {
            at[word] = sum;
            sum += count[word];
        }
        for (long long i = 0; i < n; i++) {
            unsigned long long key =
                (unsigned long long)key_src[i] - (unsigned long long)minimum;
            size_t out = at[(key >> shift) & 65535]++;
            if (passes > 1) key_dst[out] = key_src[i];
            val_dst[out] = val_src[i];
        }
        long long* swap;
        if (passes > 1) {
            swap = key_src; key_src = key_dst; key_dst = swap;
        }
        swap = val_src; val_src = val_dst; val_dst = swap;
    }
    if (val_src != l->data) memcpy(l->data, val_src, (size_t)n * 8);
    free(keys);
    free(key_buf);
    free(val_buf);
    free(count);
    free(at);
}

// ---- maps ----
// A flat key/value array plus an open-addressed index. OrderedMap leaves stable
// removal holes. Map swap-removes. Small maps have no index and scan linearly.
#define MAP_LINEAR_MAX 8
#define IDX_POS 0xffffffffULL /* low 32: pos+2, 1 = tombstone */
#define IDX_FRAG 0xffffffff00000000ULL /* high 32: hash fragment */
#define MAP_DEAD(m, p) ((m)->deadbits && (m)->deadbits[(p) >> 6] >> ((p)&63) & 1)
static void* map_wide_value(BMap* m, long long index) {
    return (char*)m->wide_values + index * m->value_stride;
}
static void map_retain_wide_value(BMap* m, void* value) {
    for (int slot = 0; slot < 58 && (m->value_ptr_mask >> slot); slot++) {
        if (!((m->value_ptr_mask >> slot) & 1)) continue;
        void* child = *(void**)((char*)value + slot * 8);
        if (child) beans_retain(child);
    }
}
static void map_release_wide_value(BMap* m, void* value) {
    // Aggregate teardown is last-field-first, matching generated structs.
    for (int slot = 58; slot-- > 0;) {
        if (!((m->value_ptr_mask >> slot) & 1)) continue;
        void* child = *(void**)((char*)value + slot * 8);
        if (child) beans_release(child);
    }
}
BMap* beans_map_new(long long key_ptr, long long val_ptr, long long ordered) {
    BMap* m = beans_alloc(sizeof(BMap), 3 | (key_ptr << 3) | (val_ptr << 4));
    m->cap = 4;
    m->data = calloc(8, 8); // idx/tombs/used/deadbits start zero: beans_alloc zeroes
    m->ordered = ordered;
    return m;
}
BMap* beans_map_new_typed_value(long long key_ptr, long long value_stride,
                                long long value_ptr_mask,
                                long long value_cycle_mask, long long ordered) {
    if (value_stride <= 0 || value_stride > (1LL << 30))
        beans_panic("invalid map value size", 0, 0);
    BMap* m = beans_map_new(key_ptr, value_ptr_mask != 0, ordered);
    m->value_stride = value_stride;
    m->value_ptr_mask = value_ptr_mask;
    m->value_cycle_mask = value_cycle_mask;
    m->wide_values = calloc(4, (size_t)value_stride);
    if (!m->wide_values) beans_panic("out of memory", 0, 0);
    return m;
}
// (re)build the index sized for the current entry count, dropping tombstones
// and compacting holes. Only moves slots and writes index words — never
// retains or releases.
static void map_reindex_to(BMap* m, long long kind, long long (*hf)(long long),
                           long long reserve) {
    if (m->deadbits) {
        long long w = 0;
        for (long long p = 0; p < m->used; p++) {
            if (MAP_DEAD(m, p)) continue;
            if (w != p) {
                m->data[w * 2] = m->data[p * 2];
                m->data[w * 2 + 1] = m->data[p * 2 + 1];
                if (m->wide_values)
                    memcpy(map_wide_value(m, w), map_wide_value(m, p),
                           (size_t)m->value_stride);
            }
            w += 1;
        }
        free(m->deadbits);
        m->deadbits = NULL;
        m->used = w; // == m->len
    }
    long long wanted = m->len > reserve ? m->len : reserve;
    long long cap = 16;
    while (wanted * 3 >= cap * 2) cap <<= 1;
    free(m->idx);
    m->idx = calloc((size_t)cap, 8);
    m->icap = cap;
    m->tombs = 0;
    unsigned long long mask = (unsigned long long)cap - 1;
    for (long long p = 0; p < m->len; p++) {
        unsigned long long h = slot_hash(m->data[p * 2], kind, hf);
        unsigned long long i = h & mask;
        while (m->idx[i] & IDX_POS) i = (i + 1) & mask;
        m->idx[i] = (h & IDX_FRAG) | (unsigned long long)(p + 2);
    }
}
static void map_reindex(BMap* m, long long kind, long long (*hf)(long long)) {
    map_reindex_to(m, kind, hf, 0);
}
void beans_map_reserve(BMap* m, long long capacity, long long kind, void* hash,
                       long long line, long long col) {
    if (capacity < 0) {
        char b[64];
        snprintf(b, sizeof b, "negative reserve capacity %lld", capacity);
        beans_panic(b, line, col);
    }
    if (capacity > (1LL << 58)) beans_panic("reserve capacity too large", line, col);
    if (capacity > m->cap) {
        long long cap = m->cap;
        while (cap < capacity && cap <= (1LL << 59)) cap *= 2;
        if (cap < capacity) cap = capacity;
        m->data = realloc(m->data, (size_t)cap * 16);
        if (!m->data) beans_panic("out of memory", line, col);
        if (m->wide_values) {
            m->wide_values = realloc(m->wide_values,
                                     (size_t)cap * (size_t)m->value_stride);
            if (!m->wide_values) beans_panic("out of memory", line, col);
        }
        if (m->deadbits) {
            long long old_words = (m->cap + 63) >> 6;
            long long new_words = (cap + 63) >> 6;
            m->deadbits = realloc(m->deadbits, (size_t)new_words * 8);
            memset(m->deadbits + old_words, 0,
                   (size_t)(new_words - old_words) * 8);
        }
        m->cap = cap;
    }
    map_reindex_to(m, kind, (long long (*)(long long))hash, capacity);
}
// keys compare with the same equality lattice as list search (slot_eq): raw,
// f64 value, string content, decimal value, structural thunk, never-equal.
// *hout is filled iff the index is active, so set can reuse it; *slot_out
// (may be NULL) gets the hit's index slot so remove can tombstone it O(1).
static long long map_find(BMap* m, long long key, long long kind, void* eq,
                          long long (*hf)(long long), unsigned long long* hout,
                          unsigned long long* slot_out) {
    if (!m->idx) {
        for (long long i = 0; i < m->len; i++) {
            if (slot_eq(m->data[i * 2], key, kind,
                        (long long (*)(long long, long long))eq)) return i;
        }
        return -1;
    }
    unsigned long long h = slot_hash(key, kind, hf);
    *hout = h;
    unsigned long long mask = (unsigned long long)m->icap - 1;
    unsigned long long frag = h & IDX_FRAG;
    unsigned long long first_tomb = ~0ULL;
    for (unsigned long long i = h & mask;; i = (i + 1) & mask) {
        unsigned long long w = m->idx[i];
        unsigned long long st = w & IDX_POS;
        if (st == 0) {
            if (slot_out) *slot_out = first_tomb != ~0ULL ? first_tomb : i;
            return -1;
        }
        if (st == 1 && first_tomb == ~0ULL) first_tomb = i;
        if (st >= 2 && (w & IDX_FRAG) == frag) {
            long long p = (long long)st - 2;
            if (slot_eq(m->data[p * 2], key, kind,
                        (long long (*)(long long, long long))eq)) {
                if (slot_out) *slot_out = i;
                return p;
            }
        }
    }
}
// Raw-slot keys (integers, bools, and identity keys) are the common map case.
// Keep their probe loop separate so every occupied bucket does not branch
// through the full equality/hash kind lattice.
static long long map_find_raw(BMap* m, long long key, unsigned long long* hout,
                              unsigned long long* slot_out) {
    if (!m->idx) {
        for (long long i = 0; i < m->len; i++) {
            if (m->data[i * 2] == key) return i;
        }
        return -1;
    }
    unsigned long long h = beans_mix64((unsigned long long)key);
    *hout = h;
    unsigned long long mask = (unsigned long long)m->icap - 1;
    unsigned long long frag = h & IDX_FRAG;
    unsigned long long first_tomb = ~0ULL;
    for (unsigned long long i = h & mask;; i = (i + 1) & mask) {
        unsigned long long w = m->idx[i];
        unsigned long long st = w & IDX_POS;
        if (st == 0) {
            if (slot_out) *slot_out = first_tomb != ~0ULL ? first_tomb : i;
            return -1;
        }
        if (st == 1 && first_tomb == ~0ULL) first_tomb = i;
        if (st >= 2 && (w & IDX_FRAG) == frag) {
            long long p = (long long)st - 2;
            if (m->data[p * 2] == key) {
                if (slot_out) *slot_out = i;
                return p;
            }
        }
    }
}

static void map_insert_miss(BMap* m, long long key, long long val,
                            unsigned long long h, long long kind,
                            long long (*hf)(long long),
                            unsigned long long insert_slot) {
    if (m->used == m->cap) {
        long long ow = (m->cap + 63) >> 6;
        m->cap *= 2;
        m->data = realloc(m->data, (size_t)m->cap * 16);
        if (m->deadbits) {
            long long nw = (m->cap + 63) >> 6;
            m->deadbits = realloc(m->deadbits, (size_t)nw * 8);
            memset(m->deadbits + ow, 0, (size_t)(nw - ow) * 8);
        }
    }
    m->data[m->used * 2] = key;
    m->data[m->used * 2 + 1] = val;
    m->used += 1;
    m->len += 1;
    if (!m->idx) {
        if (m->len > MAP_LINEAR_MAX) map_reindex(m, kind, hf);
    } else if ((m->used + m->tombs) * 3 >= m->icap * 2) {
        map_reindex(m, kind, hf);
    } else { // the miss probe already found the insertion slot
        if ((m->idx[insert_slot] & IDX_POS) == 1) m->tombs -= 1;
        m->idx[insert_slot] =
            (h & IDX_FRAG) | (unsigned long long)(m->used + 1);
    }
}

static void map_insert_miss_typed(BMap* m, long long key, void* value,
                                  unsigned long long h, long long kind,
                                  long long (*hf)(long long),
                                  unsigned long long insert_slot) {
    if (m->used == m->cap) {
        long long ow = (m->cap + 63) >> 6;
        m->cap *= 2;
        m->data = realloc(m->data, (size_t)m->cap * 16);
        m->wide_values = realloc(m->wide_values,
                                 (size_t)m->cap * (size_t)m->value_stride);
        if (!m->data || !m->wide_values) beans_panic("out of memory", 0, 0);
        if (m->deadbits) {
            long long nw = (m->cap + 63) >> 6;
            m->deadbits = realloc(m->deadbits, (size_t)nw * 8);
            memset(m->deadbits + ow, 0, (size_t)(nw - ow) * 8);
        }
    }
    m->data[m->used * 2] = key;
    m->data[m->used * 2 + 1] = 0;
    memcpy(map_wide_value(m, m->used), value, (size_t)m->value_stride);
    m->used += 1;
    m->len += 1;
    if (!m->idx) {
        if (m->len > MAP_LINEAR_MAX) map_reindex(m, kind, hf);
    } else if ((m->used + m->tombs) * 3 >= m->icap * 2) {
        map_reindex(m, kind, hf);
    } else {
        if ((m->idx[insert_slot] & IDX_POS) == 1) m->tombs -= 1;
        m->idx[insert_slot] =
            (h & IDX_FRAG) | (unsigned long long)(m->used + 1);
    }
}

// note: the map owns key and value refs; the caller retains before calling
void beans_map_set(BMap* m, long long key, long long val, long long kind, void* eq,
                   void* hash) {
    long long (*hf)(long long) = (long long (*)(long long))hash;
    unsigned long long h = 0, slot = 0;
    long long i = map_find(m, key, kind, eq, hf, &h, &slot);
    if (i >= 0) {
        long long flags = (head_of(m)->meta & CC_SHAPE) >> 3;
        if (flags & 1) beans_release((void*)key); // duplicate key not stored
        if (flags & 2) beans_release((void*)m->data[i * 2 + 1]);
        m->data[i * 2 + 1] = val;
        if (kind == 4 && (flags & 1)) cc_possible_root(m);
        return;
    }
    map_insert_miss(m, key, val, h, kind, hf, slot);
    if (kind == 4) cc_possible_root(m);
}

__attribute__((always_inline)) void beans_map_set_raw(BMap* m, long long key,
                                                       long long val) {
    unsigned long long h = 0, slot = 0;
    long long i = map_find_raw(m, key, &h, &slot);
    if (i >= 0) {
        long long flags = (head_of(m)->meta & CC_SHAPE) >> 3;
        if (flags & 1) beans_release((void*)key);
        if (flags & 2) beans_release((void*)m->data[i * 2 + 1]);
        m->data[i * 2 + 1] = val;
        return;
    }
    map_insert_miss(m, key, val, h, 0, NULL, slot);
}

void beans_map_set_typed(BMap* m, long long key, void* value, long long kind,
                         void* eq, void* hash) {
    long long (*hf)(long long) = (long long (*)(long long))hash;
    unsigned long long h = 0, slot = 0;
    long long i = map_find(m, key, kind, eq, hf, &h, &slot);
    if (i >= 0) {
        long long flags = (head_of(m)->meta & CC_SHAPE) >> 3;
        if (flags & 1) beans_release((void*)key);
        map_release_wide_value(m, map_wide_value(m, i));
        memcpy(map_wide_value(m, i), value, (size_t)m->value_stride);
        if (m->value_cycle_mask || kind == 4) cc_possible_root(m);
        return;
    }
    map_insert_miss_typed(m, key, value, h, kind, hf, slot);
    if (m->value_cycle_mask || kind == 4) cc_possible_root(m);
}

__attribute__((always_inline)) void beans_map_set_typed_raw(BMap* m,
                                                             long long key,
                                                             void* value) {
    unsigned long long h = 0, slot = 0;
    long long i = map_find_raw(m, key, &h, &slot);
    if (i >= 0) {
        long long flags = (head_of(m)->meta & CC_SHAPE) >> 3;
        if (flags & 1) beans_release((void*)key);
        map_release_wide_value(m, map_wide_value(m, i));
        memcpy(map_wide_value(m, i), value, (size_t)m->value_stride);
        if (m->value_cycle_mask) cc_possible_root(m);
        return;
    }
    map_insert_miss_typed(m, key, value, h, 0, NULL, slot);
    if (m->value_cycle_mask) cc_possible_root(m);
}

// One-probe lowering for `m[k] = m.get(k).or(0) + delta`. The incoming key
// carries an owned reference, just like map_set: a hit drops the duplicate and
// a miss transfers it into the map. Unsigned addition gives Beans' wrapping
// int behavior without signed-overflow UB in the C runtime.
void beans_map_add(BMap* m, long long key, long long delta, long long kind,
                   void* eq, void* hash) {
    long long (*hf)(long long) = (long long (*)(long long))hash;
    unsigned long long h = 0, slot = 0;
    long long i = map_find(m, key, kind, eq, hf, &h, &slot);
    if (i >= 0) {
        long long flags = (head_of(m)->meta & CC_SHAPE) >> 3;
        if (flags & 1) beans_release((void*)key);
        m->data[i * 2 + 1] =
            (long long)((unsigned long long)m->data[i * 2 + 1] +
                        (unsigned long long)delta);
        return;
    }
    map_insert_miss(m, key, delta, h, kind, hf, slot);
}

__attribute__((always_inline)) void beans_map_add_raw(BMap* m, long long key,
                                                       long long delta) {
    unsigned long long h = 0, slot = 0;
    long long i = map_find_raw(m, key, &h, &slot);
    if (i >= 0) {
        long long flags = (head_of(m)->meta & CC_SHAPE) >> 3;
        if (flags & 1) beans_release((void*)key);
        m->data[i * 2 + 1] =
            (long long)((unsigned long long)m->data[i * 2 + 1] +
                        (unsigned long long)delta);
        return;
    }
    map_insert_miss(m, key, delta, h, 0, NULL, slot);
}

long long beans_map_insert(BMap* m, long long key, long long val, long long kind,
                           void* eq, void* hash) {
    long long (*hf)(long long) = (long long (*)(long long))hash;
    unsigned long long h = 0, slot = 0;
    if (map_find(m, key, kind, eq, hf, &h, &slot) >= 0) {
        long long flags = (head_of(m)->meta & CC_SHAPE) >> 3;
        if (flags & 1) beans_release((void*)key);
        if (flags & 2) beans_release((void*)val);
        return 0;
    }
    map_insert_miss(m, key, val, h, kind, hf, slot);
    if (kind == 4) cc_possible_root(m);
    return 1;
}

long long beans_map_insert_raw(BMap* m, long long key, long long val) {
    unsigned long long h = 0, slot = 0;
    if (map_find_raw(m, key, &h, &slot) >= 0) {
        long long flags = (head_of(m)->meta & CC_SHAPE) >> 3;
        if (flags & 1) beans_release((void*)key);
        if (flags & 2) beans_release((void*)val);
        return 0;
    }
    map_insert_miss(m, key, val, h, 0, NULL, slot);
    return 1;
}

long long beans_map_insert_typed(BMap* m, long long key, void* value,
                                 long long kind, void* eq, void* hash) {
    long long (*hf)(long long) = (long long (*)(long long))hash;
    unsigned long long h = 0, slot = 0;
    if (map_find(m, key, kind, eq, hf, &h, &slot) >= 0) {
        long long flags = (head_of(m)->meta & CC_SHAPE) >> 3;
        if (flags & 1) beans_release((void*)key);
        map_release_wide_value(m, value);
        return 0;
    }
    map_insert_miss_typed(m, key, value, h, kind, hf, slot);
    if (m->value_cycle_mask || kind == 4) cc_possible_root(m);
    return 1;
}

long long beans_map_insert_typed_raw(BMap* m, long long key, void* value) {
    unsigned long long h = 0, slot = 0;
    if (map_find_raw(m, key, &h, &slot) >= 0) {
        long long flags = (head_of(m)->meta & CC_SHAPE) >> 3;
        if (flags & 1) beans_release((void*)key);
        map_release_wide_value(m, value);
        return 0;
    }
    map_insert_miss_typed(m, key, value, h, 0, NULL, slot);
    if (m->value_cycle_mask) cc_possible_root(m);
    return 1;
}

__attribute__((always_inline)) BOpt beans_map_get_raw(BMap* m, long long key) {
    unsigned long long h = 0;
    long long i = map_find_raw(m, key, &h, NULL);
    return i >= 0 ? (BOpt){m->data[i * 2 + 1], 1} : (BOpt){0, 0};
}

long long beans_map_contains_raw(BMap* m, long long key) {
    unsigned long long h = 0;
    return map_find_raw(m, key, &h, NULL) >= 0;
}

long long beans_map_get(BMap* m, long long key, long long kind, long long* ok,
                        void* eq, void* hash) {
    unsigned long long h = 0;
    long long i = map_find(m, key, kind, eq, (long long (*)(long long))hash, &h, 0);
    *ok = i >= 0;
    return i >= 0 ? m->data[i * 2 + 1] : 0;
}
long long beans_map_get_typed(BMap* m, long long key, long long kind, void* out,
                              void* eq, void* hash) {
    unsigned long long h = 0;
    long long i = map_find(m, key, kind, eq, (long long (*)(long long))hash, &h, 0);
    if (i < 0) return 0;
    memcpy(out, map_wide_value(m, i), (size_t)m->value_stride);
    return 1;
}
long long beans_map_get_typed_raw(BMap* m, long long key, void* out) {
    unsigned long long h = 0;
    long long i = map_find_raw(m, key, &h, NULL);
    if (i < 0) return 0;
    memcpy(out, map_wide_value(m, i), (size_t)m->value_stride);
    return 1;
}
static long long map_remove_found(BMap* m, long long i,
                                  unsigned long long slot, long long kind,
                                  long long (*hf)(long long)) {
    long long flags = (head_of(m)->meta & CC_SHAPE) >> 3;
    if ((flags & 1) && m->data[i * 2]) beans_release((void*)m->data[i * 2]);
    if (m->wide_values) map_release_wide_value(m, map_wide_value(m, i));
    else if ((flags & 2) && m->data[i * 2 + 1])
        beans_release((void*)m->data[i * 2 + 1]);
    if (!m->idx) {
        if (m->ordered) {
            memmove(m->data + i * 2, m->data + (i + 1) * 2,
                    (size_t)(m->used - i - 1) * 16);
            if (m->wide_values)
                memmove(map_wide_value(m, i), map_wide_value(m, i + 1),
                        (size_t)(m->used - i - 1) * (size_t)m->value_stride);
        } else if (i != m->used - 1) {
            m->data[i * 2] = m->data[(m->used - 1) * 2];
            m->data[i * 2 + 1] = m->data[(m->used - 1) * 2 + 1];
            if (m->wide_values)
                memcpy(map_wide_value(m, i), map_wide_value(m, m->used - 1),
                       (size_t)m->value_stride);
        }
        if (m->wide_values)
            memset(map_wide_value(m, m->used - 1), 0, (size_t)m->value_stride);
        m->len -= 1;
        m->used -= 1;
        return 1;
    }
    if (!m->ordered) {
        long long last = m->used - 1;
        m->idx[slot] = 1;
        m->tombs += 1;
        if (i != last) {
            long long moved_key = m->data[last * 2];
            unsigned long long h = slot_hash(moved_key, kind, hf);
            unsigned long long mask = (unsigned long long)m->icap - 1;
            for (unsigned long long at = h & mask;; at = (at + 1) & mask) {
                unsigned long long state = m->idx[at] & IDX_POS;
                if (state == (unsigned long long)(last + 2)) {
                    m->idx[at] = (m->idx[at] & IDX_FRAG) |
                                 (unsigned long long)(i + 2);
                    break;
                }
            }
            m->data[i * 2] = m->data[last * 2];
            m->data[i * 2 + 1] = m->data[last * 2 + 1];
            if (m->wide_values)
                memcpy(map_wide_value(m, i), map_wide_value(m, last),
                       (size_t)m->value_stride);
        }
        m->data[last * 2] = 0;
        m->data[last * 2 + 1] = 0;
        if (m->wide_values)
            memset(map_wide_value(m, last), 0, (size_t)m->value_stride);
        m->len -= 1;
        m->used -= 1;
        if (m->tombs > m->len) map_reindex(m, kind, hf);
        return 1;
    }
    // indexed: zero the pair into a hole — no entry moves, so no index
    // position needs fixing and delete is O(1). Reindex compacts once
    // holes outnumber live entries, so the cost is amortized.
    m->data[i * 2] = 0;
    m->data[i * 2 + 1] = 0;
    if (m->wide_values)
        memset(map_wide_value(m, i), 0, (size_t)m->value_stride);
    if (!m->deadbits) m->deadbits = calloc((size_t)((m->cap + 63) >> 6), 8);
    m->deadbits[i >> 6] |= 1ULL << (i & 63);
    m->len -= 1;
    m->idx[slot] = 1; // map_find landed on the hit's slot
    m->tombs += 1;
    if (m->used > m->len * 2) map_reindex(m, kind, hf);
    return 1;
}
long long beans_map_remove(BMap* m, long long key, long long kind, void* eq,
                           void* hash) {
    long long (*hf)(long long) = (long long (*)(long long))hash;
    unsigned long long h = 0, slot = 0;
    long long i = map_find(m, key, kind, eq, hf, &h, &slot);
    return i < 0 ? 0 : map_remove_found(m, i, slot, kind, hf);
}
long long beans_map_remove_raw(BMap* m, long long key) {
    unsigned long long h = 0, slot = 0;
    long long i = map_find_raw(m, key, &h, &slot);
    return i < 0 ? 0 : map_remove_found(m, i, slot, 0, NULL);
}
BMap* beans_map_clone(BMap* m, long long kind, void* hash) {
    long long flags = (head_of(m)->meta & CC_SHAPE) >> 3;
    BMap* copy = m->wide_values
        ? beans_map_new_typed_value(flags & 1, m->value_stride,
                                    m->value_ptr_mask, m->value_cycle_mask,
                                    m->ordered)
        : beans_map_new(flags & 1, (flags >> 1) & 1, m->ordered);
    if (m->len > copy->cap) {
        copy->cap = m->len;
        copy->data = realloc(copy->data, (size_t)copy->cap * 16);
        if (!copy->data) beans_panic("out of memory", 0, 0);
        if (copy->wide_values) {
            copy->wide_values = realloc(copy->wide_values,
                                        (size_t)copy->cap *
                                            (size_t)copy->value_stride);
            if (!copy->wide_values) beans_panic("out of memory", 0, 0);
        }
    }
    for (long long i = 0; i < m->used; i++) {
        if (MAP_DEAD(m, i)) continue;
        long long key = m->data[i * 2];
        long long value = m->data[i * 2 + 1];
        if ((flags & 1) && key) beans_retain((void*)key);
        copy->data[copy->used * 2] = key;
        if (m->wide_values) {
            copy->data[copy->used * 2 + 1] = 0;
            memcpy(map_wide_value(copy, copy->used), map_wide_value(m, i),
                   (size_t)m->value_stride);
            map_retain_wide_value(copy, map_wide_value(copy, copy->used));
        } else {
            if ((flags & 2) && value) beans_retain((void*)value);
            copy->data[copy->used * 2 + 1] = value;
        }
        copy->used += 1;
        copy->len += 1;
    }
    if (copy->len > MAP_LINEAR_MAX)
        map_reindex(copy, kind, (long long (*)(long long))hash);
    return copy;
}
BList* beans_map_keys(BMap* m) {
    long long flags = (head_of(m)->meta & CC_SHAPE) >> 3;
    BList* l = beans_list_new(flags & 1);
    for (long long i = 0; i < m->used; i++) {
        if (MAP_DEAD(m, i)) continue;
        long long k = m->data[i * 2];
        if ((flags & 1) && k) beans_retain((void*)k);
        beans_list_push(l, k);
    }
    return l;
}
BList* beans_map_keys_typed(BMap* m, long long stride, long long ptr_mask) {
    if (stride <= 0 || stride > (1LL << 30))
        beans_panic("invalid map key size", 0, 0);
    BList* l = beans_list_new_typed(stride, ptr_mask);
    for (long long i = 0; i < m->used; i++) {
        if (MAP_DEAD(m, i)) continue;
        void* key = (void*)m->data[i * 2];
        for (int slot = 0; slot < 58 && (ptr_mask >> slot); slot++) {
            if (!((ptr_mask >> slot) & 1)) continue;
            void* child = *(void**)((char*)key + slot * 8);
            if (child) beans_retain(child);
        }
        beans_list_push_typed(l, key);
    }
    return l;
}
BList* beans_map_values(BMap* m) {
    long long flags = (head_of(m)->meta & CC_SHAPE) >> 3;
    BList* l = m->wide_values
        ? beans_list_new_typed(m->value_stride, m->value_ptr_mask)
        : beans_list_new((flags >> 1) & 1);
    for (long long i = 0; i < m->used; i++) {
        if (MAP_DEAD(m, i)) continue;
        if (m->wide_values) {
            void* value = map_wide_value(m, i);
            map_retain_wide_value(m, value);
            beans_list_push_typed(l, value);
            continue;
        }
        long long v = m->data[i * 2 + 1];
        if ((flags & 2) && v) beans_retain((void*)v);
        beans_list_push(l, v);
    }
    return l;
}
void beans_map_clear(BMap* m) {
    long long flags = (head_of(m)->meta & CC_SHAPE) >> 3;
    // reverse, value before key: the interpreter's pair teardown runs members
    // last-first, entries back to front — observable once a deinit prints
    for (long long i = m->used; i-- > 0;) { // holes are zeroed: null-skip
        if (m->wide_values) map_release_wide_value(m, map_wide_value(m, i));
        else if ((flags & 2) && m->data[i * 2 + 1])
            beans_release((void*)m->data[i * 2 + 1]);
        if ((flags & 1) && m->data[i * 2]) beans_release((void*)m->data[i * 2]);
    }
    m->len = 0;
    m->used = 0;
    free(m->deadbits);
    m->deadbits = NULL;
    free(m->idx);
    m->idx = NULL;
    m->icap = 0;
    m->tombs = 0;
}

// element rendering matches the interpreter's display(): the kind code says
// how each slot turns into text (0 int, 1 f64, 2 str, 3 dec, 4 bool)
char* beans_dec_str(struct BDec* a);
char* beans_list_join(BList* l, char* sep, long long kind) {
    long long sl = beans_slen(sep);
    char** parts = malloc((size_t)(l->len ? l->len : 1) * sizeof(char*));
    long long total = 0;
    for (long long i = 0; i < l->len; i++) {
        long long v = l->data[i];
        char* s;
        if (kind == 2) {
            s = (char*)v;
        } else if (kind == 0) {
            s = beans_from_int(v);
        } else if (kind == 1) {
            double d;
            memcpy(&d, &v, 8);
            s = beans_from_float(d);
        } else if (kind == 3) {
            s = beans_dec_str((struct BDec*)v);
        } else {
            s = beans_from_bool((int)v);
        }
        parts[i] = s;
        total += beans_slen(s);
        if (i) total += sl;
    }
    char* out = beans_alloc(total + 1, total << 3);
    char* w = out;
    for (long long i = 0; i < l->len; i++) {
        if (i) {
            memcpy(w, sep, (size_t)sl);
            w += sl;
        }
        long long n = beans_slen(parts[i]);
        memcpy(w, parts[i], (size_t)n);
        w += n;
        if (kind != 2) beans_release(parts[i]); // rendered copies are ours
    }
    free(parts);
    return out;
}

// UTF-8 sequences, one string per character; a malformed lead or truncated
// tail comes through one byte at a time — byte slicing, no validation
BList* beans_str_chars(char* s) {
    long long len = beans_slen(s);
    BList* l = beans_list_new(1);
    long long i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        long long n = c < 0x80          ? 1
                      : (c >> 5) == 0x6 ? 2
                      : (c >> 4) == 0xE ? 3
                      : (c >> 3) == 0x1E ? 4
                                         : 1;
        if (i + n > len) {
            n = 1;
        } else {
            for (long long k = 1; k < n; k++) {
                if (((unsigned char)s[i + k] >> 6) != 0x2) {
                    n = 1;
                    break;
                }
            }
        }
        beans_list_push(l, (long long)str_make(s + i, n));
        i += n;
    }
    return l;
}
__attribute__((always_inline)) long long beans_str_count_chars(
    char* s, long long from, long long to, long long line, long long col) {
    long long len = beans_slen(s);
    if (from < 0 || to < from || to > len) {
        char m[112];
        snprintf(m, sizeof m, "character range %lld..%lld out of range (len %lld)",
                 from, to, len);
        beans_panic(m, line, col);
    }
    long long count = 0;
    for (long long i = from; i < to; count++) {
        unsigned char c = (unsigned char)s[i];
        long long n = c < 0x80           ? 1
                      : (c >> 5) == 0x6  ? 2
                      : (c >> 4) == 0xE  ? 3
                      : (c >> 3) == 0x1E ? 4
                                         : 1;
        if (i + n > to) {
            n = 1;
        } else {
            for (long long k = 1; k < n; k++) {
                if (((unsigned char)s[i + k] >> 6) != 0x2) {
                    n = 1;
                    break;
                }
            }
        }
        i += n;
    }
    return count;
}

// ---- display through per-type show fns (emitted by the compiler) ----
// show(slot) returns an owned string; we copy and release it per element
static char* show_join(BList* l, const char* sep, long long sl,
                       char* (*show)(long long), int brackets) {
    long long cap = 16, len = 0;
    char* buf = malloc((size_t)cap);
    if (brackets) buf[len++] = '[';
    for (long long i = 0; i < l->len; i++) {
        char* s = show(l->data[i]);
        long long n = beans_slen(s);
        long long need = len + n + sl + 2;
        if (need > cap) {
            while (cap < need) cap *= 2;
            buf = realloc(buf, (size_t)cap);
        }
        if (i) {
            memcpy(buf + len, sep, (size_t)sl);
            len += sl;
        }
        memcpy(buf + len, s, (size_t)n);
        len += n;
        beans_release(s);
    }
    if (brackets) buf[len++] = ']';
    char* out = str_make(buf, len);
    free(buf);
    return out;
}
// ---- iterative show driver ----
// printing recursed on data depth (a 400k-link enum chain smashed the C
// stack); generated @bstep fns append their own text and PUSH child work
// instead of calling each other, and this driver drains the stack. Text
// items are borrowed (interned constants or C literals); scalar steps
// append their temporary and release it.
typedef struct {
    void* fn; // step fn, or null for a text item
    long long v;
    const char* text;
    long long tlen;
} BShowItem;
typedef struct BShowCtx {
    BShowItem* items;
    long long len, cap;
    char* out;
    long long olen, ocap;
} BShowCtx;
static void show_out(BShowCtx* c, const char* p, long long n) {
    if (c->olen + n + 1 > c->ocap) {
        c->ocap = (c->olen + n + 1) * 2 + 16;
        c->out = realloc(c->out, (size_t)c->ocap);
    }
    memcpy(c->out + c->olen, p, (size_t)n);
    c->olen += n;
}
void beans_show_append(BShowCtx* c, char* s) { show_out(c, s, beans_slen(s)); }
static void show_push(BShowCtx* c, void* fn, long long v, const char* t, long long tn) {
    if (c->len == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 32;
        c->items = realloc(c->items, (size_t)c->cap * sizeof(BShowItem));
    }
    BShowItem it = {fn, v, t, tn};
    c->items[c->len++] = it;
}
void beans_show_push_val(BShowCtx* c, void* fn, long long v) {
    show_push(c, fn, v, NULL, 0);
}
void beans_show_push_lit(BShowCtx* c, char* s) {
    show_push(c, NULL, 0, s, beans_slen(s));
}
void beans_show_list_iter(BShowCtx* c, BList* l, void* elem_step) {
    show_out(c, "[", 1);
    show_push(c, NULL, 0, "]", 1);
    for (long long i = l->len; i-- > 1;) {
        show_push(c, elem_step, l->data[i], NULL, 0);
        show_push(c, NULL, 0, ", ", 2);
    }
    if (l->len > 0) show_push(c, elem_step, l->data[0], NULL, 0);
}
char* beans_show_run(void* fn, long long v) {
    BShowCtx c = {0, 0, 0, 0, 0, 0};
    show_push(&c, fn, v, NULL, 0);
    while (c.len > 0) {
        BShowItem it = c.items[--c.len];
        if (it.fn) ((void (*)(BShowCtx*, long long))it.fn)(&c, it.v);
        else show_out(&c, it.text, it.tlen);
    }
    char* r = str_make(c.out ? c.out : "", c.olen);
    free(c.out);
    free(c.items);
    return r;
}

char* beans_show_list(BList* l, char* (*show)(long long)) {
    return show_join(l, ", ", 2, show, 1);
}
char* beans_list_join_show(BList* l, char* sep, char* (*show)(long long)) {
    return show_join(l, sep, beans_slen(sep), show, 0);
}

// ---- string ops that build lists ----
BList* beans_str_split(char* s, char* sep) {
    BList* l = beans_list_new(1);
    long long n = beans_slen(s), m = beans_slen(sep);
    if (m == 0) { // no separator: the whole string, one piece
        beans_list_push(l, (long long)str_make(s, n));
        return l;
    }
    long long i = 0;
    for (long long j = str_search(s, n, sep, m, 0); j >= 0;
         j = str_search(s, n, sep, m, i)) {
        beans_list_push(l, (long long)str_make(s + i, j - i));
        i = j + m;
    }
    beans_list_push(l, (long long)str_make(s + i, n - i));
    return l;
}
BList* beans_str_lines(char* s) {
    BList* l = beans_list_new(1);
    long long n = beans_slen(s), i = 0;
    for (long long j = 0; j < n; j++) {
        if (s[j] == '\n') {
            beans_list_push(l, (long long)str_make(s + i, j - i));
            i = j + 1;
        }
    }
    // a trailing newline doesn't make an empty final line
    if (i < n) beans_list_push(l, (long long)str_make(s + i, n - i));
    return l;
}

// ---- Bytes: kind 2 with no element pointers — data freed, never walked ----
static BList* bytes_mk(long long n) {
    BList* b = beans_alloc(sizeof(BList), 2);
    long long cap = n < 8 ? 8 : n;
    b->data = calloc((size_t)cap, 1);
    if (!b->data) beans_panic("out of memory", 0, 0);
    b->len = n;
    b->cap = cap;
    return b;
}
BList* beans_bytes_new(long long n, long long line, long long col) {
    if (n < 0) {
        char m[48];
        snprintf(m, sizeof m, "negative size %lld", n);
        beans_panic(m, line, col);
    }
    return bytes_mk(n);
}
BList* beans_bytes_from(char* s) {
    long long n = beans_slen(s);
    BList* b = bytes_mk(n);
    memcpy(b->data, s, (size_t)n);
    return b;
}
__attribute__((always_inline)) long long beans_bytes_len(BList* b) {
    return b->len;
}
long long beans_bytes_eq(BList* a, BList* b) {
    return a->len == b->len && memcmp(a->data, b->data, (size_t)a->len) == 0;
}

// unsigned LEB128 over the 64-bit two's-complement pattern (negatives take
// 10 bytes); crc32 is the IEEE polynomial, table-driven — builtins.cpp
// computes the identical table
static void bytes_grow(BList* b, long long need);
void beans_bytes_append_varint(BList* b, long long x) {
    unsigned long long v = (unsigned long long)x;
    while (v >= 0x80) {
        bytes_grow(b, b->len + 1);
        ((unsigned char*)b->data)[b->len++] = (unsigned char)(v | 0x80);
        v >>= 7;
    }
    bytes_grow(b, b->len + 1);
    ((unsigned char*)b->data)[b->len++] = (unsigned char)v;
}
long long beans_bytes_get_varint(BList* b, long long pos, long long line,
                                 long long col) {
    unsigned long long v = 0;
    long long shift = 0;
    long long i = pos < 0 ? b->len : pos;
    while (1) {
        if (pos < 0 || i >= b->len) {
            char m[96];
            snprintf(m, sizeof m, "varint read at %lld out of range (len %lld)", pos,
                     b->len);
            beans_panic(m, line, col);
        }
        if (shift >= 64) {
            char m[96];
            snprintf(m, sizeof m, "varint too long at %lld", pos);
            beans_panic(m, line, col);
        }
        unsigned char byte = ((unsigned char*)b->data)[i++];
        v |= (unsigned long long)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) break;
        shift += 7;
    }
    return (long long)v;
}
long long beans_bytes_varint_size(long long x) {
    unsigned long long v = (unsigned long long)x;
    long long n = 1;
    while (v >= 0x80) {
        v >>= 7;
        n++;
    }
    return n;
}
static unsigned int crc_table[256];
static int crc_ready = 0;
long long beans_bytes_crc32(BList* b, long long from, long long to, long long line,
                            long long col) {
    if (from < 0 || to < from || to > b->len) {
        char m[96];
        snprintf(m, sizeof m, "crc32 %lld..%lld out of range (len %lld)", from, to,
                 b->len);
        beans_panic(m, line, col);
    }
    if (!crc_ready) {
        for (unsigned int i = 0; i < 256; i++) {
            unsigned int c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
        crc_ready = 1;
    }
    unsigned int c = 0xFFFFFFFFu;
    for (long long i = from; i < to; i++) {
        c = crc_table[(c ^ ((unsigned char*)b->data)[i]) & 0xFF] ^ (c >> 8);
    }
    return (long long)(c ^ 0xFFFFFFFFu);
}
static void bytes_grow(BList* b, long long need) {
    if (need <= b->cap) return;
    long long cap = b->cap;
    while (cap < need) cap *= 2;
    b->data = realloc(b->data, (size_t)cap);
    b->cap = cap;
}
void beans_bytes_resize(BList* b, long long n, long long line, long long col) {
    if (n < 0) {
        char m[48];
        snprintf(m, sizeof m, "negative size %lld", n);
        beans_panic(m, line, col);
    }
    bytes_grow(b, n);
    // regrown range reads as zero, like the interpreter's vector resize
    if (n > b->len) memset((char*)b->data + b->len, 0, (size_t)(n - b->len));
    b->len = n;
}
void beans_bytes_reserve(BList* b, long long n, long long line, long long col) {
    if (n < 0) {
        char m[64];
        snprintf(m, sizeof m, "negative reserve capacity %lld", n);
        beans_panic(m, line, col);
    }
    if (n > (1LL << 58)) beans_panic("reserve capacity too large", line, col);
    bytes_grow(b, n);
}
void beans_bytes_fill(BList* b, long long v) {
    memset(b->data, (int)(v & 255), (size_t)b->len);
}
static void bytes_oob(long long i, long long len, long long line, long long col) {
    char m[80];
    snprintf(m, sizeof m, "byte index %lld out of range (len %lld)", i, len);
    beans_panic(m, line, col);
}
__attribute__((always_inline)) long long beans_bytes_get(
    BList* b, long long i, long long line, long long col) {
    if (i < 0 || i >= b->len) bytes_oob(i, b->len, line, col);
    return (long long)((unsigned char*)b->data)[i];
}
void beans_bytes_set(BList* b, long long i, long long v, long long line, long long col) {
    if (i < 0 || i >= b->len) bytes_oob(i, b->len, line, col);
    ((unsigned char*)b->data)[i] = (unsigned char)(v & 255);
}
__attribute__((always_inline)) void beans_bytes_push(BList* b, long long v) {
    bytes_grow(b, b->len + 1);
    ((unsigned char*)b->data)[b->len++] = (unsigned char)v;
}
static void bytes_woob(const char* what, const char* op, long long pos, long long len,
                       long long line, long long col) {
    char m[96];
    snprintf(m, sizeof m, "%s %s at %lld out of range (len %lld)", what, op, pos, len);
    beans_panic(m, line, col);
}
static long long bytes_getw(BList* b, long long pos, long long w, const char* what,
                            long long line, long long col) {
    // pos > len - w, never pos + w > len: signed overflow on huge pos slips the
    // wrapped sum past the guard and the memcpy goes wild
    if (pos < 0 || w > b->len || pos > b->len - w) bytes_woob(what, "read", pos, b->len, line, col);
    unsigned long long v = 0;
    memcpy(&v, (char*)b->data + pos, (size_t)w); // little-endian hosts, documented
    return (long long)v;
}
static void bytes_putw(BList* b, long long pos, long long w, long long val,
                       const char* what, long long line, long long col) {
    if (pos < 0 || w > b->len || pos > b->len - w) bytes_woob(what, "write", pos, b->len, line, col);
    unsigned long long v = (unsigned long long)val;
    memcpy((char*)b->data + pos, &v, (size_t)w);
}
long long beans_bytes_get_u8(BList* b, long long p, long long l, long long c) { return bytes_getw(b, p, 1, "u8", l, c); }
long long beans_bytes_get_u16(BList* b, long long p, long long l, long long c) { return bytes_getw(b, p, 2, "u16", l, c); }
long long beans_bytes_get_u32(BList* b, long long p, long long l, long long c) { return bytes_getw(b, p, 4, "u32", l, c); }
long long beans_bytes_get_u64(BList* b, long long p, long long l, long long c) { return bytes_getw(b, p, 8, "u64", l, c); }
__attribute__((always_inline)) long long beans_bytes_get_i64(
    BList* b, long long p, long long l, long long c) {
    return bytes_getw(b, p, 8, "i64", l, c);
}
void beans_bytes_put_u8(BList* b, long long p, long long v, long long l, long long c) { bytes_putw(b, p, 1, v, "u8", l, c); }
void beans_bytes_put_u16(BList* b, long long p, long long v, long long l, long long c) { bytes_putw(b, p, 2, v, "u16", l, c); }
void beans_bytes_put_u32(BList* b, long long p, long long v, long long l, long long c) { bytes_putw(b, p, 4, v, "u32", l, c); }
void beans_bytes_put_u64(BList* b, long long p, long long v, long long l, long long c) { bytes_putw(b, p, 8, v, "u64", l, c); }
void beans_bytes_put_i64(BList* b, long long p, long long v, long long l, long long c) { bytes_putw(b, p, 8, v, "i64", l, c); }
BList* beans_bytes_slice(BList* b, long long from, long long to, long long line,
                         long long col) {
    if (from < 0 || to < from || to > b->len) {
        char m[96];
        snprintf(m, sizeof m, "slice %lld..%lld out of range (len %lld)", from, to,
                 b->len);
        beans_panic(m, line, col);
    }
    BList* r = bytes_mk(to - from);
    memcpy(r->data, (char*)b->data + from, (size_t)(to - from));
    return r;
}
void beans_bytes_copy_from(BList* b, BList* src, long long at, long long line,
                           long long col) {
    if (at < 0 || src->len > b->len || at > b->len - src->len) {
        char m[96];
        snprintf(m, sizeof m, "copy of %lld bytes at %lld out of range (len %lld)",
                 src->len, at, b->len);
        beans_panic(m, line, col);
    }
    memcpy((char*)b->data + at, src->data, (size_t)src->len);
}
void beans_bytes_append(BList* b, BList* o) {
    bytes_grow(b, b->len + o->len);
    memcpy((char*)b->data + b->len, o->data, (size_t)o->len);
    b->len += o->len;
}
void beans_bytes_append_str(BList* b, char* s) {
    long long n = beans_slen(s);
    bytes_grow(b, b->len + n);
    memcpy((char*)b->data + b->len, s, (size_t)n);
    b->len += n;
}
__attribute__((always_inline)) void beans_bytes_append_i64(BList* b,
                                                            long long value) {
    bytes_grow(b, b->len + 8);
    unsigned long long bits = (unsigned long long)value;
    // Both supported targets are little-endian. One fixed-width store lets
    // LLVM keep len in a register across a chain of appends.
    memcpy((char*)b->data + b->len, &bits, 8);
    b->len += 8;
}
__attribute__((always_inline)) void beans_bytes_append_range(
    BList* b, BList* source, long long from, long long to,
    long long line, long long col) {
    if (from < 0 || to < from || to > source->len) {
        char m[96];
        snprintf(m, sizeof m, "slice %lld..%lld out of range (len %lld)", from, to,
                 source->len);
        beans_panic(m, line, col);
    }
    long long count = to - from;
    long long at = b->len;
    bytes_grow(b, at + count);
    memmove((char*)b->data + at, (char*)source->data + from, (size_t)count);
    b->len += count;
}
char* beans_bytes_to_string(BList* b) {
    long long n = 0;
    while (n < b->len && ((char*)b->data)[n] != 0) n++; // strings are text
    return str_make((char*)b->data, n);
}
char* beans_bytes_to_string_full(BList* b) {
    return str_make((char*)b->data, b->len);
}

// ---- files (kind 6 resources) -----------------------------------------------
// errno -> Error.kind slug; the interpreter builds the identical pair
static const char* fs_kind_of(int err) {
    switch (err) {
        case ENOENT: return "not_found";
        case EACCES:
        case EPERM: return "permission";
        case EEXIST: return "exists";
        case ENOTDIR: return "not_dir";
        case EISDIR: return "is_dir";
        case ENOTEMPTY: return "not_empty";
        default: return "io";
    }
}
static void* fs_err_obj(const char* path, int err) {
    size_t n = strlen(path) + 96;
    char* b = malloc(n);
    snprintf(b, n, "%s: %s", path, strerror(err));
    void* e = mk_error(b, fs_kind_of(err));
    free(b);
    return e;
}
// for paths that are beans strings: splice by stored length so a path with
// an embedded NUL reports the same bytes the interpreter does (the plain
// fs_err_obj above stays for internal C-built paths, which never hold NUL)
static void* fs_err_obj_rc(char* path, int err) {
    const char* es = strerror(err);
    long long lp = beans_slen(path);
    size_t le = strlen(es);
    long long total = lp + 2 + (long long)le;
    char* m = beans_alloc(total + 1, total << 3);
    memcpy(m, path, (size_t)lp);
    m[lp] = ':';
    m[lp + 1] = ' ';
    memcpy(m + lp + 2, es, le);
    return mk_error_own(m, fs_kind_of(err));
}
static void* op_err_obj(const char* op, int err) {
    char b[96];
    snprintf(b, sizeof b, "%s: %s", op, strerror(err));
    return mk_error(b, fs_kind_of(err));
}
static void* closed_err(void) { return mk_error("file is closed", "closed"); }

BRes beans_file_read_at(BFile* f, long long pos, long long n) {
    if (f->closed) return (BRes){0, closed_err()};
    if (pos < 0 || n < 0) return (BRes){0, mk_error("negative read", "io")};
    // clamp to what the file can actually give: a corrupted length field must
    // not become a giant allocation — the read comes back short anyway
    struct stat rst;
    if (fstat((int)f->fd, &rst) == 0 && S_ISREG(rst.st_mode)) {
        long long rem = rst.st_size > pos ? rst.st_size - pos : 0;
        if (n > rem) n = rem;
    }
    BList* buf = bytes_mk(n);
    long long got = 0;
    while (got < n) {
        ssize_t r = pread((int)f->fd, (char*)buf->data + got, (size_t)(n - got),
                          (off_t)(pos + got));
        if (r < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            beans_release(buf); // the error path must not leak the buffer
            return (BRes){0, op_err_obj("read", e)};
        }
        if (r == 0) break; // eof: a short read returns what's there
        got += r;
    }
    buf->len = got;
    return (BRes){(long long)buf, NULL};
}
BRes beans_file_write_at(BFile* f, long long pos, BList* d) {
    if (f->closed) return (BRes){0, closed_err()};
    if (pos < 0) return (BRes){0, mk_error("negative write", "io")};
    long long done = 0;
    while (done < d->len) {
        ssize_t r = pwrite((int)f->fd, (char*)d->data + done, (size_t)(d->len - done),
                           (off_t)(pos + done));
        if (r < 0) {
            if (errno == EINTR) continue;
            return (BRes){0, op_err_obj("write", errno)};
        }
        done += r;
    }
    return (BRes){done, NULL};
}
BRes beans_file_read(BFile* f, long long n) {
    if (f->closed) return (BRes){0, closed_err()};
    if (n < 0) return (BRes){0, mk_error("negative read", "io")};
    struct stat rst;
    if (fstat((int)f->fd, &rst) == 0 && S_ISREG(rst.st_mode)) {
        long long at = (long long)lseek((int)f->fd, 0, SEEK_CUR);
        long long rem = at >= 0 && rst.st_size > at ? rst.st_size - at : 0;
        if (n > rem) n = rem;
    }
    BList* buf = bytes_mk(n);
    long long got = 0;
    while (got < n) {
        ssize_t r = read((int)f->fd, (char*)buf->data + got, (size_t)(n - got));
        if (r < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            beans_release(buf); // the error path must not leak the buffer
            return (BRes){0, op_err_obj("read", e)};
        }
        if (r == 0) break;
        got += r;
    }
    buf->len = got;
    return (BRes){(long long)buf, NULL};
}
BRes beans_file_write(BFile* f, BList* d) {
    if (f->closed) return (BRes){0, closed_err()};
    long long done = 0;
    while (done < d->len) {
        ssize_t r = write((int)f->fd, (char*)d->data + done, (size_t)(d->len - done));
        if (r < 0) {
            if (errno == EINTR) continue;
            return (BRes){0, op_err_obj("write", errno)};
        }
        done += r;
    }
    return (BRes){done, NULL};
}
long long beans_file_seek(BFile* f, long long pos, long long line, long long col) {
    if (f->closed) beans_panic("file is closed", line, col);
    off_t r = lseek((int)f->fd, (off_t)pos, SEEK_SET);
    if (r < 0) {
        char m[96];
        snprintf(m, sizeof m, "seek to %lld: %s", pos, strerror(errno));
        beans_panic(m, line, col);
    }
    return (long long)r;
}
long long beans_file_seek_end(BFile* f, long long off, long long line, long long col) {
    if (f->closed) beans_panic("file is closed", line, col);
    off_t r = lseek((int)f->fd, (off_t)off, SEEK_END);
    if (r < 0) {
        char m[96];
        snprintf(m, sizeof m, "seek to %lld: %s", off, strerror(errno));
        beans_panic(m, line, col);
    }
    return (long long)r;
}
long long beans_file_tell(BFile* f, long long line, long long col) {
    if (f->closed) beans_panic("file is closed", line, col);
    return (long long)lseek((int)f->fd, 0, SEEK_CUR);
}
BRes beans_file_size(BFile* f) {
    if (f->closed) return (BRes){0, closed_err()};
    struct stat st;
    if (fstat((int)f->fd, &st) != 0) return (BRes){0, op_err_obj("size", errno)};
    return (BRes){(long long)st.st_size, NULL};
}
BRes beans_file_truncate(BFile* f, long long n) {
    if (f->closed) return (BRes){0, closed_err()};
    if (ftruncate((int)f->fd, (off_t)n) != 0) {
        return (BRes){0, op_err_obj("truncate", errno)};
    }
    return (BRes){1, NULL};
}
BRes beans_file_sync(BFile* f) {
    if (f->closed) return (BRes){0, closed_err()};
    if (fsync((int)f->fd) != 0) return (BRes){0, op_err_obj("sync", errno)};
    return (BRes){1, NULL};
}
BRes beans_file_close(BFile* f) {
    if (f->closed) return (BRes){0, mk_error("file already closed", "closed")};
    f->closed = 1;
    // While worker threads are live, defer the real close(): a racing op on
    // another thread could still be mid-syscall on this fd, and reusing the
    // number for a freshly-opened file would silently corrupt it. The fd stays
    // open (harmless — same file) until the handle's last ref drops in
    // cc_free_shell, when no thread can hold it. This mirrors the collector's
    // own "don't touch shared resources while mutators run" gate. Zero cost
    // single-threaded, where cc_threads is 0 and the fd closes now.
    if (cc_threads > 0) return (BRes){1, NULL};
    long long fd = f->fd;
    f->fd = -1;
    if (close((int)fd) != 0) return (BRes){0, op_err_obj("close", errno)};
    return (BRes){1, NULL};
}

// advisory flock — single-writer databases; try_lock's ok(false) means "held
// by someone else", every other failure is a real error
BRes beans_file_lock(BFile* f) {
    if (f->closed) return (BRes){0, closed_err()};
    // a blocking lock is the classic EINTR victim — retry rather than fail
    while (flock((int)f->fd, LOCK_EX) != 0) {
        if (errno == EINTR) continue;
        return (BRes){0, op_err_obj("lock", errno)};
    }
    return (BRes){1, NULL};
}
BRes beans_file_try_lock(BFile* f) {
    if (f->closed) return (BRes){0, closed_err()};
    if (flock((int)f->fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK) return (BRes){0, NULL};
        return (BRes){0, op_err_obj("try_lock", errno)};
    }
    return (BRes){1, NULL};
}
BRes beans_file_unlock(BFile* f) {
    if (f->closed) return (BRes){0, closed_err()};
    if (flock((int)f->fd, LOCK_UN) != 0) return (BRes){0, op_err_obj("unlock", errno)};
    return (BRes){1, NULL};
}

// ---- mmap (kind 6, shape bit 0 = 1) ----
// whole-file shared mapping; the fd is kept open so resize() can ftruncate +
// remap. get/put/read/write panic on a closed or short map, flush/close
// report errors as Results like File does.
static void* mmap_closed_err(void) { return mk_error("mmap is closed", "closed"); }
BRes beans_mmap_open(char* path, long long writable) {
    int fd = open(path, writable ? O_RDWR : O_RDONLY);
    if (fd < 0) return (BRes){0, fs_err_obj_rc(path, errno)};
    struct stat st;
    if (fstat(fd, &st) != 0) {
        int e = errno;
        close(fd);
        return (BRes){0, fs_err_obj_rc(path, e)};
    }
    char* p = NULL;
    if (st.st_size > 0) {
        p = mmap(NULL, (size_t)st.st_size,
                 writable ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            int e = errno;
            close(fd);
            return (BRes){0, fs_err_obj_rc(path, e)};
        }
    }
    BMMap* m = beans_alloc(sizeof(BMMap), 6 | (1 << 3));
    m->p = p;
    m->len = (long long)st.st_size;
    m->fd = fd; // kept: resize() needs it to ftruncate + remap
    m->writable = writable;
    return (BRes){(long long)m, NULL};
}
long long beans_mmap_len(BMMap* m) { return m->len; }
static void mmap_guard(BMMap* m, long long line, long long col) {
    if (m->closed) beans_panic("mmap is closed", line, col);
}
static long long mmap_word(BMMap* m, const char* what, long long pos, long long w,
                           long long line, long long col) {
    mmap_guard(m, line, col);
    // pos > len - w, never pos + w > len — the sum overflows for huge pos
    if (pos < 0 || w > m->len || pos > m->len - w) {
        char b[96];
        snprintf(b, sizeof b, "%s read at %lld out of range (len %lld)", what, pos,
                 m->len);
        beans_panic(b, line, col);
    }
    unsigned long long v = 0;
    memcpy(&v, m->p + pos, (size_t)w);
    return (long long)v;
}
static void mmap_put_word(BMMap* m, const char* what, long long pos, long long v,
                          long long w, long long line, long long col) {
    mmap_guard(m, line, col);
    if (!m->writable) beans_panic("mmap is read-only", line, col);
    if (pos < 0 || w > m->len || pos > m->len - w) {
        char b[96];
        snprintf(b, sizeof b, "%s write at %lld out of range (len %lld)", what, pos,
                 m->len);
        beans_panic(b, line, col);
    }
    memcpy(m->p + pos, &v, (size_t)w);
}
long long beans_mmap_get_u8(BMMap* m, long long p, long long l, long long c) { return mmap_word(m, "u8", p, 1, l, c); }
long long beans_mmap_get_u16(BMMap* m, long long p, long long l, long long c) { return mmap_word(m, "u16", p, 2, l, c); }
long long beans_mmap_get_u32(BMMap* m, long long p, long long l, long long c) { return mmap_word(m, "u32", p, 4, l, c); }
long long beans_mmap_get_u64(BMMap* m, long long p, long long l, long long c) { return mmap_word(m, "u64", p, 8, l, c); }
long long beans_mmap_get_i64(BMMap* m, long long p, long long l, long long c) { return mmap_word(m, "i64", p, 8, l, c); }
void beans_mmap_put_u8(BMMap* m, long long p, long long v, long long l, long long c) { mmap_put_word(m, "u8", p, v, 1, l, c); }
void beans_mmap_put_u16(BMMap* m, long long p, long long v, long long l, long long c) { mmap_put_word(m, "u16", p, v, 2, l, c); }
void beans_mmap_put_u32(BMMap* m, long long p, long long v, long long l, long long c) { mmap_put_word(m, "u32", p, v, 4, l, c); }
void beans_mmap_put_u64(BMMap* m, long long p, long long v, long long l, long long c) { mmap_put_word(m, "u64", p, v, 8, l, c); }
void beans_mmap_put_i64(BMMap* m, long long p, long long v, long long l, long long c) { mmap_put_word(m, "i64", p, v, 8, l, c); }
BList* beans_mmap_read(BMMap* m, long long pos, long long n, long long line,
                       long long col) {
    mmap_guard(m, line, col);
    if (pos < 0 || n < 0 || n > m->len || pos > m->len - n) {
        char b[96];
        snprintf(b, sizeof b, "read %lld at %lld out of range (len %lld)", n, pos,
                 m->len);
        beans_panic(b, line, col);
    }
    BList* r = bytes_mk(n);
    memcpy(r->data, m->p + pos, (size_t)n);
    return r;
}
void beans_mmap_write(BMMap* m, long long pos, BList* d, long long line,
                      long long col) {
    mmap_guard(m, line, col);
    if (!m->writable) beans_panic("mmap is read-only", line, col);
    if (pos < 0 || d->len > m->len || pos > m->len - d->len) {
        char b[96];
        snprintf(b, sizeof b, "write %lld at %lld out of range (len %lld)", d->len,
                 pos, m->len);
        beans_panic(b, line, col);
    }
    memcpy(m->p + pos, d->data, (size_t)d->len);
}
BRes beans_mmap_flush(BMMap* m) {
    if (m->closed) return (BRes){0, mmap_closed_err()};
    if (m->len > 0 && msync(m->p, (size_t)m->len, MS_SYNC) != 0) {
        return (BRes){0, op_err_obj("flush", errno)};
    }
    return (BRes){1, NULL};
}
BRes beans_mmap_flush_range(BMMap* m, long long pos, long long n) {
    if (m->closed) return (BRes){0, mmap_closed_err()};
    if (pos < 0 || n < 0 || n > m->len || pos > m->len - n) {
        char b[96];
        snprintf(b, sizeof b, "flush %lld at %lld out of range (len %lld)", n, pos,
                 m->len);
        return (BRes){0, mk_error(b, "io")};
    }
    if (n > 0) {
        long long page = (long long)getpagesize();
        long long start = pos - pos % page; // msync wants a page-aligned base
        if (msync(m->p + start, (size_t)(pos + n - start), MS_SYNC) != 0) {
            return (BRes){0, op_err_obj("flush", errno)};
        }
    }
    return (BRes){1, NULL};
}
BRes beans_mmap_close(BMMap* m) {
    if (m->closed) return (BRes){0, mk_error("mmap already closed", "closed")};
    m->closed = 1;
    // defer munmap+close while workers run (see beans_file_close): a racing op
    // reading through the mapping must not have it pulled out from under it
    if (cc_threads > 0) return (BRes){1, NULL};
    int bad = m->p && munmap(m->p, (size_t)m->len) != 0;
    int e = errno;
    m->p = NULL;
    if (m->fd >= 0) close((int)m->fd);
    m->fd = -1;
    if (bad) return (BRes){0, op_err_obj("close", e)};
    return (BRes){1, NULL};
}

// grow or shrink in place: truncate the file, drop the old mapping, map
// fresh. On a mapping failure the handle stays open but empty (len 0).
BRes beans_mmap_resize(BMMap* m, long long n) {
    if (m->closed) return (BRes){0, mmap_closed_err()};
    if (!m->writable) return (BRes){0, mk_error("mmap is read-only", "permission")};
    if (n < 0) return (BRes){0, mk_error("negative resize", "io")};
    if (ftruncate((int)m->fd, (off_t)n) != 0) {
        return (BRes){0, op_err_obj("resize", errno)};
    }
    if (m->p) munmap(m->p, (size_t)m->len);
    m->p = NULL;
    m->len = 0;
    if (n > 0) {
        char* p = mmap(NULL, (size_t)n, PROT_READ | PROT_WRITE, MAP_SHARED,
                       (int)m->fd, 0);
        if (p == MAP_FAILED) return (BRes){0, op_err_obj("resize", errno)};
        m->p = p;
        m->len = n;
    }
    return (BRes){1, NULL};
}

// ---- Dir.walk: files and symlinks under root (lstat — never follows a
// link), paths relative to root, "/"-joined, sorted at the end ----
typedef struct {
    char** v;
    long long len, cap;
} StrVec;
static void sv_push(StrVec* s, char* p) {
    if (s->len == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->v = realloc(s->v, (size_t)s->cap * sizeof(char*));
    }
    s->v[s->len++] = p;
}
static char* path_cat(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    char* r = malloc(la + lb + 2);
    memcpy(r, a, la);
    r[la] = '/';
    memcpy(r + la + 1, b, lb);
    r[la + 1 + lb] = 0;
    return r;
}
static void sv_free(StrVec* s) {
    for (long long i = 0; i < s->len; i++) free(s->v[i]);
    free(s->v);
}
// Iterative walk: an explicit stack of relative dir paths, one DIR open at a
// time. Recursion held one fd per depth level and ran out at ~250 deep; this
// caps open fds at one. Output is sorted afterward, so traversal order is
// free.
static int walk_dir(const char* root, StrVec* out, char** epath, int* eno) {
    StrVec stack = {0, 0, 0};
    sv_push(&stack, strdup("")); // "" = root itself
    int ok = 1;
    while (stack.len > 0) {
        char* rel = stack.v[--stack.len];
        char* full = rel[0] ? path_cat(root, rel) : strdup(root);
        DIR* d = opendir(full);
        if (!d) {
            *epath = full;
            *eno = errno;
            free(rel);
            ok = 0;
            break;
        }
        free(full);
        struct dirent* en;
        while ((en = readdir(d))) {
            if (strcmp(en->d_name, ".") == 0 || strcmp(en->d_name, "..") == 0) continue;
            char* r2 = rel[0] ? path_cat(rel, en->d_name) : strdup(en->d_name);
            char* abs = path_cat(root, r2);
            struct stat st;
            if (lstat(abs, &st) != 0) {
                *epath = abs;
                *eno = errno;
                free(r2);
                ok = 0;
                break;
            }
            free(abs);
            if (S_ISDIR(st.st_mode)) sv_push(&stack, r2); // descend later
            else sv_push(out, r2);
        }
        closedir(d);
        free(rel);
        if (!ok) break;
    }
    for (long long i = 0; i < stack.len; i++) free(stack.v[i]);
    free(stack.v);
    return ok;
}
static int walk_cmp(const void* a, const void* b) {
    return strcmp(*(char* const*)a, *(char* const*)b);
}
BRes beans_dir_walk(char* path) {
    StrVec out = {0, 0, 0};
    char* epath = NULL;
    int eno = 0;
    if (!walk_dir(path, &out, &epath, &eno)) {
        void* e = fs_err_obj(epath, eno);
        free(epath);
        for (long long i = 0; i < out.len; i++) free(out.v[i]);
        free(out.v);
        return (BRes){0, e};
    }
    qsort(out.v, (size_t)out.len, sizeof(char*), walk_cmp);
    BList* l = beans_list_new(1);
    for (long long i = 0; i < out.len; i++) {
        beans_list_push(l, (long long)rc_strdup(out.v[i]));
        free(out.v[i]);
    }
    free(out.v);
    return (BRes){(long long)l, NULL};
}

BRes beans_file_open(char* path, char* mode) {
    int flags;
    if (strcmp(mode, "r") == 0) flags = O_RDONLY;
    else if (strcmp(mode, "rw") == 0) flags = O_RDWR;
    else if (strcmp(mode, "create") == 0) flags = O_RDWR | O_CREAT;
    else if (strcmp(mode, "append") == 0) flags = O_WRONLY | O_CREAT | O_APPEND;
    else {
        // mode is a beans string of any length — heap-build so a long bad
        // mode reports its full text like the interpreter, not a truncation
        size_t n = strlen(mode) + 24;
        char* m = malloc(n);
        snprintf(m, n, "bad open mode '%s'", mode);
        void* e = mk_error(m, "io");
        free(m);
        return (BRes){0, e};
    }
    int fd = open(path, flags, 0644);
    if (fd < 0) return (BRes){0, fs_err_obj_rc(path, errno)};
    BFile* f = beans_alloc(sizeof(BFile), 6);
    f->fd = fd;
    f->closed = 0;
    return (BRes){(long long)f, NULL};
}

long long beans_file_exists(char* path) {
    struct stat st;
    return stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
}
BRes beans_file_size_p(char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return (BRes){0, fs_err_obj_rc(path, errno)};
    return (BRes){(long long)st.st_size, NULL};
}
BRes beans_file_remove(char* path) {
    struct stat st;
    // lstat: remove the link itself, and let a dangling symlink be removed
    if (lstat(path, &st) != 0) return (BRes){0, fs_err_obj_rc(path, errno)};
    int r = S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path);
    if (r != 0) return (BRes){0, fs_err_obj_rc(path, errno)};
    return (BRes){1, NULL};
}
BRes beans_file_rename(char* from, char* to) {
    if (rename(from, to) != 0) return (BRes){0, fs_err_obj_rc(from, errno)};
    return (BRes){1, NULL};
}
BRes beans_dir_make(char* path) {
    if (mkdir(path, 0755) != 0) return (BRes){0, fs_err_obj_rc(path, errno)};
    return (BRes){1, NULL};
}
BRes beans_dir_make_all(char* path) {
    long long n = beans_slen(path);
    char* cur = malloc((size_t)n + 1);
    long long i = 0;
    while (i < n) {
        long long j = i;
        while (j < n && path[j] != '/') j++;
        memcpy(cur, path, (size_t)j);
        cur[j] = 0;
        i = j + 1;
        if (cur[0] == 0) continue;
        if (mkdir(cur, 0755) != 0) {
            int me = errno;
            // EEXIST is only ok if the existing entry is a directory
            struct stat st;
            if (me != EEXIST || stat(cur, &st) != 0 || !S_ISDIR(st.st_mode)) {
                BRes r = {0, fs_err_obj(cur, me == EEXIST ? ENOTDIR : me)};
                free(cur);
                return r;
            }
        }
    }
    free(cur);
    return (BRes){1, NULL};
}
static int name_cmp(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}
BRes beans_dir_list(char* path) {
    DIR* d = opendir(path);
    if (!d) return (BRes){0, fs_err_obj_rc(path, errno)};
    char** names = NULL;
    long long cnt = 0, cap = 0;
    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (cnt == cap) {
            cap = cap ? cap * 2 : 16;
            names = realloc(names, (size_t)cap * sizeof(char*));
        }
        names[cnt++] = strdup(de->d_name);
    }
    closedir(d);
    qsort(names, (size_t)cnt, sizeof(char*), name_cmp); // deterministic
    BList* l = beans_list_new(1);
    for (long long i = 0; i < cnt; i++) {
        beans_list_push(l, (long long)rc_strdup(names[i]));
        free(names[i]);
    }
    free(names);
    return (BRes){(long long)l, NULL};
}
BRes beans_dir_remove(char* path) {
    if (rmdir(path) != 0) return (BRes){0, fs_err_obj_rc(path, errno)};
    return (BRes){1, NULL};
}
// Iterative delete: gather the tree pre-order (parent before child) with one
// DIR open at a time, then remove in reverse (deepest first). Recursion held
// one fd per level and a fixed 1024-byte path buffer that silently truncated
// deep paths; this heap-builds every path and caps open fds at one.
static int rm_tree(const char* p) {
    struct stat st;
    if (lstat(p, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(p);
    StrVec dirs = {0, 0, 0}; // dirs to rmdir, in discovery (pre) order
    StrVec stack = {0, 0, 0}; // dirs still to scan
    sv_push(&stack, strdup(p));
    int ok = 1;
    while (stack.len > 0) {
        char* dir = stack.v[--stack.len];
        DIR* d = opendir(dir);
        if (!d) {
            free(dir);
            ok = 0;
            break;
        }
        struct dirent* de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            char* sub = path_cat(dir, de->d_name);
            struct stat cst;
            if (lstat(sub, &cst) != 0) {
                free(sub);
                ok = 0;
                break;
            }
            if (S_ISDIR(cst.st_mode)) sv_push(&stack, sub); // scan later
            else {
                if (unlink(sub) != 0) ok = 0;
                free(sub);
                if (!ok) break;
            }
        }
        closedir(d);
        sv_push(&dirs, dir); // remove this dir after its children
        if (!ok) break;
    }
    for (long long i = 0; i < stack.len; i++) free(stack.v[i]);
    free(stack.v);
    // deepest first: dirs were collected parent-before-child, so reverse
    for (long long i = dirs.len; ok && i-- > 0;) {
        if (rmdir(dirs.v[i]) != 0) ok = 0;
    }
    sv_free(&dirs);
    return ok ? 0 : -1;
}
BRes beans_dir_remove_all(char* path) {
    struct stat st;
    if (lstat(path, &st) != 0) return (BRes){0, fs_err_obj_rc(path, errno)};
    if (rm_tree(path) != 0) return (BRes){0, fs_err_obj_rc(path, errno)};
    return (BRes){1, NULL};
}
long long beans_dir_exists(char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
char* beans_dir_temp(void) {
    const char* t = getenv("TMPDIR");
    const char* src = t && *t ? t : "/tmp";
    long long n = (long long)strlen(src);
    while (n > 1 && src[n - 1] == '/') n--; // trim trailing slashes
    return str_make(src, n);
}
BRes beans_dir_sync(char* path) {
    // the database commit pattern: fsync the directory after a rename
    int fd = open(path, O_RDONLY);
    if (fd < 0) return (BRes){0, fs_err_obj_rc(path, errno)};
    if (fsync(fd) != 0) {
        int e = errno;
        close(fd);
        return (BRes){0, fs_err_obj_rc(path, e)};
    }
    close(fd);
    return (BRes){1, NULL};
}

// ---- std.os / std.io --------------------------------------------------------
static int os_argc;
static char** os_argv;
__attribute__((constructor)) static void os_capture(int argc, char** argv) {
    os_argc = argc;
    os_argv = argv;
}
BList* beans_os_args(void) {
    BList* l = beans_list_new(1);
    for (int i = 1; i < os_argc; i++) {
        beans_list_push(l, (long long)rc_strdup(os_argv[i]));
    }
    return l;
}
BOpt beans_os_env(char* name) {
    const char* v = getenv(name);
    if (!v) return (BOpt){0, 0};
    return (BOpt){(long long)rc_strdup(v), 1};
}
void beans_os_exit(long long code) { exit((int)code); }
long long beans_os_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
long long beans_os_ticks_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
void beans_os_sleep_ms(long long ms) {
    if (ms > 0) {
        struct timespec ts;
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (ms % 1000) * 1000000;
        nanosleep(&ts, NULL);
    }
}
BOpt beans_io_read_line(void) {
    char* out = NULL;
    long long len = 0, cap = 0;
    int c;
    int any = 0;
    while ((c = fgetc(stdin)) != EOF) {
        any = 1;
        if (c == '\n') break;
        if (len == cap) {
            cap = cap ? cap * 2 : 128;
            out = realloc(out, (size_t)cap);
        }
        out[len++] = (char)c;
    }
    if (!any) {
        free(out);
        return (BOpt){0, 0};
    }
    char* s = str_make(out ? out : "", len);
    free(out);
    return (BOpt){(long long)s, 1};
}
char* beans_io_read_all(void) {
    char* out = NULL;
    long long len = 0, cap = 0;
    char chunk[65536];
    size_t r;
    while ((r = fread(chunk, 1, sizeof chunk, stdin)) > 0) {
        if (len + (long long)r > cap) {
            cap = cap ? cap * 2 : 65536;
            while (cap < len + (long long)r) cap *= 2;
            out = realloc(out, (size_t)cap);
        }
        memcpy(out + len, chunk, r);
        len += r;
    }
    char* s = str_make(out ? out : "", len);
    free(out);
    return s;
}
void beans_eprintln(char* s) {
    fwrite(s, 1, (size_t)beans_slen(s), stderr);
    fputc('\n', stderr);
}
void beans_eprint(char* s) { fwrite(s, 1, (size_t)beans_slen(s), stderr); }

// ---- threads ----
typedef struct {
    void* payload;
    long long result;
    pthread_t th;
    long long (*thunk)(void*);
    void (*typed_thunk)(void*, void*);
    void* env;
    long long result_size;
    int joined;
} BThread;
void beans_thread_release_env(void* env);
static void* thread_main(void* arg) {
    BThread* t = arg;
    if (t->typed_thunk) t->typed_thunk(t->env, t->payload);
    else t->result = t->thunk(t->env);
    beans_release(t->env);
    beans_release(t); // the running thread's own ref on the handle
    // last heap touch is done — the cycle collector may run again
    cc_threads -= 1;
    return NULL;
}
BThread* beans_thread_spawn(void* thunk, void* env, long long result_ptr) {
    BThread* t = beans_alloc(sizeof(BThread), 1 | (result_ptr << 4));
    t->thunk = (long long (*)(void*))thunk;
    t->env = env; // ownership of the closure box moves to the thread
    cc_enable_mt(); // from here every count op is atomic, in every thread
    beans_retain(t); // one ref for the handle, one for the running thread
    cc_threads += 1;
    pthread_create(&t->th, NULL, thread_main, t);
    return t;
}
BThread* beans_thread_spawn_typed(void* thunk, void* env, long long size,
                                  long long ptr_mask) {
    if (size <= 0 || size > (1LL << 30))
        beans_panic("invalid thread result size", 0, 0);
    BThread* t = beans_alloc(sizeof(BThread), 1 | (1LL << 3));
    t->payload = beans_alloc(size, 1 | (ptr_mask << 3));
    t->result_size = size;
    t->typed_thunk = (void (*)(void*, void*))thunk;
    t->env = env;
    cc_enable_mt();
    beans_retain(t);
    cc_threads += 1;
    pthread_create(&t->th, NULL, thread_main, t);
    return t;
}
long long beans_thread_join(BThread* t) {
    if (t->joined) beans_panic("thread already joined", 0, 0);
    t->joined = 1;
    pthread_join(t->th, NULL);
    long long result = t->result;
    t->result = 0; // ownership of a pointer result moves to the caller
    return result;
}
void beans_thread_join_typed(BThread* t, void* out, long long size) {
    if (t->joined) beans_panic("thread already joined", 0, 0);
    t->joined = 1;
    pthread_join(t->th, NULL);
    if (size != t->result_size) beans_panic("thread result size mismatch", 0, 0);
    void* payload = t->payload;
    memcpy(out, payload, (size_t)size);
    memset(payload, 0, (size_t)size); // move nested refs out of the payload box
    t->payload = NULL;
    beans_release(payload);
}

BMutex* beans_mutex_new(long long inner, long long inner_ptr) {
    BMutex* mu = beans_alloc(sizeof(BMutex), 5 | (inner_ptr << 3));
    pthread_mutex_init(&mu->m, NULL);
    mu->inner = inner;
    return mu;
}
long long beans_mutex_lock(BMutex* mu) {
    pthread_mutex_lock(&mu->m);
    return mu->inner;
}
BMutex* beans_mutex_new_typed(void* value, long long size, long long ptr_mask) {
    if (size <= 0 || size > (1LL << 30))
        beans_panic("invalid mutex value size", 0, 0);
    void* payload = beans_alloc(size, 1 | (ptr_mask << 3));
    memcpy(payload, value, (size_t)size);
    return beans_mutex_new((long long)payload, 1);
}
void beans_mutex_lock_typed(BMutex* mu, void* out, long long size) {
    pthread_mutex_lock(&mu->m);
    memcpy(out, (void*)mu->inner, (size_t)size);
}
void beans_mutex_unlock(BMutex* mu) { pthread_mutex_unlock(&mu->m); }

BChan* beans_chan_new(long long cap, long long elem_ptr) {
    BChan* c = beans_alloc(sizeof(BChan), 4 | (elem_ptr << 3));
    c->cap = cap > 0 ? cap : 1;
    c->stride = 8;
    c->ptr_mask = elem_ptr;
    c->q = calloc((size_t)c->cap, 8);
    if (!c->q) beans_panic("out of memory", 0, 0);
    pthread_mutex_init(&c->m, NULL);
    pthread_cond_init(&c->can_send, NULL);
    pthread_cond_init(&c->can_recv, NULL);
    return c;
}
BChan* beans_chan_new_typed(long long cap, long long stride, long long ptr_mask) {
    if (stride <= 0 || stride > (1LL << 30))
        beans_panic("invalid channel element size", 0, 0);
    long long capacity = cap > 0 ? cap : 1;
    if (capacity > (1LL << 58) / stride)
        beans_panic("channel capacity too large", 0, 0);
    BChan* c = beans_alloc(sizeof(BChan), 4 | ((ptr_mask != 0) << 3));
    c->cap = capacity;
    c->stride = stride;
    c->ptr_mask = ptr_mask;
    c->q = calloc((size_t)capacity, (size_t)stride);
    if (!c->q) beans_panic("out of memory", 0, 0);
    pthread_mutex_init(&c->m, NULL);
    pthread_cond_init(&c->can_send, NULL);
    pthread_cond_init(&c->can_recv, NULL);
    return c;
}
long long beans_chan_send(BChan* c, long long v) {
    pthread_mutex_lock(&c->m);
    while (c->count == c->cap && !c->closed) pthread_cond_wait(&c->can_send, &c->m);
    if (c->closed) {
        pthread_mutex_unlock(&c->m);
        return 0; // caller panics; caller also still owns v
    }
    c->q[(c->head + c->count) % c->cap] = v;
    c->count += 1;
    pthread_cond_signal(&c->can_recv);
    pthread_mutex_unlock(&c->m);
    return 1;
}
long long beans_chan_send_typed(BChan* c, void* value) {
    pthread_mutex_lock(&c->m);
    while (c->count == c->cap && !c->closed) pthread_cond_wait(&c->can_send, &c->m);
    if (c->closed) {
        pthread_mutex_unlock(&c->m);
        return 0;
    }
    void* destination =
        (char*)c->q + ((c->head + c->count) % c->cap) * c->stride;
    memcpy(destination, value, (size_t)c->stride);
    c->count += 1;
    pthread_cond_signal(&c->can_recv);
    pthread_mutex_unlock(&c->m);
    return 1;
}
long long beans_chan_recv(BChan* c, long long* ok) {
    pthread_mutex_lock(&c->m);
    while (c->count == 0 && !c->closed) pthread_cond_wait(&c->can_recv, &c->m);
    if (c->count == 0) {
        *ok = 0;
        pthread_mutex_unlock(&c->m);
        return 0;
    }
    long long v = c->q[c->head];
    c->head = (c->head + 1) % c->cap;
    c->count -= 1;
    *ok = 1;
    pthread_cond_signal(&c->can_send);
    pthread_mutex_unlock(&c->m);
    return v;
}
long long beans_chan_recv_typed(BChan* c, void* out) {
    pthread_mutex_lock(&c->m);
    while (c->count == 0 && !c->closed) pthread_cond_wait(&c->can_recv, &c->m);
    if (c->count == 0) {
        pthread_mutex_unlock(&c->m);
        return 0;
    }
    void* source = (char*)c->q + c->head * c->stride;
    memcpy(out, source, (size_t)c->stride);
    memset(source, 0, (size_t)c->stride);
    c->head = (c->head + 1) % c->cap;
    c->count -= 1;
    pthread_cond_signal(&c->can_send);
    pthread_mutex_unlock(&c->m);
    return 1;
}
void beans_chan_close(BChan* c) {
    pthread_mutex_lock(&c->m);
    c->closed = 1;
    pthread_cond_broadcast(&c->can_send);
    pthread_cond_broadcast(&c->can_recv);
    pthread_mutex_unlock(&c->m);
}

typedef struct { _Atomic long long v; } BAtomic;
BAtomic* beans_atomic_new(long long init) {
    BAtomic* a = beans_alloc(sizeof(BAtomic), 0);
    a->v = init;
    return a;
}
long long beans_atomic_add(BAtomic* a, long long d) { return (a->v += d); }
long long beans_atomic_get(BAtomic* a) { return a->v; }
void beans_atomic_set(BAtomic* a, long long v) { a->v = v; }

// ---- decimal: 128-bit coefficient + base-10 scale (same math as the interpreter) ----
typedef struct BDec {
    __int128 c;
    long long s;
} BDec;
typedef unsigned __int128 BDecV;
#define BDEC_COEFF_BITS 112
static BDecV decv_coeff_mask(void) {
    return (((BDecV)1) << BDEC_COEFF_BITS) - 1;
}
static __int128 decv_coeff(BDecV v) {
    BDecV mask = decv_coeff_mask();
    BDecV raw = v & mask;
    if (raw & (((BDecV)1) << (BDEC_COEFF_BITS - 1))) raw |= ~mask;
    return (__int128)raw;
}
static long long decv_scale(BDecV v) {
    return (long long)(v >> BDEC_COEFF_BITS);
}
static BDecV decv_mk(__int128 c, long long s) {
    const __int128 limit = ((__int128)1) << (BDEC_COEFF_BITS - 1);
    if (s < 0 || s > 65535 || c < -limit || c >= limit)
        beans_panic("decimal overflow", 0, 0);
    return (((BDecV)(unsigned long long)s) << BDEC_COEFF_BITS) |
           ((BDecV)c & decv_coeff_mask());
}
static BDec* dec_mk(__int128 c, long long s) {
    BDec* d = beans_alloc(sizeof(BDec), 0);
    d->c = c;
    d->s = s;
    return d;
}
static __int128 pow10i(long long n) {
    __int128 p = 1;
    for (long long i = 0; i < n; i++) p *= 10;
    return p;
}
BDec* beans_dec_new(__int128 c, long long s) { return dec_mk(c, s); }
BDec* beans_dec_from_int(long long v) { return dec_mk((__int128)v, 0); }
BDec* beans_decv_box(BDecV v) { return dec_mk(decv_coeff(v), decv_scale(v)); }
BDecV beans_decv_unbox(BDec* v) { return decv_mk(v->c, v->s); }
BDecV beans_decv_from_int(long long v) { return decv_mk((__int128)v, 0); }
static void dec_align(BDec* a, BDec* b, __int128* ca, __int128* cb, long long* s) {
    *s = a->s > b->s ? a->s : b->s;
    *ca = a->c * pow10i(*s - a->s);
    *cb = b->c * pow10i(*s - b->s);
}
static void decv_align(BDecV a, BDecV b, __int128* ca, __int128* cb,
                       long long* s) {
    long long as = decv_scale(a), bs = decv_scale(b);
    *s = as > bs ? as : bs;
    *ca = decv_coeff(a) * pow10i(*s - as);
    *cb = decv_coeff(b) * pow10i(*s - bs);
}
__attribute__((always_inline)) BDecV beans_decv_add(BDecV a, BDecV b) {
    if (decv_scale(a) == decv_scale(b))
        return decv_mk(decv_coeff(a) + decv_coeff(b), decv_scale(a));
    __int128 ca, cb;
    long long s;
    decv_align(a, b, &ca, &cb, &s);
    return decv_mk(ca + cb, s);
}
__attribute__((always_inline)) BDecV beans_decv_sub(BDecV a, BDecV b) {
    if (decv_scale(a) == decv_scale(b))
        return decv_mk(decv_coeff(a) - decv_coeff(b), decv_scale(a));
    __int128 ca, cb;
    long long s;
    decv_align(a, b, &ca, &cb, &s);
    return decv_mk(ca - cb, s);
}
BDecV beans_decv_mul(BDecV a, BDecV b) {
    return decv_mk(decv_coeff(a) * decv_coeff(b),
                   decv_scale(a) + decv_scale(b));
}
BDecV beans_decv_div(BDecV a, BDecV b, long long ln, long long cl) {
    __int128 bc = decv_coeff(b);
    if (bc == 0) beans_panic("divide by zero", ln, cl);
    long long extra = 20;
    __int128 q = decv_coeff(a) * pow10i(extra + decv_scale(b)) / bc;
    long long s = decv_scale(a) + extra;
    while (s > 0 && q % 10 == 0) {
        q /= 10;
        s -= 1;
    }
    return decv_mk(q, s);
}
BDecV beans_decv_neg(BDecV a) {
    return decv_mk(-decv_coeff(a), decv_scale(a));
}
BDecV beans_decv_abs(BDecV a) {
    __int128 c = decv_coeff(a);
    return decv_mk(c < 0 ? -c : c, decv_scale(a));
}
BDecV beans_decv_round(BDecV a, long long places) {
    __int128 c = decv_coeff(a);
    long long s = decv_scale(a);
    if (places >= s) return a;
    __int128 f = pow10i(s - places);
    __int128 q = c / f, rem = c % f;
    if (rem < 0) rem = -rem;
    if (rem * 2 >= f) q += c >= 0 ? 1 : -1;
    if (places < 0) return decv_mk(q * pow10i(-places), 0);
    return decv_mk(q, places);
}
int beans_decv_cmp(BDecV a, BDecV b) {
    __int128 ca, cb;
    long long s;
    decv_align(a, b, &ca, &cb, &s);
    return ca < cb ? -1 : ca > cb ? 1 : 0;
}
long long beans_decv_hash(BDecV value) {
    __int128 c = decv_coeff(value);
    long long s = decv_scale(value);
    while (s > 0 && c % 10 == 0) {
        c /= 10;
        s -= 1;
    }
    unsigned long long lo = (unsigned long long)(unsigned __int128)c;
    unsigned long long hi = (unsigned long long)((unsigned __int128)c >> 64);
    return (long long)beans_mix64(lo ^ beans_mix64(hi ^ (unsigned long long)s));
}
BDec* beans_dec_add(BDec* a, BDec* b) {
    __int128 ca, cb;
    long long s;
    dec_align(a, b, &ca, &cb, &s);
    return dec_mk(ca + cb, s);
}
BDec* beans_dec_sub(BDec* a, BDec* b) {
    __int128 ca, cb;
    long long s;
    dec_align(a, b, &ca, &cb, &s);
    return dec_mk(ca - cb, s);
}
BDec* beans_dec_mul(BDec* a, BDec* b) { return dec_mk(a->c * b->c, a->s + b->s); }
BDec* beans_dec_div(BDec* a, BDec* b, long long ln, long long cl) {
    if (b->c == 0) beans_panic("divide by zero", ln, cl);
    long long extra = 20;
    __int128 num = a->c * pow10i(extra + b->s);
    __int128 q = num / b->c;
    long long s = a->s + extra;
    while (s > 0 && q % 10 == 0) {
        q /= 10;
        s -= 1;
    }
    return dec_mk(q, s);
}
BDec* beans_dec_neg(BDec* a) { return dec_mk(-a->c, a->s); }
BDec* beans_dec_abs(BDec* a) { return dec_mk(a->c < 0 ? -a->c : a->c, a->s); }
BDec* beans_dec_round(BDec* a, long long places) {
    if (places >= a->s) return dec_mk(a->c, a->s);
    __int128 f = pow10i(a->s - places);
    __int128 q = a->c / f, rem = a->c % f;
    if (rem < 0) rem = -rem;
    if (rem * 2 >= f) q += a->c >= 0 ? 1 : -1;
    // negative places round to a power of ten: scale the whole-number result
    // back up and keep scale 0 (matches Decimal::round_to)
    if (places < 0) return dec_mk(q * pow10i(-places), 0);
    return dec_mk(q, places);
}
int beans_dec_cmp(BDec* a, BDec* b) {
    __int128 ca, cb;
    long long s;
    dec_align(a, b, &ca, &cb, &s);
    return ca < cb ? -1 : ca > cb ? 1 : 0;
}
// dec_cmp aligns scales, so 2.50 == 2.5 — hash the canonical trailing-zero-free
// form so equal decimals land in the same map index slot
long long beans_dec_hash(BDec* d) {
    __int128 c = d->c;
    long long s = d->s;
    while (s > 0 && c % 10 == 0) {
        c /= 10;
        s -= 1;
    }
    unsigned long long lo = (unsigned long long)(unsigned __int128)c;
    unsigned long long hi = (unsigned long long)((unsigned __int128)c >> 64);
    return (long long)beans_mix64(lo ^ beans_mix64(hi ^ (unsigned long long)s));
}
// same acceptance rule as the interpreter's dec_valid: [+-]? digits with '_',
// one optional '.', one optional e/E exponent with its own sign and a digit
static int dec_valid_c(const char* s) {
    size_t i = 0;
    if (s[i] == '+' || s[i] == '-') i++;
    int digits = 0, dot = 0;
    for (; s[i]; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') { digits++; continue; }
        if (c == '_') continue;
        if (c == '.' && !dot) { dot = 1; continue; }
        if ((c == 'e' || c == 'E') && digits) {
            i++;
            if (s[i] == '+' || s[i] == '-') i++;
            if (!(s[i] >= '0' && s[i] <= '9')) return 0;
            // capped at 4096 exactly like the interpreter's dec_valid
            long long ev = 0;
            while (s[i] >= '0' && s[i] <= '9') {
                ev = ev * 10 + (s[i] - '0');
                if (ev > 4096) return 0;
                i++;
            }
            return s[i] == '\0';
        }
        return 0;
    }
    return digits > 0;
}
// mirror of Decimal::parse — the two must compute identically
BRes beans_str_to_decimal(char* s) {
    if (!dec_valid_c(s)) return parse_fail(s, "decimal");
    __int128 coeff = 0;
    long long scale = 0, exp = 0;
    int neg = 0, after_dot = 0;
    size_t i = 0;
    if (s[i] == '-' || s[i] == '+') {
        neg = s[i] == '-';
        i++;
    }
    for (; s[i]; i++) {
        char c = s[i];
        if (c == '_') continue;
        if (c == '.') { after_dot = 1; continue; }
        if (c == 'e' || c == 'E') {
            exp = strtol(s + i + 1, NULL, 10);
            break;
        }
        coeff = coeff * 10 + (c - '0');
        if (after_dot) scale += 1;
    }
    scale -= exp;
    if (scale < 0) {
        coeff *= pow10i(-scale);
        scale = 0;
    }
    if (neg) coeff = -coeff;
    return (BRes){(long long)dec_mk(coeff, scale), NULL};
}
long long beans_dec_to_int(BDec* a) { return (long long)(a->c / pow10i(a->s)); }
double beans_dec_to_f64(BDec* a) { return (double)a->c / (double)pow10i(a->s); }
long long beans_decv_to_int(BDecV a) {
    return (long long)(decv_coeff(a) / pow10i(decv_scale(a)));
}
double beans_decv_to_f64(BDecV a) {
    return (double)decv_coeff(a) / (double)pow10i(decv_scale(a));
}
BDec* beans_dec_from_f64(double v) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.17g", v);
    __int128 c = 0;
    long long s = 0;
    int neg = 0, after = 0;
    long long ex = 0;
    for (const char* p = buf; *p; p++) {
        if (*p == '-') { neg = 1; continue; }
        if (*p == '+') continue;
        if (*p == '.') { after = 1; continue; }
        if (*p == 'e' || *p == 'E') {
            ex = strtoll(p + 1, NULL, 10);
            break;
        }
        c = c * 10 + (*p - '0');
        if (after) s += 1;
    }
    s -= ex;
    if (s < 0) {
        c *= pow10i(-s);
        s = 0;
    }
    if (neg) c = -c;
    return dec_mk(c, s);
}
BDecV beans_decv_from_f64(double v) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.17g", v);
    __int128 c = 0;
    long long s = 0;
    int neg = 0, after = 0;
    long long ex = 0;
    for (const char* p = buf; *p; p++) {
        if (*p == '-') { neg = 1; continue; }
        if (*p == '+') continue;
        if (*p == '.') { after = 1; continue; }
        if (*p == 'e' || *p == 'E') {
            ex = strtoll(p + 1, NULL, 10);
            break;
        }
        c = c * 10 + (*p - '0');
        if (after) s += 1;
    }
    s -= ex;
    if (s < 0) {
        c *= pow10i(-s);
        s = 0;
    }
    if (neg) c = -c;
    return decv_mk(c, s);
}
char* beans_dec_str(BDec* a) {
    __int128 c = a->c;
    int neg = c < 0;
    if (neg) c = -c;
    // scratch sized from the scale: an __int128 holds at most 39 digits, but
    // the zero-fill below runs to scale+1 — "1e-100".to_decimal() legitimately
    // carries scale 100, and the old fixed 64/80-byte stack buffers smashed
    // the stack. Small values keep the stack fast path.
    long long cap = (a->s > 38 ? a->s : 38) + 2;
    char dsmall[64], osmall[80];
    char* digits = cap <= 63 ? dsmall : malloc((size_t)cap + 1);
    char* out = cap + 2 <= 79 ? osmall : malloc((size_t)cap + 3);
    if (!digits || !out) beans_panic("decimal too large to print", 0, 0);
    long long n = 0;
    if (c == 0) digits[n++] = '0';
    while (c > 0) {
        digits[n++] = (char)('0' + (int)(c % 10));
        c /= 10;
    }
    while (n <= a->s) digits[n++] = '0';
    long long o = 0;
    if (neg) out[o++] = '-';
    for (long long i = n - 1; i >= 0; i--) {
        out[o++] = digits[i];
        if (i == a->s && i != 0) out[o++] = '.';
    }
    out[o] = '\0';
    char* r = rc_strdup(out);
    if (digits != dsmall) free(digits);
    if (out != osmall) free(out);
    return r;
}
char* beans_decv_str(BDecV v) {
    BDec d = {decv_coeff(v), decv_scale(v)};
    return beans_dec_str(&d);
}

// Decimal List values stay as their native 16-byte representation. The old
// generic slot ABI boxed each element solely to fit a pointer in i64.
void beans_list_decv_max(BList* list, BDecV* out, long long* ok) {
    *ok = list->len > 0;
    *out = 0;
    if (!*ok) return;
    BDecV* values = (BDecV*)list->data;
    *out = values[0];
    for (long long i = 1; i < list->len; i++)
        if (beans_decv_cmp(values[i], *out) > 0) *out = values[i];
}
void beans_list_decv_min(BList* list, BDecV* out, long long* ok) {
    *ok = list->len > 0;
    *out = 0;
    if (!*ok) return;
    BDecV* values = (BDecV*)list->data;
    *out = values[0];
    for (long long i = 1; i < list->len; i++)
        if (beans_decv_cmp(values[i], *out) < 0) *out = values[i];
}
long long beans_list_decv_contains(BList* list, BDecV value) {
    BDecV* values = (BDecV*)list->data;
    for (long long i = 0; i < list->len; i++)
        if (beans_decv_cmp(values[i], value) == 0) return 1;
    return 0;
}
long long beans_list_decv_index(BList* list, BDecV value, long long* ok) {
    BDecV* values = (BDecV*)list->data;
    for (long long i = 0; i < list->len; i++) {
        if (beans_decv_cmp(values[i], value) == 0) {
            *ok = 1;
            return i;
        }
    }
    *ok = 0;
    return 0;
}
static long long decv_less(BDecV a, BDecV b, void* thunk, void* box) {
    if (thunk)
        return ((long long (*)(void*, BDecV, BDecV))thunk)(box, a, b);
    return beans_decv_cmp(a, b) < 0;
}
static void list_decv_merge_sort(BList* list, void* thunk, void* box) {
    long long n = list->len;
    if (n < 2) return;
    BDecV* values = (BDecV*)list->data;
    BDecV* buffer = malloc((size_t)n * sizeof(BDecV));
    if (!buffer) beans_panic("out of memory", 0, 0);
    for (long long width = 1; width < n; width *= 2) {
        for (long long lo = 0; lo < n; lo += 2 * width) {
            long long mid = lo + width < n ? lo + width : n;
            long long hi = lo + 2 * width < n ? lo + 2 * width : n;
            long long left = lo, right = mid, out = lo;
            while (left < mid && right < hi) {
                if (!decv_less(values[right], values[left], thunk, box))
                    buffer[out++] = values[left++];
                else
                    buffer[out++] = values[right++];
            }
            while (left < mid) buffer[out++] = values[left++];
            while (right < hi) buffer[out++] = values[right++];
            memcpy(values + lo, buffer + lo, (size_t)(hi - lo) * sizeof(BDecV));
        }
    }
    free(buffer);
}
void beans_list_decv_sort(BList* list) { list_decv_merge_sort(list, NULL, NULL); }
void beans_list_decv_sort_by(BList* list, void* thunk, void* box) {
    list_decv_merge_sort(list, thunk, box);
}
void beans_list_decv_sort_by_key(BList* list, void* thunk, void* box) {
    long long n = list->len;
    if (n < 2) return;
    BDecV* values = (BDecV*)list->data;
    long long (*key_fn)(void*, BDecV) = (long long (*)(void*, BDecV))thunk;
    long long* keys = malloc((size_t)n * sizeof(long long));
    long long* key_buffer = malloc((size_t)n * sizeof(long long));
    BDecV* value_buffer = malloc((size_t)n * sizeof(BDecV));
    if (!keys || !key_buffer || !value_buffer) beans_panic("out of memory", 0, 0);
    for (long long i = 0; i < n; i++) keys[i] = key_fn(box, values[i]);
    for (long long width = 1; width < n; width *= 2) {
        for (long long lo = 0; lo < n; lo += 2 * width) {
            long long mid = lo + width < n ? lo + width : n;
            long long hi = lo + 2 * width < n ? lo + 2 * width : n;
            long long left = lo, right = mid, out = lo;
            while (left < mid && right < hi) {
                long long take = keys[right] < keys[left] ? right++ : left++;
                key_buffer[out] = keys[take];
                value_buffer[out++] = values[take];
            }
            while (left < mid) {
                key_buffer[out] = keys[left];
                value_buffer[out++] = values[left++];
            }
            while (right < hi) {
                key_buffer[out] = keys[right];
                value_buffer[out++] = values[right++];
            }
            memcpy(keys + lo, key_buffer + lo,
                   (size_t)(hi - lo) * sizeof(long long));
            memcpy(values + lo, value_buffer + lo,
                   (size_t)(hi - lo) * sizeof(BDecV));
        }
    }
    free(keys);
    free(key_buffer);
    free(value_buffer);
}
char* beans_list_decv_join(BList* list, char* separator) {
    long long separator_len = beans_slen(separator);
    char** parts = malloc((size_t)(list->len ? list->len : 1) * sizeof(char*));
    if (!parts) beans_panic("out of memory", 0, 0);
    BDecV* values = (BDecV*)list->data;
    long long total = 0;
    for (long long i = 0; i < list->len; i++) {
        parts[i] = beans_decv_str(values[i]);
        total += beans_slen(parts[i]);
        if (i) total += separator_len;
    }
    char* result = beans_alloc(total + 1, total << 3);
    char* write = result;
    for (long long i = 0; i < list->len; i++) {
        if (i) {
            memcpy(write, separator, (size_t)separator_len);
            write += separator_len;
        }
        long long length = beans_slen(parts[i]);
        memcpy(write, parts[i], (size_t)length);
        write += length;
        beans_release(parts[i]);
    }
    *write = 0;
    free(parts);
    return result;
}
char* beans_show_list_decv(BList* list) {
    char* separator = rc_strdup(", ");
    char* middle = beans_list_decv_join(list, separator);
    beans_release(separator);
    long long length = beans_slen(middle);
    char* result = beans_alloc(length + 3, (length + 2) << 3);
    result[0] = '[';
    memcpy(result + 1, middle, (size_t)length);
    result[length + 1] = ']';
    result[length + 2] = 0;
    beans_release(middle);
    return result;
}

// ---- std.fmt (mirrors builtins.cpp byte for byte) ----
// same 1e6 width ceiling the interpolation spec enforces at compile time — a
// pad is a fill, not an allocation primitive; past the cap it is a panic on
// both backends, not a 1TB alloc the OOM killer reaps
#define FMT_PAD_MAX 1000000
char* beans_fmt_pad_left(char* s, long long w, long long line, long long col) {
    if (w > FMT_PAD_MAX) beans_panic("pad width too large", line, col);
    long long n = beans_slen(s);
    if (w <= n) return str_make(s, n);
    char* out = beans_alloc(w + 1, w << 3);
    memset(out, ' ', (size_t)(w - n));
    memcpy(out + (w - n), s, (size_t)n);
    return out;
}
char* beans_fmt_pad_right(char* s, long long w, long long line, long long col) {
    if (w > FMT_PAD_MAX) beans_panic("pad width too large", line, col);
    long long n = beans_slen(s);
    if (w <= n) return str_make(s, n);
    char* out = beans_alloc(w + 1, w << 3);
    memcpy(out, s, (size_t)n);
    memset(out + n, ' ', (size_t)(w - n));
    return out;
}
char* beans_fmt_float(double x, long long p) {
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    char buf[512];
    snprintf(buf, sizeof buf, "%.*f", (int)p, x);
    return rc_strdup(buf);
}
char* beans_fmt_dec(BDec* d, long long p) {
    if (p < 0) p = 0;
    if (p > 60) p = 60;
    BDec t = *d;
    if (p < t.s) { // beans_dec_round, on the stack
        __int128 f = pow10i(t.s - p);
        __int128 q = t.c / f, rem = t.c % f;
        if (rem < 0) rem = -rem;
        if (rem * 2 >= f) q += t.c >= 0 ? 1 : -1;
        t.c = q;
        t.s = p;
    }
    char* base = beans_dec_str(&t);
    long long frac = t.s;
    if (p <= frac) return base;
    long long bn = beans_slen(base);
    long long extra = (frac == 0 ? 1 : 0) + (p - frac);
    char* out = beans_alloc(bn + extra + 1, (bn + extra) << 3);
    memcpy(out, base, (size_t)bn);
    long long o = bn;
    if (frac == 0) out[o++] = '.';
    for (long long i = 0; i < p - frac; i++) out[o++] = '0';
    beans_release(base);
    return out;
}
char* beans_decv_fmt(BDecV v, long long p) {
    BDec d = {decv_coeff(v), decv_scale(v)};
    return beans_fmt_dec(&d, p);
}
