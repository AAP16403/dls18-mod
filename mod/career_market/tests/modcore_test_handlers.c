/* Test handlers for tests/test_modcore.py (compiled together with ../modcore.c). */
#include <stdint.h>

typedef struct { uint32_t apsr; uint32_t r[13]; uint32_t lr; uint32_t target; } ModCtx;
typedef uint32_t (*ModHookFn)(ModCtx *ctx, uint32_t base);
int32_t modcore_register(const char *name, ModHookFn fn);
int32_t modcore_init(uint32_t base);

static volatile uint32_t g_calls;
static volatile uint32_t g_seen_r0;
static volatile uint32_t g_seen_sp;

/* mode 1: observe and continue with the original instructions */
static uint32_t pass_through(ModCtx *ctx, uint32_t base) {
    (void)base;
    ++g_calls;
    g_seen_r0 = ctx->r[0];
    g_seen_sp = (uint32_t)(uintptr_t)(ctx + 1);
    return 0;
}

/* mode 2: replace the function: *r0 = 1000, then resume at the site's `bx lr` (Double+6) */
static uint32_t override(ModCtx *ctx, uint32_t base) {
    ++g_calls;
    *(int32_t *)(uintptr_t)ctx->r[0] = 1000;
    ctx->r[1] = 0x1234;
    return (base + 0x1FC6EA) | 1u;
}

__attribute__((visibility("default"))) int32_t test_init(uint32_t base, int32_t mode) {
    modcore_register("test_double", mode == 2 ? override : pass_through);
    return modcore_init(base);
}
__attribute__((visibility("default"))) uint32_t test_calls(void) { return g_calls; }
__attribute__((visibility("default"))) uint32_t test_seen_r0(void) { return g_seen_r0; }
__attribute__((visibility("default"))) uint32_t test_seen_sp(void) { return g_seen_sp; }
