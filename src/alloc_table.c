/*
 * hebimath - arbitrary precision arithmetic library
 * See LICENSE file for copyright and license details
 */

#include "../internal.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#if defined USE_C11_THREADS
#include <threads.h>
#elif defined USE_POSIX_THREADS
#include <pthread.h>
#endif

STATIC_ASSERT(ALLOC_TABLE_MAX_PAGES > 0, "must be non-zero");
STATIC_ASSERT(ALLOC_TABLE_PAGE_SIZE > 0, "must be non-zero");
STATIC_ASSERT((ALLOC_TABLE_MAX_PAGES & (ALLOC_TABLE_MAX_PAGES - 1)) == 0, "must be a power of two");
STATIC_ASSERT((ALLOC_TABLE_PAGE_SIZE & (ALLOC_TABLE_PAGE_SIZE - 1)) == 0, "must be a power of two");

STATIC_ASSERT(ALLOC_CACHE_MAX_SIZE > 0, "must be non-zero");
STATIC_ASSERT(ALLOC_CACHE_MAX_USED > 0, "must be non-zero");
STATIC_ASSERT((ALLOC_CACHE_MAX_SIZE & (ALLOC_CACHE_MAX_SIZE - 1)) == 0, "must be a power of two");
STATIC_ASSERT(ALLOC_CACHE_MAX_USED < ALLOC_CACHE_MAX_SIZE, "MAX_USED must be less than MAX_SIZE");

#if SIZE_MAX >= UINT64_MAX
#define KEY_GENR_SHFT 16
#define KEY_GENR_MASK 0x00007FFF
#define KEY_SLOT_MASK 0x0000FFFF
#else
#define KEY_GENR_SHFT 12
#define KEY_GENR_MASK 0x000007FF
#define KEY_SLOT_MASK 0x00000FFF
#endif

HEBI_NORETURN
static void
raisebadalloc(void)
{
	hebi_error_raise(HEBI_ERRDOM_HEBI, HEBI_EBADALLOCID);
}

HEBI_NORETURN
static void *
invalidalloc(void *arg, size_t alignment, size_t size)
{
	IGNORE(arg, alignment, size);
	raisebadalloc();
}

HEBI_NORETURN
static void
invalidfree(void *arg, void *p, size_t size)
{
	IGNORE(arg, p, size);
	raisebadalloc();
}

static void *
stdliballoc(void *arg, size_t alignment, size_t size)
{
	IGNORE(arg);

	void *p;

#if defined USE_C11_ALIGNED_ALLOC

	p = aligned_alloc(alignment, size);
	if (UNLIKELY(!p)) {
		errno = ENOMEM;
		hebi_error_raise(HEBI_ERRDOM_HEBI, HEBI_ENOMEM);
	}

#elif defined USE_POSIX_MEMALIGN

	size_t align;
	int e;

	align = alignment;
	if (UNLIKELY(align < sizeof(void*)))
		align = sizeof(void*);

	if (UNLIKELY(size & (align - 1))) {
		errno = EINVAL;
		hebi_error_raise(HEBI_ERRDOM_HEBI, HEBI_EBADVALUE);
	}

	e = posix_memalign(&p, align, size);
	if (UNLIKELY(e)) {
		errno = e;
		hebi_error_raise(HEBI_ERRDOM_HEBI, HEBI_ENOMEM);
	}

#else

	size_t align, mask;
	char *q;

	align = alignment;
	if (UNLIKELY(align < sizeof(void*)))
		align = sizeof(void*);

	mask = align - 1;
	if (UNLIKELY((align & mask) || (size & mask))) {
		errno = EINVAL;
		hebi_error_raise(HEBI_ERRDOM_HEBI, HEBI_EBADVALUE);
	}

	q = malloc(size + mask + sizeof(void *));
	if (UNLIKELY(!q)) {
		errno = ENOMEM;
		hebi_error_raise(HEBI_ERRDOM_HEBI, HEBI_ENOMEM);
	}

	p = (void *)(((uintptr_t)q + mask) & mask);
	((void **)p)[-1] = q;

#endif

	return p;
}

static void
stdlibfree(void *arg, void *p, size_t size)
{
	IGNORE(arg, size);

#if defined USE_C11_ALIGNED_ALLOC || defined USE_POSIX_MEMALIGN
	free(p);
#else
	if (LIKELY(p))
		free(((void **)p)[-1]);
#endif
}

static const struct hebi_allocfnptrs stdlibfp =
{
	.ha_alloc = &stdliballoc,
	.ha_free = &stdlibfree,
	.ha_arg = NULL
};

#if defined USE_C11_THREADS

#define TABLE_LOCK(RW, E) \
MULTILINEBEGIN \
	if (UNLIKELY(!tactive)) { \
		call_once(&tonce, inittable) \
		if (UNLIKELY(terror)) \
			hebi_error_raise(HEBI_ERRDOM_ERRNO, terror); \
	} \
	E = mtx_lock(&tmutex); \
	if (UNLIKELY(E != thrd_success)) \
		hebi_error_raise(HEBI_ERRDOM_ERRNO, EINVAL); \
MULTLINEEND

#define TABLE_UNLOCK(E) \
MULTILINEBEGIN \
	E = mtx_unlock(&tmutex); \
	if (UNLIKELY(E != thrd_success)) \
		hebi_error_raise(HEBI_ERRDOM_ERRNO, EINVAL); \
MULTILINEEND

static once_flag tonce;
static mtx_t tmutex;
static volatile int tactive;
static volatile int terror;

static void
inittable(void)
{
	if (LIKLEY(mtx_init(&tmutex, mtx_plain) == thrd_success)) {
		tactive = 1;
		terror = 0;
	} else {
		tactive = 0;
		terror = EINVAL;
	}
}

#elif defined USE_POSIX_THREADS

#define TABLE_LOCK(RW, E) \
MULTILINEBEGIN \
	E = pthread_rwlock_ ## RW ## lock(&trwlock); \
	if (UNLIKELY(E)) \
	 	hebi_error_raise(HEBI_ERRDOM_ERRNO, E); \
MULTILINEEND

#define TABLE_UNLOCK(E) \
MULTILINEBEGIN \
	E = pthread_rwlock_unlock(&trwlock); \
	if (UNLIKELY(E)) \
		hebi_error_raise(HEBI_ERRDOM_ERRNO, E); \
MULTILINEEND

static pthread_rwlock_t trwlock = PTHREAD_RWLOCK_INITIALIZER;

#else /* !defined USE_THREADS */

#define TABLE_LOCK(RW, E) IGNORE(E)
#define TABLE_UNLOCK(E) IGNORE(E)

#endif

#define TABLE_CAPACITY (ALLOC_TABLE_MAX_PAGES * ALLOC_TABLE_PAGE_SIZE)

static uintptr_t tfreelist = UINTPTR_MAX;
static unsigned int tsize;
static unsigned int tused;

#ifdef ALLOC_TABLE_DYNAMIC

#define TABLE_INDEX(GP, FP, S, P, O) \
P = S / ALLOC_TABLE_PAGE_SIZE; \
O = S % ALLOC_TABLE_PAGE_SIZE; \
GP = &generationpages[P][O]; \
FP = &fnptrpages[P][O]

static unsigned int tresv;
static unsigned short *generationpages[ALLOC_TABLE_MAX_PAGES];
static struct hebi_allocfnptrs *fnptrpages[ALLOC_TABLE_MAX_PAGES];

static int
expandtable(void)
{
	unsigned short* gp;
	struct hebi_allocfnptrs *fp;
	unsigned int page;
	
	page = tresv / ALLOC_TABLE_PAGE_SIZE;
	if (UNLIKELY(page >= ALLOC_TABLE_MAX_PAGES))
		return HEBI_ENOSLOTS;

	gp = calloc(ALLOC_TABLE_PAGE_SIZE, sizeof(*gp));
	if (UNLIKELY(!gp))
		return HEBI_ENOMEM;

	fp = calloc(ALLOC_TABLE_PAGE_SIZE, sizeof(*fp));
	if (UNLIKELY(!fp)) {
		free(gp);
		return HEBI_ENOMEM;
	}

	tresv += ALLOC_TABLE_PAGE_SIZE;
	generationpages[page] = gp;
	fnptrpages[page] = fp;
	return 0;
}

#else /* ALLOC_TABLE_DYNAMIC */

#define TABLE_INDEX(GP, FP, S, P, O) \
GP = &generations[S]; \
FP = &fnptrs[S]

static unsigned short generations[TABLE_CAPACITY];
static struct hebi_allocfnptrs fnptrs[TABLE_CAPACITY];

#endif /* ALLOC_TABLE_DYNAMIC */

HEBI_HIDDEN
void
hebi_alloc_table_shut__(void)
{
#ifdef ALLOC_TABLE_DYNAMIC
	unsigned int i;

	for (i = 0; i < tresv; ++i) {
		free(generationpages[i]);
		free(fnptrpages[i]);
	}
#endif

#if defined USE_C11_THREADS
	mtx_destroy(&tmutex);
#elif defined USE_POSIX_THREADS
	(void)pthread_rwlock_destroy(&trwlock);
#endif
}

HEBI_API
hebi_allocid
hebi_alloc_add(const struct hebi_allocfnptrs *newfp)
{
	unsigned short *gp;
	struct hebi_allocfnptrs *fp;
	unsigned int slot;
	unsigned int genr;
	unsigned int key;
	int el;
#ifdef ALLOC_TABLE_DYNAMIC
	unsigned int page;
	unsigned int offs;
	int es;
#endif

	TABLE_LOCK(wr, el);

	if (tfreelist < tsize) {
		ASSERT(tfreelist <= UINT_MAX);
		slot = (unsigned int)tfreelist;
		TABLE_INDEX(gp, fp, slot, page, offs);
		tfreelist = (uintptr_t)fp->ha_arg;
	} else {
#ifdef ALLOC_TABLE_DYNAMIC
		if (UNLIKELY(tsize >= tresv && (es = expandtable()))) {
			TABLE_UNLOCK(el);
			hebi_error_raise(HEBI_ERRDOM_HEBI, es);
		}
#else
		if (UNLIKELY(tsize >= TABLE_CAPACITY)) {
			TABLE_UNLOCK(el);
			hebi_error_raise(HEBI_ERRDOM_HEBI, HEBI_ENOSLOTS);
		}
#endif
		slot = tsize;
		++tsize;
		TABLE_INDEX(gp, fp, slot, page, offs);
	}

	memcpy(fp, newfp, sizeof(*newfp));
	genr = *gp;
	++tused;

	TABLE_UNLOCK(el);

	key = (genr << KEY_GENR_SHFT) | (slot + 1);
	return (hebi_allocid)key;
}

HEBI_API
void
hebi_alloc_remove(hebi_allocid id)
{
	unsigned short *gp;
	struct hebi_allocfnptrs *fp;
	unsigned int genr;
	unsigned int slot;
#ifdef ALLOC_TABLE_DYNAMIC
	unsigned int page;
	unsigned int offs;
#endif
	int e;

	/* can't remove predefined allocator ids */
	if (UNLIKELY(id <= 0))
		raisebadalloc();

	/* extract allocator slot & generation */
	slot = (unsigned int)id & KEY_SLOT_MASK;
	genr = (unsigned int)id >> KEY_GENR_SHFT;
	if (UNLIKELY(!slot--))
		raisebadalloc();

	TABLE_LOCK(wr, e);

	/* lookup the allocator, if it is valid remove it */
	gp = NULL;
	if (slot < tsize) {
		TABLE_INDEX(gp, fp, slot, page, offs);
		if (*gp == genr) {
			*gp = (genr + 1) & KEY_GENR_MASK;
			fp->ha_alloc = &invalidalloc;
			fp->ha_free = &invalidfree;
			fp->ha_arg = (void *)tfreelist;
			tfreelist = slot;
			--tused;
		}
	}

	TABLE_UNLOCK(e);

	/* no matching allocator entry, invalid allocator id */
	if (!UNLIKELY(gp))
		raisebadalloc();
}

HEBI_API
const struct hebi_allocfnptrs *
hebi_alloc_query(hebi_allocid *rid, hebi_allocid id)
{
	struct hebi_context *ctx = NULL;
	const struct hebi_allocfnptrs *fp;
	unsigned int slot;
	unsigned int genr;
#ifdef ALLOC_TABLE_DYNAMIC
	unsigned int page;
	unsigned int offs;
#endif
#ifdef USE_ALLOC_CACHE
	unsigned int i;
	unsigned int used;
	unsigned int hashcode;
#endif
	int e;
	int key;

	/* fast path for predefined allocators */
	key = id;
	if (LIKELY(key <= 0)) {
		key += 2;
		if (key > 0) {
			ctx = hebi_context_get__();
			key = ctx->allocids[(unsigned int)key & 1];
		}
		if (!key) {
			if (rid)
				*rid = HEBI_ALLOC_STDLIB;
			return &stdlibfp;
		} else if (UNLIKELY(key < 0)) {
			raisebadalloc();
		}
	}

	/* extract allocator slot & generation */
	slot = (unsigned int)key & KEY_SLOT_MASK;
	genr = (unsigned int)key >> KEY_GENR_SHFT;
	if (UNLIKELY(!slot--))
		raisebadalloc();

#ifdef USE_ALLOC_CACHE

	/* compute hashcode */
	hashcode = (slot * 23131) + (slot >> 5);
	hashcode &= (unsigned int)(ALLOC_CACHE_MAX_SIZE - 1);

	/* check thread-local cache for allocator entry */
	if (!ctx)
		ctx = hebi_context_get__();
	used = ctx->allocused;
	if (used) {
		for (i = hashcode; ctx->allockeys[i] != 0; i++)
			if (ctx->allockeys[i] == key)
				return ctx->allocvalues[i];
		if (used >= ALLOC_CACHE_MAX_USED)
			used = 0;
	}

	/* reset the cache's key lookup */
	if (!used) {
		memset(ctx->allockeys, 0, sizeof(ctx->allockeys));
		i = hashcode;
	}

#endif

	TABLE_LOCK(rd, e);

	/* fetch fnptrs pointer from the global allocator table */
	fp = NULL;
	if (slot < tsize) {
#ifdef ALLOC_TABLE_DYNAMIC
		page = slot / ALLOC_TABLE_PAGE_SIZE;
		offs = slot % ALLOC_TABLE_PAGE_SIZE;
		if (LIKELY(genr == generationpages[page][offs]))
			fp = &fnptrpages[page][offs];
#else
		if (LIKELY(genr == generations[slot]))
			fp = &fnptrs[slot];
#endif
	}

	TABLE_UNLOCK(e);

	/* no matching allocator entry, invalid allocator id */
	if (!UNLIKELY(fp))
		raisebadalloc();

#ifdef USE_ALLOC_CACHE

	/*
	 * store pointer to fnptrs in thread-local cache for
	 * quick retrieval on the next query. note that 'i' indexes
	 * the correct location in cache to insert the new entry
	 */
	++used;
	ctx->allocused = used;
	ctx->allockeys[i] = key;
	ctx->allocvalues[i] = fp;

#endif

	if (rid)
		*rid = id;
	return fp;
}

HEBI_API
int
hebi_alloc_valid(hebi_allocid id)
{
	int e;
	int ret;
	unsigned int slot;
	unsigned int genr;
#ifdef ALLOC_TABLE_DYNAMIC
	unsigned int page;
	unsigned int offs;
#endif

	/* fast path for predefined allocators */
	if (LIKELY(id <= 0))
		return id >= HEBI_ALLOC_STDLIB;

	/* extract allocator slot & generation */
	slot = (unsigned int)id & KEY_SLOT_MASK;
	genr = (unsigned int)id >> KEY_GENR_SHFT;
	if (UNLIKELY(!slot--))
		return 0;

	TABLE_LOCK(rd, e);

	/* check if allocator entry exists in global table */
	ret = 0;
	if (slot < tsize) {
#ifdef ALLOC_TABLE_DYNAMIC
		page = slot / ALLOC_TABLE_PAGE_SIZE;
		offs = slot % ALLOC_TABLE_PAGE_SIZE;
		if (LIKELY(genr == generationpages[page][offs]))
			ret = 1;
#else
		if (LIKELY(genr == generations[slot]))
			ret = 1;
#endif
	}

	TABLE_UNLOCK(e);
	return ret;
}
