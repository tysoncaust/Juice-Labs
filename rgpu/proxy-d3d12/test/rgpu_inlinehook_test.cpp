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
static fn1_t volatile g_overlayOriginalAddress = real2;
static int our_detour2(int x) { g_ourDetour++; return g_realTramp(x); }     /* our inline detour */
static int overlay_hook2(int x) { g_overlayCalls++; fn1_t f = g_overlayOriginalAddress; return f(x); } /* forced indirect call to patched address */

/* ---- test 3: a handcrafted prologue with a short conditional jump whose target
 * is also inside the overwritten 14-byte region. This is the exact relocation class
 * used by TXR's base CreateCommandList implementation. */
static fn1_t g_branchTramp = nullptr;
static int branch_detour(int x) { return g_branchTramp(x) + 2000; }

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("rgpu inline-hook test\n---------------------\n");

    /* Test 1 */
    volatile int va = 2, vb = 3;
    int before = target_fn(va, vb);
    if (!rgpu_inline_hook((void *)target_fn, (void *)detour_fn, (void **)&g_orig)) {
        std::printf("  TEST1 FAIL (hook aborted)\n"); return 1;
    }
    int after = target_fn(va, vb);
    int viaOrig = g_orig(va, vb);
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

    /* Test 3: test ecx,ecx; jz to an instruction inside the 14-byte patch span. */
    const unsigned char branchCode[14] = {
        0x85, 0xC9,             /* test ecx,ecx */
        0x74, 0x04,             /* jz +4 -> source offset 8 */
        0x8B, 0xC1,             /* mov eax,ecx */
        0xC3,                   /* ret */
        0x90,                   /* nop */
        0x31, 0xC0,             /* xor eax,eax */
        0xC3,                   /* ret */
        0x90, 0x90, 0x90        /* pad through the 14-byte hook span */
    };
    unsigned char *branchMem = (unsigned char *)VirtualAlloc(
        nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    int t3 = 0;
    if (branchMem) {
        std::memcpy(branchMem, branchCode, sizeof(branchCode));
        FlushInstructionCache(GetCurrentProcess(), branchMem, sizeof(branchCode));
        fn1_t branchFn = (fn1_t)branchMem;
        int beforeZero = branchFn(0), beforeNine = branchFn(9);
        bool installed = rgpu_inline_hook(branchMem, (void *)branch_detour, (void **)&g_branchTramp);
        int afterZero = installed ? branchFn(0) : -1;
        int afterNine = installed ? branchFn(9) : -1;
        int originalZero = installed ? g_branchTramp(0) : -1;
        int originalNine = installed ? g_branchTramp(9) : -1;
        t3 = installed && beforeZero == 0 && beforeNine == 9 &&
             afterZero == 2000 && afterNine == 2009 &&
             originalZero == 0 && originalNine == 9;
        std::printf("  TEST3 short-Jcc relocation: detour=(%d,%d) trampoline=(%d,%d) -> %s\n",
                    afterZero, afterNine, originalZero, originalNine, t3 ? "PASS" : "FAIL");
        if (g_branchTramp) VirtualFree((void *)g_branchTramp, 0, MEM_RELEASE);
        VirtualFree(branchMem, 0, MEM_RELEASE);
    } else {
        std::printf("  TEST3 short-Jcc relocation: allocation failed -> FAIL\n");
    }

    int ok = t1 && t2 && t3;
    std::printf("---------------------\n");
    std::printf(ok ? "RESULT: inline hooking works, survives vtable re-patching, and relocates short Jcc\n" : "RESULT: FAIL\n");
    return ok ? 0 : 1;
}
