/* rgpu ID3D11Device wrapper — globals + tee sink. The wrapper itself is inline
 * in the header; this TU owns the counters and the single tee serializer so the
 * proxy DLL and the harness link one definition. */
#include "rgpu_d3d11_device.h"

volatile long g_rgpu_dev_wrapped   = 0;
volatile long g_rgpu_tee_texture2d = 0;
volatile long g_rgpu_tee_rtv       = 0;

RgpuDevice &rgpu_tee() {
    static RgpuDevice tee;   // captures the real game's device-level creates as protocol
    return tee;
}
