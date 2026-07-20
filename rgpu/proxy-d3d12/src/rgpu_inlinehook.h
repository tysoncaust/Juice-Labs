/* rgpu minimal x64 inline (trampoline) hook.
 *
 * Writes a 14-byte RIP-relative absolute JMP (FF 25 00000000 <abs64>) at a target
 * function's entry and relocates the overwritten prologue into an executable
 * trampoline, so calling the trampoline runs the original function. Unlike vtable
 * patching, this hooks the function BODY — so a third-party overlay (e.g. UE4SS)
 * re-patching the same vtable slot afterwards cannot bypass us.
 *
 * CONSERVATIVE BY DESIGN: a small x64 length decoder relocates only a whitelist of
 * position-independent prologue instructions (push, mov r/m<->r, lea, sub/add
 * r/m,imm). If it meets ANYTHING it can't prove is safe to relocate (RIP-relative
 * operand, relative call/jmp, unknown opcode), it ABORTS the hook (returns false,
 * leaves the target untouched). A missed hook is acceptable; a corrupted target is
 * not. Install before the game starts submitting (single-threaded window). */
#ifndef RGPU_INLINEHOOK_H
#define RGPU_INLINEHOOK_H
#include <windows.h>
#include <cstdint>
#include <cstring>

/* Decoded instruction: total length + where a field that needs relocation lives.
 * rip_disp_off > 0: byte offset of a RIP-relative disp32 within the instruction.
 * rel32_off   > 0: byte offset of a rel32 (E8 call / E9 jmp) within the instruction. */
struct rgpu_insn { int len; int rip_disp_off; int rel32_off; };

/* Decode one x64 instruction from the small whitelist we relocate. len==0 => not
 * safely relocatable (abort the hook). */
static inline rgpu_insn rgpu_x64_decode(const uint8_t *p) {
    rgpu_insn r{0, 0, 0};
    int i = 0;
    for (;;) {   /* legacy + REX prefixes */
        uint8_t b = p[i];
        if (b == 0xF0 || b == 0xF2 || b == 0xF3 || b == 0x66 || b == 0x67) { i++; continue; }
        if ((b & 0xF0) == 0x40) { i++; break; }   /* REX is the last prefix */
        break;
    }
    uint8_t op = p[i++];
    bool bad = false;
    auto modrm = [&]() {   /* advance past ModRM(+SIB+disp); flags RIP-relative for fixup */
        uint8_t m = p[i++]; int mod = (m >> 6) & 3, rm = m & 7;
        if (mod == 3) return;
        if (rm == 4) { uint8_t sib = p[i++]; int base = sib & 7; if (mod == 0 && base == 5) i += 4; }
        else if (mod == 0 && rm == 5) { r.rip_disp_off = i; i += 4; return; }  /* RIP-rel disp32 */
        if (mod == 1) i += 1; else if (mod == 2) i += 4;
    };
    switch (op) {
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:    /* push r64 */
        case 0x90:                                     /* nop */
        case 0xC3:                                     /* ret (ends flow; still copyable) */
            break;
        case 0x88: case 0x89: case 0x8A: case 0x8B:    /* mov r/m<->r */
        case 0x8D: case 0x63:                          /* lea / movsxd */
            modrm(); break;
        case 0x0F: {
            uint8_t op2 = p[i++];
            if (op2 == 0x1E || op2 == 0x1F) modrm();    /* endbr64/32, multi-byte nop */
            else bad = true;
            break;
        }
        case 0x83: modrm(); i += 1; break;             /* grp1 r/m, imm8 */
        case 0x81: modrm(); i += 4; break;             /* grp1 r/m, imm32 */
        case 0xC7: modrm(); i += 4; break;             /* mov r/m, imm32 */
        case 0xE8: case 0xE9: r.rel32_off = i; i += 4; break;  /* call/jmp rel32 */
        default: bad = true; break;
    }
    r.len = bad ? 0 : i;
    return r;
}

/* Legacy length-only helper (used by tests). */
static inline int rgpu_x64_len(const uint8_t *p) { return rgpu_x64_decode(p).len; }

/* Allocate an executable page within +/-2GB of `target` so relocated RIP-relative
 * disp32 / rel32 fields in the trampoline stay in range. Falls back to any address
 * (fine for prologues with no relative fields). */
static inline void *rgpu_alloc_near(void *target, size_t size) {
    uintptr_t t = (uintptr_t)target;
    for (uintptr_t d = 0x10000; d < 0x60000000; d += 0x10000) {
        uintptr_t cand[2] = { (t - d) & ~(uintptr_t)0xFFFF, (t + d) & ~(uintptr_t)0xFFFF };
        for (int k = 0; k < 2; k++) {
            void *p = VirtualAlloc((void *)cand[k], size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
    }
    return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

/* Install an inline hook. On success, *tramp_out is an executable trampoline that
 * calls the original function; returns false (target untouched) if the prologue
 * can't be safely relocated. Relocates RIP-relative disp32 and rel32 call/jmp so
 * position-dependent prologues (e.g. `mov rbx,[rip+x]`) work too. */
static inline bool rgpu_inline_hook(void *target, void *detour, void **tramp_out) {
    uint8_t *t = (uint8_t *)target;
    struct { int off, len, ripoff, reloff; } ins[12];
    int nins = 0, copied = 0;
    while (copied < 14) {
        if (nins >= 12) return false;
        rgpu_insn d = rgpu_x64_decode(t + copied);
        if (d.len <= 0) return false;                  /* abort: not relocatable */
        ins[nins] = {copied, d.len, d.rip_disp_off, d.rel32_off};
        nins++; copied += d.len;
    }
    uint8_t *tramp = (uint8_t *)rgpu_alloc_near(target, (size_t)copied + 14);
    if (!tramp) return false;
    std::memcpy(tramp, t, copied);
    for (int j = 0; j < nins; j++) {                   /* fix up relative fields for the new location */
        int o = ins[j].off, len = ins[j].len;
        if (ins[j].ripoff) {
            int32_t disp; std::memcpy(&disp, t + o + ins[j].ripoff, 4);
            int64_t abs = (int64_t)(uintptr_t)(t + o + len) + disp;
            int64_t nd = abs - (int64_t)(uintptr_t)(tramp + o + len);
            if (nd < INT32_MIN || nd > INT32_MAX) { VirtualFree(tramp, 0, MEM_RELEASE); return false; }
            int32_t nd32 = (int32_t)nd; std::memcpy(tramp + o + ins[j].ripoff, &nd32, 4);
        }
        if (ins[j].reloff) {
            int32_t rel; std::memcpy(&rel, t + o + ins[j].reloff, 4);
            int64_t abs = (int64_t)(uintptr_t)(t + o + len) + rel;
            int64_t nr = abs - (int64_t)(uintptr_t)(tramp + o + len);
            if (nr < INT32_MIN || nr > INT32_MAX) { VirtualFree(tramp, 0, MEM_RELEASE); return false; }
            int32_t nr32 = (int32_t)nr; std::memcpy(tramp + o + ins[j].reloff, &nr32, 4);
        }
    }
    tramp[copied] = 0xFF; tramp[copied + 1] = 0x25;    /* jmp qword ptr [rip+0] -> original + copied */
    *(uint32_t *)(tramp + copied + 2) = 0;
    *(uint64_t *)(tramp + copied + 6) = (uint64_t)(t + copied);
    DWORD old;
    if (!VirtualProtect(t, 14, PAGE_EXECUTE_READWRITE, &old)) { VirtualFree(tramp, 0, MEM_RELEASE); return false; }
    t[0] = 0xFF; t[1] = 0x25; *(uint32_t *)(t + 2) = 0; *(uint64_t *)(t + 6) = (uint64_t)detour;  /* abs jmp -> detour */
    VirtualProtect(t, 14, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 14);
    *tramp_out = tramp;
    return true;
}
#endif
