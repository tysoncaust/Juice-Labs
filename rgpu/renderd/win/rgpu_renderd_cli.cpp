/* rgpu-renderd-win stdio CLI: reads one rgpu COMMAND_BATCH from stdin (binary),
 * executes it on the real D3D11 renderer, and writes the presented frame to
 * stdout as: [uint32 width][uint32 height][rgba bytes]. Lets the transport agent
 * drive the native renderer for a genuine networked frame. */
#include "rgpu_renderd_win.h"
#include <cstdio>
#include <cstdint>
#include <vector>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::vector<uint8_t> in; uint8_t buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) in.insert(in.end(), buf, buf + n);
    RgpuFrame frame; std::string err;
    if (!rgpu_render_batch(in.data(), (uint32_t)in.size(), frame, err)) {
        std::fprintf(stderr, "rgpu_renderd_cli: %s\n", err.c_str());
        return 1;
    }
    fwrite(&frame.width, 4, 1, stdout);
    fwrite(&frame.height, 4, 1, stdout);
    fwrite(frame.rgba.data(), 1, frame.rgba.size(), stdout);
    return 0;
}
