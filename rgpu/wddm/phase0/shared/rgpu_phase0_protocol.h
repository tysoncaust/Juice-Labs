#pragma once
#include <cstdint>
#include <cstddef>

namespace rgpu::phase0 {
constexpr uint32_t kMagic = 0x30504752; // RGP0
constexpr uint16_t kMajor = 1;
constexpr uint16_t kMinor = 0;
constexpr uint32_t kMaxPayload = 4u * 1024u * 1024u;
constexpr uint32_t kQueueSlots = 8;

enum class Opcode : uint32_t { Hello=1, CreateSurface=2, ClearSurface=3, Present=4, Fence=5, DeviceLost=6 };
enum class Status : int32_t { Ok=0, QueueFull=-1, BadMessage=-2, DeviceLost=-3, Timeout=-4 };
#pragma pack(push,1)
struct Header { uint32_t magic; uint16_t major; uint16_t minor; uint32_t bytes; uint64_t sequence; Opcode opcode; uint32_t flags; };
struct SurfaceDesc { uint32_t width; uint32_t height; uint32_t stride; uint32_t format; };
struct ClearPayload { uint32_t surface_id; uint32_t bgra; };
struct PresentPayload { uint32_t surface_id; uint64_t fence_value; };
struct Completion { uint64_t sequence; Status status; uint64_t value; uint32_t payload_bytes; uint32_t reserved; };
#pragma pack(pop)
static_assert(sizeof(Header)==28);
static_assert(sizeof(Completion)==28);
inline bool valid_header(const Header& h) { return h.magic==kMagic && h.major==kMajor && h.bytes<=kMaxPayload; }
}
