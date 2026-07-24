#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace rgpu::phase3 {

inline constexpr wchar_t kMappingName[] = L"Local\\RgpuPhase3QueueV1";
inline constexpr wchar_t kRequestEventName[] = L"Local\\RgpuPhase3RequestV1";
inline constexpr wchar_t kCompletionEventName[] = L"Local\\RgpuPhase3CompletionV1";
inline constexpr uint32_t kMagic = 0x33504752u; // RGP3
inline constexpr uint16_t kVersion = 1;
inline constexpr LONG kCapacity = 16;
inline constexpr uint32_t kPayloadBytes = 192;

enum class Opcode : uint32_t {
    Ping = 1,
    SubmitMetadata = 2,
    Shutdown = 3,
};

enum class Status : int32_t {
    Ok = 0,
    BadMessage = -1,
    Unsupported = -2,
};

#pragma pack(push, 1)
struct Message {
    uint32_t magic;
    uint16_t version;
    uint16_t bytes;
    uint64_t sequence;
    uint32_t opcode;
    int32_t status;
    uint32_t payload_bytes;
    uint32_t payload_crc32;
    uint8_t payload[kPayloadBytes];
};
#pragma pack(pop)

struct Queue {
    volatile LONG write_index;
    volatile LONG read_index;
    Message slots[kCapacity];
};

struct SharedState {
    uint32_t magic;
    uint16_t version;
    uint16_t bytes;
    volatile LONG initialized;
    volatile LONG service_alive;
    Queue requests;
    Queue completions;
};

inline uint32_t crc32(const uint8_t* data, size_t size) noexcept {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = static_cast<uint32_t>(-
                static_cast<int32_t>(crc & 1u));
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

inline void initialize(SharedState* state) noexcept {
    if (InterlockedCompareExchange(&state->initialized, 1, 0) == 0) {
        std::memset(state, 0, sizeof(*state));
        state->magic = kMagic;
        state->version = kVersion;
        state->bytes = static_cast<uint16_t>(sizeof(*state));
        MemoryBarrier();
        InterlockedExchange(&state->initialized, 2);
    } else {
        while (InterlockedCompareExchange(&state->initialized, 2, 2) != 2) {
            Sleep(0);
        }
    }
}

inline bool valid(const Message& message) noexcept {
    return message.magic == kMagic && message.version == kVersion &&
           message.bytes == sizeof(Message) &&
           message.payload_bytes <= kPayloadBytes &&
           message.payload_crc32 == crc32(message.payload, message.payload_bytes);
}

inline bool try_push(Queue* queue, const Message& message) noexcept {
    const LONG write = InterlockedCompareExchange(&queue->write_index, 0, 0);
    const LONG read = InterlockedCompareExchange(&queue->read_index, 0, 0);
    const LONG next = (write + 1) % kCapacity;
    if (next == read) {
        return false;
    }
    queue->slots[write] = message;
    MemoryBarrier();
    InterlockedExchange(&queue->write_index, next);
    return true;
}

inline bool try_pop(Queue* queue, Message* output) noexcept {
    const LONG read = InterlockedCompareExchange(&queue->read_index, 0, 0);
    const LONG write = InterlockedCompareExchange(&queue->write_index, 0, 0);
    if (read == write) {
        return false;
    }
    *output = queue->slots[read];
    MemoryBarrier();
    InterlockedExchange(&queue->read_index, (read + 1) % kCapacity);
    return true;
}

inline Message make_message(uint64_t sequence, Opcode opcode,
                            const void* payload = nullptr,
                            uint32_t payload_bytes = 0) noexcept {
    Message message{};
    message.magic = kMagic;
    message.version = kVersion;
    message.bytes = static_cast<uint16_t>(sizeof(Message));
    message.sequence = sequence;
    message.opcode = static_cast<uint32_t>(opcode);
    message.status = static_cast<int32_t>(Status::Ok);
    message.payload_bytes = payload_bytes > kPayloadBytes ? kPayloadBytes : payload_bytes;
    if (payload && message.payload_bytes) {
        std::memcpy(message.payload, payload, message.payload_bytes);
    }
    message.payload_crc32 = crc32(message.payload, message.payload_bytes);
    return message;
}

} // namespace rgpu::phase3
