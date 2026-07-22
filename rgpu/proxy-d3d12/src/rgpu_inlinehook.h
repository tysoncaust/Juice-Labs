/* rgpu minimal x64 inline (trampoline) hook.
 *
 * Writes a 5-byte relative JMP at the target to a nearby relay, whose 14-byte
 * absolute JMP reaches the detour. The overwritten prologue is relocated into an
 * executable trampoline, so calling the trampoline runs the original function. Unlike vtable
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
 * rel32_off   > 0: byte offset of a rel32 (E8 call / E9 jmp) within the instruction.
 * rel8_off    > 0: byte offset of a short conditional-branch displacement. */
struct rgpu_insn {
    int len;
    int rip_disp_off;
    int rel32_off;
    int rel8_off;
    int rel8_cond;
};

/* Decode one x64 instruction from the small whitelist we relocate. len==0 => not
 * safely relocatable (abort the hook). */
static inline rgpu_insn rgpu_x64_decode(const uint8_t *p) {
    rgpu_insn r{0, 0, 0, 0, 0};
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
    if (op >= 0x70 && op <= 0x7F) {                 /* short Jcc rel8 */
        r.rel8_off = i;
        r.rel8_cond = op & 0x0F;
        i += 1;
        r.len = i;
        return r;
    }
    switch (op) {
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:    /* push r64 */
        case 0x90:                                     /* nop */
        case 0xC3:                                     /* ret (ends flow; still copyable) */
            break;
        case 0x84: case 0x85:                          /* test r/m, r */
        case 0x88: case 0x89: case 0x8A: case 0x8B:    /* mov r/m<->r */
        case 0x8D: case 0x63:                          /* lea / movsxd */
        case 0x00: case 0x01: case 0x08: case 0x09:    /* add/or  r/m<->r */
        case 0x30: case 0x31: case 0x32: case 0x33:    /* xor/... r/m<->r */
        case 0x28: case 0x29: case 0x2A: case 0x2B:    /* sub     r/m<->r */
            modrm(); break;
        case 0x0F: {
            uint8_t op2 = p[i++];
            /* endbr64/32 + multi-byte nop, plus common SSE/SSE2 reg/mem instructions
             * (op2 + ModRM, no immediate) that appear in MSVC prologues: movups/movss,
             * movaps, ucomiss, and/andn/or/xorps, add/mul/sub/div ps, movd/movq/movdqa,
             * movq, pxor. These are position-independent (modrm() flags any RIP-rel). */
            if (op2 == 0x1E || op2 == 0x1F ||
                op2 == 0x10 || op2 == 0x11 || op2 == 0x28 || op2 == 0x29 ||
                op2 == 0x2E || op2 == 0x2F || op2 == 0x54 || op2 == 0x55 ||
                op2 == 0x56 || op2 == 0x57 || op2 == 0x58 || op2 == 0x59 ||
                op2 == 0x5C || op2 == 0x5E || op2 == 0x6E || op2 == 0x6F ||
                op2 == 0x7E || op2 == 0x7F || op2 == 0xD6 || op2 == 0xEF)
                modrm();
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
    struct HookInsn {
        int src_off, src_len, out_off, out_len;
        int ripoff, rel32off, rel8off, rel8cond;
    } ins[12];
    int nins = 0, copied = 0, tramp_bytes = 0;

    /* Patch the target with a 5-byte relative jump to a nearby relay. The relay then
     * performs the unrestricted 14-byte absolute jump to the detour. This minimizes
     * the overwritten prologue and avoids corrupting common loop/branch targets that
     * land between bytes 5 and 13 of a function entry. */
    while (copied < 5) {
        if (nins >= 12) return false;
        rgpu_insn d = rgpu_x64_decode(t + copied);
        if (d.len <= 0) return false;
        int out_len = d.rel8_off ? 6 : d.len;
        ins[nins] = {copied, d.len, tramp_bytes, out_len,
                     d.rip_disp_off, d.rel32_off, d.rel8_off, d.rel8_cond};
        nins++;
        copied += d.len;
        tramp_bytes += out_len;
    }

    const size_t jump_size = 14;
    const size_t allocation_size = (size_t)tramp_bytes + jump_size + jump_size;
    uint8_t *tramp = (uint8_t *)rgpu_alloc_near(target, allocation_size);
    if (!tramp) return false;
    uint8_t *relay = tramp + tramp_bytes + jump_size;

    auto translate_branch_target = [&](uintptr_t absolute, uintptr_t *translated) -> bool {
        uintptr_t begin = (uintptr_t)t, finish = begin + (uintptr_t)copied;
        if (absolute < begin || absolute >= finish) {
            *translated = absolute;
            return true;
        }
        int source_offset = (int)(absolute - begin);
        for (int k = 0; k < nins; k++) {
            if (ins[k].src_off == source_offset) {
                *translated = (uintptr_t)(tramp + ins[k].out_off);
                return true;
            }
        }
        return false;
    };

    for (int j = 0; j < nins; j++) {
        HookInsn &h = ins[j];
        uint8_t *src = t + h.src_off;
        uint8_t *dst = tramp + h.out_off;

        if (h.rel8off) {
            int8_t rel = 0;
            std::memcpy(&rel, src + h.rel8off, 1);
            uintptr_t absolute = (uintptr_t)(src + h.src_len) + (intptr_t)rel;
            uintptr_t translated = 0;
            if (!translate_branch_target(absolute, &translated)) {
                VirtualFree(tramp, 0, MEM_RELEASE);
                return false;
            }
            dst[0] = 0x0F;
            dst[1] = (uint8_t)(0x80 | h.rel8cond);
            int64_t nr = (int64_t)translated - (int64_t)(uintptr_t)(dst + 6);
            if (nr < INT32_MIN || nr > INT32_MAX) {
                VirtualFree(tramp, 0, MEM_RELEASE);
                return false;
            }
            int32_t nr32 = (int32_t)nr;
            std::memcpy(dst + 2, &nr32, 4);
            continue;
        }

        std::memcpy(dst, src, (size_t)h.src_len);
        if (h.ripoff) {
            int32_t disp;
            std::memcpy(&disp, src + h.ripoff, 4);
            int64_t absolute = (int64_t)(uintptr_t)(src + h.src_len) + disp;
            int64_t nd = absolute - (int64_t)(uintptr_t)(dst + h.src_len);
            if (nd < INT32_MIN || nd > INT32_MAX) {
                VirtualFree(tramp, 0, MEM_RELEASE);
                return false;
            }
            int32_t nd32 = (int32_t)nd;
            std::memcpy(dst + h.ripoff, &nd32, 4);
        }
        if (h.rel32off) {
            int32_t rel;
            std::memcpy(&rel, src + h.rel32off, 4);
            uintptr_t absolute = (uintptr_t)(src + h.src_len) + (intptr_t)rel;
            uintptr_t translated = 0;
            if (!translate_branch_target(absolute, &translated)) {
                VirtualFree(tramp, 0, MEM_RELEASE);
                return false;
            }
            int64_t nr = (int64_t)translated - (int64_t)(uintptr_t)(dst + h.src_len);
            if (nr < INT32_MIN || nr > INT32_MAX) {
                VirtualFree(tramp, 0, MEM_RELEASE);
                return false;
            }
            int32_t nr32 = (int32_t)nr;
            std::memcpy(dst + h.rel32off, &nr32, 4);
        }
    }

    /* Trampoline tail -> first untouched original instruction. */
    tramp[tramp_bytes] = 0xFF;
    tramp[tramp_bytes + 1] = 0x25;
    *(uint32_t *)(tramp + tramp_bytes + 2) = 0;
    *(uint64_t *)(tramp + tramp_bytes + 6) = (uint64_t)(t + copied);

    /* Nearby relay -> arbitrary detour address. */
    relay[0] = 0xFF;
    relay[1] = 0x25;
    *(uint32_t *)(relay + 2) = 0;
    *(uint64_t *)(relay + 6) = (uint64_t)detour;

    int64_t relay_rel = (int64_t)(uintptr_t)relay - (int64_t)(uintptr_t)(t + 5);
    if (relay_rel < INT32_MIN || relay_rel > INT32_MAX) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }

    DWORD old;
    if (!VirtualProtect(t, (SIZE_T)copied, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    t[0] = 0xE9;
    int32_t relay_rel32 = (int32_t)relay_rel;
    std::memcpy(t + 1, &relay_rel32, 4);
    for (int i = 5; i < copied; i++) t[i] = 0x90;
    VirtualProtect(t, (SIZE_T)copied, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, (SIZE_T)copied);
    *tramp_out = tramp;
    return true;
}
#endif
