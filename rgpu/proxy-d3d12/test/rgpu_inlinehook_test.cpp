/* Verify the rgpu inline hooker:
 *   Test 1 - basic: the detour fires AND the trampoline still calls the original
 *            (proves the x64 length decoder relocated the prologue correctly).
 *   Test 2 - the point of the hardening: an inline hook SURVIVES a later vtable
 *            re-patch (the UE4SS scenario). We inline-hook a function, then a
 *            simulated overlay overwrites the vtable slot to point at its own hook;
 *            when the app calls through the (re-patched) vtable, the overlay's hook
 *            calls the original function ADDRESS, which still hits our inline JMP -
 *            so BOTH fire. Plain vtable hooking loses this race; inline hooking wins. */
#include "../src/rgpu_inlinehook.h"
#include <cstdio>

typedef int (*fn_t)(int, int);
static fn_t g_orig = nullptr;
__attribute__((noinline)) int target_fn(int a, int b) {
    volatile int acc = a; for (int i = 0; i < 4; i++) acc += b + i; return acc;
}
static int detour_fn(int a, int b) { return g_orig(a, b) + 1000; }

/* ---- test 2 objects ---- */
typedef int (*fn1_t)(int);
static int g_realCalls = 0, g_ourDetour = 0, g_overlayCalls = 0;
static fn1_t g_realTramp = nullptr;
__attribute__((noinline)) int real2(int x) { volatile int a = x; for (int i = 0; i < 5; i++) a += i * x; g_realCalls++; return a; }
static int our_detour2(int x) { g_ourDetour++; return g_realTramp(x); }     /* our inline detour */
static int overlay_hook2(int x) { g_overlayCalls++; return ((fn1_t)real2)(x); } /* overlay: calls original addr */

int main() {
    std::printf("rgpu inline-hook test\n---------------------\n");

    /* Test 1 */
    volatile int va = 2, vb = 3;
    int before = target_fn(va, vb);
    if (!rgpu_inline_hook((void *)target_fn, (void *)detour_fn, (void **)&g_orig)) {
        std::printf("  TEST1 FAIL (hook aborted)\n"); return 1;
    }
    int after = target_fn(va, vb), viaOrig = g_orig(va, vb);
    int t1 = (after == before + 1000) && (viaOrig == before);
    std::printf("  TEST1 basic hook: detour=%d(exp %d) trampoline=%d(exp %d) -> %s\n",
                after, before + 1000, viaOrig, before, t1 ? "PASS" : "FAIL");

    /* Test 2: inline hook vs a later vtable re-patch */
    int base = real2(4);              /* also bumps g_realCalls to 1 */
    void *vtbl[1] = {(void *)real2};  /* a fake COM vtable slot pointing at the real fn */
    if (!rgpu_inline_hook((void *)real2, (void *)our_detour2, (void **)&g_realTramp)) {
        std::printf("  TEST2 FAIL (hook aborted)\n"); return 1;
    }
    vtbl[0] = (void *)overlay_hook2; /* <-- UE4SS-style: overwrite the vtable slot AFTER our inline hook */
    g_realCalls = g_ourDetour = g_overlayCalls = 0;
    int r = ((fn1_t)vtbl[0])(4);      /* app calls through the re-patched vtable */
    int t2 = (g_overlayCalls == 1 && g_ourDetour == 1 && g_realCalls == 1 && r == base);
    std::printf("  TEST2 survive vtable re-patch: overlay=%d ourInline=%d real=%d result=%d(exp %d) -> %s\n",
                g_overlayCalls, g_ourDetour, g_realCalls, r, base, t2 ? "PASS" : "FAIL");

    int ok = t1 && t2;
    std::printf("---------------------\n");
    std::printf(ok ? "RESULT: inline hooking works AND survives a vtable re-patch (beats UE4SS)\n" : "RESULT: FAIL\n");
    return ok ? 0 : 1;
}
