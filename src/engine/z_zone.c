/*
 * z_zone.c - Zone memory allocator
 *
 * Based on Quake II zone allocator (id Software GPL).
 * Provides tagged memory allocation for the engine.
 *
 * SoF exports: Z_Malloc, Z_Free, Z_Touch
 * Original addresses: Z_Malloc=0x20940, Z_Free=0x203D0, Z_Touch=0x20430
 */

#include "../common/qcommon.h"

/* Zone memory block header */
typedef struct zhead_s {
    struct zhead_s  *prev, *next;
    int             size;       /* including header */
    int             tag;        /* Z_TAG_* */
    int             magic;
} zhead_t;

#define Z_MAGIC     0x1D1D1D1D
#define Z_MAXSIZE   0x1000000   /* 16 MB max single allocation */

static zhead_t  z_chain;
static int      z_count;
static int      z_bytes;

/* ==========================================================================
   Z_Init
   ========================================================================== */

void Z_Init(void)
{
    z_chain.next = z_chain.prev = &z_chain;
    z_count = 0;
    z_bytes = 0;
}

/* ==========================================================================
   Z_TagMalloc
   ========================================================================== */

void *Z_TagMalloc(int size, int tag)
{
    zhead_t *z;

    if (size <= 0)
        Com_Error(ERR_FATAL, "Z_TagMalloc: size %d", size);
    if (size > Z_MAXSIZE)
        Com_Error(ERR_FATAL, "Z_TagMalloc: %d bytes is too large", size);

    size += sizeof(zhead_t);
    z = (zhead_t *)malloc(size);
    if (!z)
        Com_Error(ERR_FATAL, "Z_TagMalloc: failed on allocation of %d bytes", size);

    memset(z, 0, size);
    z->magic = Z_MAGIC;
    z->tag = tag;
    z->size = size;

    z->next = z_chain.next;
    z->prev = &z_chain;
    z_chain.next->prev = z;
    z_chain.next = z;

    z_count++;
    z_bytes += size;

    return (void *)(z + 1);
}

/* ==========================================================================
   Z_Malloc — SoF export, ordinal used by sound DLLs
   ========================================================================== */

void *Z_Malloc(int size)
{
    return Z_TagMalloc(size, Z_TAG_GENERAL);
}

/* ==========================================================================
   Z_Free — SoF export
   ========================================================================== */

void Z_Free(void *ptr)
{
    zhead_t *z;

    if (!ptr)
        return;

    z = ((zhead_t *)ptr) - 1;

    if (z->magic != Z_MAGIC)
        Com_Error(ERR_FATAL, "Z_Free: bad magic (%x)", z->magic);

    z->prev->next = z->next;
    z->next->prev = z->prev;

    z_count--;
    z_bytes -= z->size;

    free(z);
}

/* ==========================================================================
   Z_Touch — SoF export (prevents zone eviction in original; no-op here)
   ========================================================================== */

void Z_Touch(void *ptr)
{
    /* In the original SoF, this touched the memory page to prevent
     * it from being paged out. In a modern recomp, this is a no-op. */
    (void)ptr;
}

/* ==========================================================================
   Z_FreeTags — free all allocations with a given tag
   ========================================================================== */

void Z_FreeTags(int tag)
{
    zhead_t *z, *next;

    for (z = z_chain.next; z != &z_chain; z = next) {
        next = z->next;
        if (z->tag == tag) {
            Z_Free((void *)(z + 1));
        }
    }
}

/* ==========================================================================
   Hunk Memory — large persistent allocations via virtual memory
   ========================================================================== */

typedef struct {
    int     maxsize;
    int     cursize;
} hunkheader_t;

void *Hunk_Begin(int maxsize)
{
    hunkheader_t *hdr;

    /* Allocate the maximum possible size upfront.
     * On modern systems with virtual memory, this is fine —
     * only touched pages actually consume physical memory. */
    hdr = (hunkheader_t *)malloc(maxsize + sizeof(hunkheader_t));
    if (!hdr)
        Com_Error(ERR_FATAL, "Hunk_Begin: failed on %d bytes", maxsize);

    memset(hdr, 0, sizeof(hunkheader_t));
    hdr->maxsize = maxsize;
    hdr->cursize = 0;

    return (void *)hdr;
}

void *Hunk_Alloc(int size)
{
    hunkheader_t *hdr;
    byte *buf;

    /* The hunk base pointer was returned by Hunk_Begin */
    /* We need to recover it — in Q2, this was stored globally */
    /* For now, this is a simplified version */

    /* Align to 32 bytes */
    size = (size + 31) & ~31;

    /* TODO: In the full implementation, Hunk_Alloc works on the
     * current hunk allocated by Hunk_Begin. For now, just malloc. */
    buf = (byte *)malloc(size);
    if (!buf)
        Com_Error(ERR_FATAL, "Hunk_Alloc: failed on %d bytes", size);
    memset(buf, 0, size);

    return buf;
}

int Hunk_End(void)
{
    /* Return the total bytes used in the current hunk */
    /* TODO: Proper implementation with virtual memory commit */
    return 0;
}

void Hunk_Free(void *base)
{
    if (base)
        free(base);
}
