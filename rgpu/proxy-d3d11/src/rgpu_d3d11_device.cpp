/* rgpu D3D11 wrapper globals + tee sink. The device/context wrappers are inline
 * in their headers; this TU owns the counters and the single tee serializer so
 * the proxy DLL and the harness link one definition. */
#include "rgpu_tee.h"

volatile long g_rgpu_dev_wrapped   = 0;
volatile long g_rgpu_tee_texture2d = 0;
volatile long g_rgpu_tee_rtv       = 0;
volatile long g_rgpu_ctx_wrapped   = 0;
volatile long g_rgpu_tee_draws     = 0;
volatile long g_rgpu_tee_clears    = 0;
volatile long g_rgpu_tee_state     = 0;

RgpuDevice &rgpu_tee() {
    static RgpuDevice tee;   // captures the game's device + per-frame calls as protocol
    return tee;
}
