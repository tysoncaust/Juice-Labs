/* rgpu tee sink shared by the device + context wrappers.
 *
 * The pass-through wrappers forward every call to the real object (so the game
 * renders correctly on the local GPU) AND record the covered subset into this
 * one protocol serializer, proving the game's device-creation and per-frame
 * command flow is captured into the rgpu wire format. Bound resources are
 * recorded by a pointer-derived object id; full COM-pointer->id object-graph
 * translation (so a remote renderer can resolve them) is the resource-tracking
 * layer that follows — the same handle-translation problem D3D12 has. */
#ifndef RGPU_TEE_H
#define RGPU_TEE_H
#include "rgpu_serializer.h"
#include <cstdint>

/* device-level */
extern volatile long g_rgpu_dev_wrapped;        /* real devices wrapped */
extern volatile long g_rgpu_tee_texture2d;      /* RT textures tee'd */
extern volatile long g_rgpu_tee_rtv;            /* RTVs tee'd */
/* context-level */
extern volatile long g_rgpu_ctx_wrapped;        /* immediate contexts wrapped */
extern volatile long g_rgpu_tee_draws;          /* Draw/Dispatch calls tee'd */
extern volatile long g_rgpu_tee_clears;         /* Clear* calls tee'd */
extern volatile long g_rgpu_tee_state;          /* state-setting calls tee'd (e.g. OMSetRenderTargets) */

RgpuDevice &rgpu_tee();                          /* the single serializer capturing the tee */

/* pointer -> stable-ish object id placeholder for the tee (low 32 bits). */
static inline uint32_t rgpu_obj_id(const void *p) { return (uint32_t)(uintptr_t)p; }

#endif
