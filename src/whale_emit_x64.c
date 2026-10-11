/*
 * WhaleJS: x86-64 direct machine-code emitter (Windows PE x64 ABI).
 *
 * Why this file exists: TinyCC spills every named C variable to memory at
 * every statement boundary (by design - it has no register allocator), so
 * once the generator has folded everything foldable, the remaining gap to
 * V8/node is per-statement load/store traffic that no C-level trick removes.
 * This emitter takes the same double-domain core the token path builds and
 * emits machine code with real register allocation (loop-carried locals stay
 * in xmm across the whole loop) instead of going through C.
 *
 * Scope, deliberately narrow (route B, notes <67>/<87>):
 * - only the numeric core (`double core(ctx, args, An, deopt, ret_obj,
 *   var_refs)`): double arithmetic, comparisons, branches, locals, direct
 *   recursive calls. The wrapper (unbox/box/deopt/object returns) keeps going
 *   through TinyCC - it is all calls and branches with no hot loop.
 * - Windows x64 calling convention throughout (rcx/rdx/r8/r9, xmm0-3,
 *   32-byte shadow space, caller-owned). No SysV, no ARM here.
 * - executable memory is VirtualAlloc'd RWX; no .pdata/unwind registration
 *   (nothing throws through JIT code - deopt returns normally).
 *
 * Status: skeleton (byte buffer + executable memory + add-machine probe).
 * The core lowering plugs in next, one opcode family at a time, each verified
 * against the token path's output before the next lands.
 */
#include <windows.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} WhaleX64Buf;

static int whale_x64_reserve(WhaleX64Buf *b, size_t extra)
{
    uint8_t *nb;
    size_t ncap;

    if (b->len + extra <= b->cap)
        return 1;
    ncap = b->cap ? b->cap * 2 : 256;
    while (ncap < b->len + extra)
        ncap *= 2;
    nb = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, ncap);
    if (!nb)
        return 0;
    if (b->buf) {
        memcpy(nb, b->buf, b->len);
        HeapFree(GetProcessHeap(), 0, b->buf);
    }
    b->buf = nb;
    b->cap = ncap;
    return 1;
}

static int whale_x64_byte(WhaleX64Buf *b, uint8_t v)
{
    if (!whale_x64_reserve(b, 1))
        return 0;
    b->buf[b->len++] = v;
    return 1;
}

static int whale_x64_bytes(WhaleX64Buf *b, const uint8_t *p, size_t n)
{
    size_t i;

    if (!whale_x64_reserve(b, n))
        return 0;
    for (i = 0; i < n; i++)
        b->buf[b->len++] = p[i];
    return 1;
}

/* Commit the buffer to executable memory. Returns the entry point, or NULL.
   The caller owns nothing afterwards: the page lives until process exit
   (same lifetime rule as the singleton TCC state). */
static void *whale_x64_commit(WhaleX64Buf *b)
{
    void *mem;

    if (b->len == 0)
        return NULL;
    mem = VirtualAlloc(NULL, (b->len + 4095) & ~(size_t)4095,
                       MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem)
        return NULL;
    memcpy(mem, b->buf, b->len);
    return mem;
}

/* Probe: `double f(double a, double b) { return a + b; }` in raw bytes
   (movapd xmm0,xmm0 is a no-op placeholder so the shape is visible;
   the real sequence is addsd xmm0,xmm1; ret).
   Windows x64: doubles arrive in xmm0/xmm1, return in xmm0. */
typedef double (*WhaleX64Add2)(double a, double b);

void *JS_WhaleX64ProbeAdd2(void)
{
    WhaleX64Buf b;
    /* addsd xmm0, xmm1 (F2 0F 58 C1) ; ret (C3) */
    static const uint8_t code[] = { 0xF2, 0x0F, 0x58, 0xC1, 0xC3 };

    memset(&b, 0, sizeof(b));
    if (!whale_x64_bytes(&b, code, sizeof(code)))
        return NULL;
    {
        void *fn = whale_x64_commit(&b);

        if (b.buf)
            HeapFree(GetProcessHeap(), 0, b.buf);
        return fn;
    }
}

/* ========================================================================
 * Rejected direction (kept as a record): a token->IR translator here.
 *
 * The translator assumed the x64 tier would take the token stream and
 * rebuild IR from it - a second parse of an already-parsed program, paying
 * conversion cost on every compile and leaving the single-pass property.
 * Per review: never build IR on the side. Expensive work belongs in the
 * re-JIT tier (1000-call recompile, gated by executed/compile-cost ratio),
 * and cheap wins belong fused into TCC's own emit points (gen_opf/gen_opi,
 * single-pass, zero IR). The constructs below stay deleted; the byte
 * buffer + commit above remain as the emit-piece library (VEX/REX/mm
 * encoders incubate here, then move into gen_op* one at a time).
 * ======================================================================== */
