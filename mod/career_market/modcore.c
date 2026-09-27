/*
 * modcore: C hooks without code-cave limits.
 *
 * build_mod.py extends libDLS18.so with a code segment (CAVE3) and a data segment (MODDATA) and,
 * for every entry in its RUNTIME_HOOKS list, patches the hook site with `b.w stub` in CAVE3. A stub
 * loads a pointer from its MODDATA slot; if the slot is empty it runs the displaced instructions and
 * continues (stock behaviour), otherwise it jumps to that pointer with every register and the stack
 * exactly as they were at the site.
 *
 * This file owns the other side: modcore_init() finds the CAVE3 descriptor through the loaded
 * program headers, and fills the slot of every hook that has a registered C handler with one of the
 * thunks below. A thunk saves r0-r12, lr and the flags into a ModCtx, calls the handler, and then
 * resumes either at the stub's "resume" block (the displaced instructions, then the site) or at an
 * address the handler chose. No code page is ever written at runtime; only MODDATA (plain data).
 */
#include <stdint.h>

#define MODCORE_DESC_MAGIC 0x3144434Du      /* "MCD1" */
#define MODCORE_MAX_HOOKS 64

typedef struct {
    uint32_t apsr;
    uint32_t r[13];        /* r0..r12 as at the hook site; a handler may change them */
    uint32_t lr;
    uint32_t target;       /* written by the thunk: where execution continues */
} ModCtx;                  /* the site's sp is (uint32_t)(ctx + 1) */

/* Return 0 to run the original instructions and continue; or an address (Thumb: |1) to go to. */
typedef uint32_t (*ModHookFn)(ModCtx *ctx, uint32_t base);

typedef struct {
    uint32_t magic;
    uint32_t count;
    uint32_t slots_va;     /* MODDATA address of slot 0 (link address) */
    uint32_t version;
} ModDescHeader;

typedef struct {
    uint32_t site_va;
    uint32_t resume_va;    /* stub's resume block, Thumb bit set */
    uint32_t name_hash;    /* FNV-1a of the hook name */
    uint32_t displaced;    /* number of displaced bytes */
} ModDescEntry;

typedef struct {
    uint32_t name_hash;
    ModHookFn fn;
} ModHandler;

static ModHandler g_mod_handlers[MODCORE_MAX_HOOKS];
static int32_t g_mod_handler_count;
static ModHookFn g_mod_by_slot[MODCORE_MAX_HOOKS];
static uint32_t g_mod_resume[MODCORE_MAX_HOOKS];
static uint32_t g_mod_hash[MODCORE_MAX_HOOKS];
static uint32_t g_mod_base;
static int32_t g_mod_hook_count;
static int32_t g_mod_ready;

uint32_t modcore_hash(const char *name) {
    uint32_t h = 2166136261u;
    while (*name) { h ^= (uint8_t)*name++; h *= 16777619u; }
    return h;
}

/* Register before modcore_init (or call modcore_init again after registering). */
int32_t modcore_register(const char *name, ModHookFn fn) {
    uint32_t hash = modcore_hash(name);
    for (int32_t i = 0; i < g_mod_handler_count; ++i) {
        if (g_mod_handlers[i].name_hash == hash) { g_mod_handlers[i].fn = fn; return i; }
    }
    if (g_mod_handler_count >= MODCORE_MAX_HOOKS) return -1;
    g_mod_handlers[g_mod_handler_count].name_hash = hash;
    g_mod_handlers[g_mod_handler_count].fn = fn;
    return g_mod_handler_count++;
}

__attribute__((used)) uint32_t modcore_dispatch(ModCtx *ctx, uint32_t slot) {
    uint32_t resume = slot < MODCORE_MAX_HOOKS ? g_mod_resume[slot] : 0;
    ModHookFn fn = slot < MODCORE_MAX_HOOKS ? g_mod_by_slot[slot] : 0;
    uint32_t target = fn ? fn(ctx, g_mod_base) : 0;
    return target ? target : resume;
}

/* One thunk per slot: save state, align the stack for C, dispatch, restore, jump to the target. */
#define MODCORE_THUNK(n) \
    __asm__(".text\n.balign 2\n.thumb\n.thumb_func\n" \
            ".type modcore_thunk_" #n ", %function\n" \
            "modcore_thunk_" #n ":\n" \
            "  sub sp, #4\n" \
            "  push.w {r0-r12, lr}\n" \
            "  mrs r0, apsr\n" \
            "  push {r0}\n" \
            "  mov r5, sp\n" \
            "  bic r4, r5, #7\n" \
            "  mov sp, r4\n" \
            "  mov r0, r5\n" \
            "  movs r1, #" #n "\n" \
            "  bl modcore_dispatch\n" \
            "  mov sp, r5\n" \
            "  str r0, [sp, #60]\n" \
            "  pop {r0}\n" \
            "  msr APSR_nzcvq, r0\n" \
            "  pop.w {r0-r12, lr}\n" \
            "  pop {pc}\n")
#define MODCORE_THUNK8(a) MODCORE_THUNK(a##0); MODCORE_THUNK(a##1); MODCORE_THUNK(a##2); MODCORE_THUNK(a##3); \
    MODCORE_THUNK(a##4); MODCORE_THUNK(a##5); MODCORE_THUNK(a##6); MODCORE_THUNK(a##7)
MODCORE_THUNK(0); MODCORE_THUNK(1); MODCORE_THUNK(2); MODCORE_THUNK(3);
MODCORE_THUNK(4); MODCORE_THUNK(5); MODCORE_THUNK(6); MODCORE_THUNK(7);
MODCORE_THUNK(8); MODCORE_THUNK(9);
MODCORE_THUNK8(1); MODCORE_THUNK8(2); MODCORE_THUNK8(3); MODCORE_THUNK8(4); MODCORE_THUNK8(5);
MODCORE_THUNK(60); MODCORE_THUNK(61); MODCORE_THUNK(62); MODCORE_THUNK(63);
/* slots 10-17, 20-27, ... use the 1x..5x macros; 18, 19, 28, 29, ... are defined here */
MODCORE_THUNK(18); MODCORE_THUNK(19); MODCORE_THUNK(28); MODCORE_THUNK(29); MODCORE_THUNK(38);
MODCORE_THUNK(39); MODCORE_THUNK(48); MODCORE_THUNK(49); MODCORE_THUNK(58); MODCORE_THUNK(59);

#define T(n) extern void modcore_thunk_##n(void)
T(0); T(1); T(2); T(3); T(4); T(5); T(6); T(7); T(8); T(9); T(10); T(11); T(12); T(13); T(14); T(15);
T(16); T(17); T(18); T(19); T(20); T(21); T(22); T(23); T(24); T(25); T(26); T(27); T(28); T(29); T(30);
T(31); T(32); T(33); T(34); T(35); T(36); T(37); T(38); T(39); T(40); T(41); T(42); T(43); T(44); T(45);
T(46); T(47); T(48); T(49); T(50); T(51); T(52); T(53); T(54); T(55); T(56); T(57); T(58); T(59); T(60);
T(61); T(62); T(63);
#undef T
static void (*const k_mod_thunks[MODCORE_MAX_HOOKS])(void) = {
    modcore_thunk_0, modcore_thunk_1, modcore_thunk_2, modcore_thunk_3, modcore_thunk_4, modcore_thunk_5,
    modcore_thunk_6, modcore_thunk_7, modcore_thunk_8, modcore_thunk_9, modcore_thunk_10, modcore_thunk_11,
    modcore_thunk_12, modcore_thunk_13, modcore_thunk_14, modcore_thunk_15, modcore_thunk_16, modcore_thunk_17,
    modcore_thunk_18, modcore_thunk_19, modcore_thunk_20, modcore_thunk_21, modcore_thunk_22, modcore_thunk_23,
    modcore_thunk_24, modcore_thunk_25, modcore_thunk_26, modcore_thunk_27, modcore_thunk_28, modcore_thunk_29,
    modcore_thunk_30, modcore_thunk_31, modcore_thunk_32, modcore_thunk_33, modcore_thunk_34, modcore_thunk_35,
    modcore_thunk_36, modcore_thunk_37, modcore_thunk_38, modcore_thunk_39, modcore_thunk_40, modcore_thunk_41,
    modcore_thunk_42, modcore_thunk_43, modcore_thunk_44, modcore_thunk_45, modcore_thunk_46, modcore_thunk_47,
    modcore_thunk_48, modcore_thunk_49, modcore_thunk_50, modcore_thunk_51, modcore_thunk_52, modcore_thunk_53,
    modcore_thunk_54, modcore_thunk_55, modcore_thunk_56, modcore_thunk_57, modcore_thunk_58, modcore_thunk_59,
    modcore_thunk_60, modcore_thunk_61, modcore_thunk_62, modcore_thunk_63,
};

/* elf_extend.py leaves {"MCPH", moved phdr vaddr, CAVE3 vaddr, MODDATA vaddr} at the old program
 * header offset (0x34, first page, read-only); the CAVE3 descriptor starts at the CAVE3 vaddr. */
#define MODCORE_PH_MARKER 0x4850434Du
static const ModDescHeader *find_descriptor(uint32_t base) {
    const uint32_t *marker = (const uint32_t *)(uintptr_t)(base + 0x34);
    if (marker[0] != MODCORE_PH_MARKER || marker[2] < 0x800000u || marker[2] > 0x2000000u) return 0;
    const ModDescHeader *d = (const ModDescHeader *)(uintptr_t)(base + marker[2]);
    return d->magic == MODCORE_DESC_MAGIC ? d : 0;
}

/* Fill the slots. Safe to call more than once (e.g. from every market entry point). */
int32_t modcore_init(uint32_t base) {
    if (!base) return 0;
    const ModDescHeader *d = find_descriptor(base);
    if (!d || d->count > MODCORE_MAX_HOOKS) return 0;
    const ModDescEntry *e = (const ModDescEntry *)(d + 1);
    uint32_t *slots = (uint32_t *)(uintptr_t)(base + d->slots_va);
    g_mod_base = base;
    g_mod_hook_count = (int32_t)d->count;
    int32_t installed = 0;
    for (uint32_t i = 0; i < d->count; ++i) {
        g_mod_resume[i] = base + e[i].resume_va;
        g_mod_hash[i] = e[i].name_hash;
        ModHookFn fn = 0;
        for (int32_t k = 0; k < g_mod_handler_count; ++k) {
            if (g_mod_handlers[k].name_hash == e[i].name_hash) fn = g_mod_handlers[k].fn;
        }
        g_mod_by_slot[i] = fn;
        uint32_t want = fn ? (uint32_t)(uintptr_t)k_mod_thunks[i] | 1u : 0u;
        if (slots[i] != want) slots[i] = want;
        installed += fn != 0;
    }
    g_mod_ready = 1;
    return installed;
}

int32_t modcore_hook_count(void) { return g_mod_hook_count; }

/* The resume block of a hook (displaced instructions, then the rest of the function), Thumb bit
 * set, or 0. A handler at a function entry can call it as the original function and then return to
 * the caller itself (ctx->lr), which wraps the stock body. */
uint32_t modcore_resume(const char *name) {
    uint32_t hash = modcore_hash(name);
    for (int32_t i = 0; i < g_mod_hook_count; ++i) {
        if (g_mod_hash[i] == hash) return g_mod_resume[i];
    }
    return 0;
}
