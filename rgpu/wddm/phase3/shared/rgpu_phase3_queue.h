#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace rgpu::phase3 {

inline constexpr wchar_t kMappingName[] = L"Local\\RgpuPhase3TransportV2";
inline constexpr wchar_t kRequestEventName[] = L"Local\\RgpuPhase3RequestV2";
inline constexpr uint32_t kMagic = 0x32504752u; // RGP2
inline constexpr uint16_t kVersion = 2;
inline constexpr uint32_t kControlCapacity = 256;
inline constexpr uint32_t kClientCount = 16;
inline constexpr uint32_t kClientCompletionCapacity = 128;
inline constexpr uint32_t kInlinePayloadBytes = 128;
inline constexpr uint32_t kBulkSlotCount = 128;
inline constexpr uint32_t kBulkSlotBytes = 64 * 1024;
inline constexpr uint32_t kBulkArenaBytes = kBulkSlotCount * kBulkSlotBytes;
inline constexpr uint32_t kMaxOutstandingPerProcess = 128;
inline constexpr uint32_t kMaxBulkBytesPerProcess = 4 * 1024 * 1024;
inline constexpr uint32_t kInvalidBulkSlot = 0xFFFFFFFFu;
inline constexpr uint32_t kInvalidClientSlot = 0xFFFFFFFFu;

enum class Opcode : uint32_t {
    Ping = 1,
    SubmitBatch = 2,
    Cancel = 3,
    Reset = 4,
    Shutdown = 5,
    DeviceLost = 6,
};

enum class Status : int32_t {
    Ok = 0,
    BadMessage = -1,
    Unsupported = -2,
    QueueFull = -3,
    StaleGeneration = -4,
    QuotaExceeded = -5,
    Cancelled = -6,
    DeviceLost = -7,
    InvalidOwner = -8,
    BulkValidationFailed = -9,
    InvalidClient = -10,
};

enum MessageFlags : uint32_t {
    MessageFlagNone = 0,
    MessageFlagHasBulk = 1u << 0,
    MessageFlagCompletion = 1u << 1,
};

#pragma pack(push, 8)
struct Message {
    uint32_t magic;
    uint16_t version;
    uint16_t header_bytes;
    uint32_t total_bytes;
    uint32_t flags;
    uint64_t connection_generation;
    uint64_t sequence;
    uint32_t owner_pid;
    uint32_t client_slot;
    uint32_t opcode;
    int32_t status;
    uint64_t object_id;
    uint64_t fence_value;
    uint32_t bulk_slot;
    uint32_t bulk_bytes;
    uint32_t bulk_crc32;
    uint32_t inline_bytes;
    uint32_t inline_crc32;
    uint32_t reserved;
    uint8_t inline_payload[kInlinePayloadBytes];
};
#pragma pack(pop)

static_assert(sizeof(Message) % 8 == 0, "Message must remain naturally aligned");

template <uint32_t Capacity>
struct alignas(64) RingSlot {
    volatile LONG64 sequence;
    Message message;
};

template <uint32_t Capacity>
struct alignas(64) Ring {
    volatile LONG64 enqueue_position;
    uint8_t enqueue_padding[56];
    volatile LONG64 dequeue_position;
    uint8_t dequeue_padding[56];
    RingSlot<Capacity> slots[Capacity];
};

struct alignas(64) ClientChannel {
    volatile LONG owner_pid;
    uint32_t reserved0;
    volatile LONG64 connection_generation;
    volatile LONG64 heartbeat_tick;
    volatile LONG outstanding_count;
    volatile LONG outstanding_bulk_bytes;
    uint8_t header_padding[24];
    Ring<kClientCompletionCapacity> completions;
};

struct alignas(64) BulkLease {
    volatile LONG state; // 0 free, 1 writer-owned, 2 ready for service
    uint32_t owner_pid;
    uint32_t client_slot;
    uint32_t bytes;
    uint64_t connection_generation;
    uint64_t sequence;
    uint32_t crc32;
    uint32_t reserved0;
    uint64_t object_id;
    uint8_t reserved[16];
};

struct alignas(64) SharedState {
    volatile LONG initialized; // 0 new, 1 initializing, 2 ready
    uint32_t magic;
    uint16_t version;
    uint16_t header_bytes;
    uint32_t total_bytes;
    volatile LONG service_alive;
    volatile LONG64 connection_generation;
    volatile LONG64 next_sequence;
    volatile LONG device_lost;
    uint8_t header_padding[20];
    Ring<kControlCapacity> requests;
    ClientChannel clients[kClientCount];
    BulkLease bulk_leases[kBulkSlotCount];
    alignas(64) uint8_t bulk_data[kBulkArenaBytes];
};

inline void completion_event_name(uint32_t client_slot, wchar_t* output,
                                  size_t output_count) noexcept {
    if (!output || output_count == 0) return;
    _snwprintf_s(output, output_count, _TRUNCATE,
                 L"Local\\RgpuPhase3CompletionV2_%u", client_slot);
}

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

template <uint32_t Capacity>
inline void initialize_ring(Ring<Capacity>* ring) noexcept {
    ring->enqueue_position = 0;
    ring->dequeue_position = 0;
    for (uint32_t index = 0; index < Capacity; ++index) {
        ring->slots[index].sequence = static_cast<LONG64>(index);
        std::memset(&ring->slots[index].message, 0, sizeof(Message));
    }
}

inline bool initialize_service_state(SharedState* state) noexcept {
    const LONG previous = InterlockedCompareExchange(&state->initialized, 1, 0);
    if (previous == 0) {
        std::memset(reinterpret_cast<uint8_t*>(state) + sizeof(state->initialized), 0,
                    sizeof(*state) - sizeof(state->initialized));
        state->magic = kMagic;
        state->version = kVersion;
        state->header_bytes = static_cast<uint16_t>(offsetof(SharedState, requests));
        state->total_bytes = static_cast<uint32_t>(sizeof(*state));
        state->connection_generation = 1;
        state->next_sequence = 1;
        initialize_ring(&state->requests);
        for (uint32_t index = 0; index < kClientCount; ++index) {
            initialize_ring(&state->clients[index].completions);
        }
        MemoryBarrier();
        InterlockedExchange(&state->initialized, 2);
        return true;
    }
    for (uint32_t spin = 0; spin < 5000; ++spin) {
        if (InterlockedCompareExchange(&state->initialized, 2, 2) == 2) return true;
        Sleep(1);
    }
    return false;
}

inline bool state_valid(const SharedState* state) noexcept {
    return state && state->magic == kMagic && state->version == kVersion &&
           state->total_bytes == sizeof(SharedState) &&
           InterlockedCompareExchange(
               const_cast<volatile LONG*>(&state->initialized), 2, 2) == 2;
}

template <uint32_t Capacity>
inline bool try_push(Ring<Capacity>* ring, const Message& message) noexcept {
    LONG64 position = InterlockedCompareExchange64(&ring->enqueue_position, 0, 0);
    for (;;) {
        RingSlot<Capacity>* slot = &ring->slots[static_cast<uint64_t>(position) % Capacity];
        const LONG64 sequence = InterlockedCompareExchange64(&slot->sequence, 0, 0);
        const LONG64 difference = sequence - position;
        if (difference == 0) {
            if (InterlockedCompareExchange64(&ring->enqueue_position, position + 1,
                                             position) == position) {
                slot->message = message;
                MemoryBarrier();
                InterlockedExchange64(&slot->sequence, position + 1);
                return true;
            }
        } else if (difference < 0) {
            return false;
        } else {
            position = InterlockedCompareExchange64(&ring->enqueue_position, 0, 0);
        }
    }
}

template <uint32_t Capacity>
inline bool try_pop(Ring<Capacity>* ring, Message* output) noexcept {
    LONG64 position = InterlockedCompareExchange64(&ring->dequeue_position, 0, 0);
    for (;;) {
        RingSlot<Capacity>* slot = &ring->slots[static_cast<uint64_t>(position) % Capacity];
        const LONG64 sequence = InterlockedCompareExchange64(&slot->sequence, 0, 0);
        const LONG64 difference = sequence - (position + 1);
        if (difference == 0) {
            if (InterlockedCompareExchange64(&ring->dequeue_position, position + 1,
                                             position) == position) {
                *output = slot->message;
                MemoryBarrier();
                InterlockedExchange64(&slot->sequence,
                                      position + static_cast<LONG64>(Capacity));
                return true;
            }
        } else if (difference < 0) {
            return false;
        } else {
            position = InterlockedCompareExchange64(&ring->dequeue_position, 0, 0);
        }
    }
}

inline uint64_t next_sequence(SharedState* state) noexcept {
    return static_cast<uint64_t>(InterlockedIncrement64(&state->next_sequence));
}

inline int register_client(SharedState* state, uint32_t owner_pid,
                           uint64_t generation) noexcept {
    for (uint32_t index = 0; index < kClientCount; ++index) {
        ClientChannel* channel = &state->clients[index];
        const LONG current = InterlockedCompareExchange(&channel->owner_pid, 0, 0);
        if (current == static_cast<LONG>(owner_pid) &&
            static_cast<uint64_t>(InterlockedCompareExchange64(
                &channel->connection_generation, 0, 0)) == generation) {
            InterlockedExchange64(&channel->heartbeat_tick,
                                  static_cast<LONG64>(GetTickCount64()));
            return static_cast<int>(index);
        }
        if (current == 0 && InterlockedCompareExchange(
                &channel->owner_pid, static_cast<LONG>(owner_pid), 0) == 0) {
            initialize_ring(&channel->completions);
            channel->connection_generation = static_cast<LONG64>(generation);
            channel->heartbeat_tick = static_cast<LONG64>(GetTickCount64());
            channel->outstanding_count = 0;
            channel->outstanding_bulk_bytes = 0;
            MemoryBarrier();
            return static_cast<int>(index);
        }
    }
    return -1;
}

inline void unregister_client(SharedState* state, uint32_t client_slot,
                              uint32_t owner_pid) noexcept {
    if (client_slot >= kClientCount) return;
    ClientChannel* channel = &state->clients[client_slot];
    if (InterlockedCompareExchange(&channel->owner_pid, 0, 0) !=
        static_cast<LONG>(owner_pid)) return;
    channel->connection_generation = 0;
    channel->heartbeat_tick = 0;
    channel->outstanding_count = 0;
    channel->outstanding_bulk_bytes = 0;
    initialize_ring(&channel->completions);
    MemoryBarrier();
    InterlockedExchange(&channel->owner_pid, 0);
}

inline bool client_owned_by(const SharedState* state, uint32_t client_slot,
                            uint32_t owner_pid, uint64_t generation) noexcept {
    if (client_slot >= kClientCount) return false;
    const ClientChannel& channel = state->clients[client_slot];
    return InterlockedCompareExchange(
               const_cast<volatile LONG*>(&channel.owner_pid), 0, 0) ==
               static_cast<LONG>(owner_pid) &&
           static_cast<uint64_t>(InterlockedCompareExchange64(
               const_cast<volatile LONG64*>(&channel.connection_generation), 0, 0)) ==
               generation;
}

inline bool acquire_process_credit(SharedState* state, uint32_t client_slot,
                                   uint32_t bulk_bytes) noexcept {
    if (client_slot >= kClientCount || bulk_bytes > kBulkSlotBytes) return false;
    ClientChannel* channel = &state->clients[client_slot];
    const LONG outstanding = InterlockedIncrement(&channel->outstanding_count);
    if (outstanding > static_cast<LONG>(kMaxOutstandingPerProcess)) {
        InterlockedDecrement(&channel->outstanding_count);
        return false;
    }
    const LONG total_bulk = InterlockedExchangeAdd(
        &channel->outstanding_bulk_bytes, static_cast<LONG>(bulk_bytes)) +
        static_cast<LONG>(bulk_bytes);
    if (total_bulk > static_cast<LONG>(kMaxBulkBytesPerProcess)) {
        InterlockedExchangeAdd(&channel->outstanding_bulk_bytes,
                               -static_cast<LONG>(bulk_bytes));
        InterlockedDecrement(&channel->outstanding_count);
        return false;
    }
    return true;
}

inline void release_process_credit(SharedState* state, uint32_t client_slot,
                                   uint32_t bulk_bytes) noexcept {
    if (client_slot >= kClientCount) return;
    ClientChannel* channel = &state->clients[client_slot];
    InterlockedExchangeAdd(&channel->outstanding_bulk_bytes,
                           -static_cast<LONG>(bulk_bytes));
    InterlockedDecrement(&channel->outstanding_count);
}

inline Message make_message(uint64_t generation, uint64_t sequence,
                            uint32_t owner_pid, uint32_t client_slot,
                            Opcode opcode, uint64_t object_id = 0,
                            uint64_t fence_value = 0,
                            const void* inline_payload = nullptr,
                            uint32_t inline_bytes = 0) noexcept {
    Message message{};
    message.magic = kMagic;
    message.version = kVersion;
    message.header_bytes = static_cast<uint16_t>(offsetof(Message, inline_payload));
    message.total_bytes = sizeof(Message);
    message.connection_generation = generation;
    message.sequence = sequence;
    message.owner_pid = owner_pid;
    message.client_slot = client_slot;
    message.opcode = static_cast<uint32_t>(opcode);
    message.status = static_cast<int32_t>(Status::Ok);
    message.object_id = object_id;
    message.fence_value = fence_value;
    message.bulk_slot = kInvalidBulkSlot;
    message.inline_bytes = inline_bytes > kInlinePayloadBytes ? kInlinePayloadBytes : inline_bytes;
    if (inline_payload && message.inline_bytes) {
        std::memcpy(message.inline_payload, inline_payload, message.inline_bytes);
    }
    message.inline_crc32 = crc32(message.inline_payload, message.inline_bytes);
    return message;
}

inline bool valid_message_structure(const Message& message) noexcept {
    return message.magic == kMagic && message.version == kVersion &&
           message.header_bytes == offsetof(Message, inline_payload) &&
           message.total_bytes == sizeof(Message) &&
           message.client_slot < kClientCount &&
           message.inline_bytes <= kInlinePayloadBytes &&
           message.inline_crc32 == crc32(message.inline_payload, message.inline_bytes) &&
           ((message.flags & MessageFlagHasBulk) == 0 ||
            (message.bulk_slot < kBulkSlotCount && message.bulk_bytes <= kBulkSlotBytes));
}

inline bool valid_for_generation(const Message& message, uint64_t generation) noexcept {
    return valid_message_structure(message) &&
           message.connection_generation == generation;
}

inline bool object_owned_by(uint64_t object_id, uint32_t owner_pid) noexcept {
    return object_id == 0 || static_cast<uint32_t>(object_id >> 32) == owner_pid;
}

inline int acquire_bulk_slot(SharedState* state, uint32_t owner_pid,
                             uint32_t client_slot, uint64_t generation,
                             uint64_t sequence, uint64_t object_id,
                             const void* data, uint32_t bytes) noexcept {
    if (!data || bytes == 0 || bytes > kBulkSlotBytes ||
        !object_owned_by(object_id, owner_pid) ||
        !client_owned_by(state, client_slot, owner_pid, generation)) {
        return -1;
    }
    const uint32_t start = static_cast<uint32_t>(sequence % kBulkSlotCount);
    for (uint32_t offset = 0; offset < kBulkSlotCount; ++offset) {
        const uint32_t index = (start + offset) % kBulkSlotCount;
        BulkLease* lease = &state->bulk_leases[index];
        if (InterlockedCompareExchange(&lease->state, 1, 0) == 0) {
            lease->owner_pid = owner_pid;
            lease->client_slot = client_slot;
            lease->connection_generation = generation;
            lease->sequence = sequence;
            lease->bytes = bytes;
            lease->object_id = object_id;
            uint8_t* destination = state->bulk_data +
                                   static_cast<size_t>(index) * kBulkSlotBytes;
            std::memcpy(destination, data, bytes);
            lease->crc32 = crc32(destination, bytes);
            MemoryBarrier();
            InterlockedExchange(&lease->state, 2);
            return static_cast<int>(index);
        }
    }
    return -1;
}

inline bool validate_bulk_lease(const SharedState* state, const Message& message) noexcept {
    if ((message.flags & MessageFlagHasBulk) == 0) return message.bulk_slot == kInvalidBulkSlot;
    if (message.bulk_slot >= kBulkSlotCount || message.bulk_bytes == 0 ||
        message.bulk_bytes > kBulkSlotBytes) return false;
    const BulkLease* lease = &state->bulk_leases[message.bulk_slot];
    if (InterlockedCompareExchange(
            const_cast<volatile LONG*>(&lease->state), 2, 2) != 2) return false;
    if (lease->owner_pid != message.owner_pid ||
        lease->client_slot != message.client_slot ||
        lease->connection_generation != message.connection_generation ||
        lease->sequence != message.sequence || lease->bytes != message.bulk_bytes ||
        lease->object_id != message.object_id || lease->crc32 != message.bulk_crc32) {
        return false;
    }
    const uint8_t* data = state->bulk_data +
                          static_cast<size_t>(message.bulk_slot) * kBulkSlotBytes;
    return crc32(data, message.bulk_bytes) == message.bulk_crc32;
}

inline void release_bulk_slot(SharedState* state, uint32_t slot) noexcept {
    if (slot >= kBulkSlotCount) return;
    BulkLease* lease = &state->bulk_leases[slot];
    std::memset(reinterpret_cast<uint8_t*>(lease) + sizeof(lease->state), 0,
                sizeof(*lease) - sizeof(lease->state));
    MemoryBarrier();
    InterlockedExchange(&lease->state, 0);
}

inline void attach_bulk(Message* message, const SharedState* state,
                        uint32_t slot) noexcept {
    const BulkLease& lease = state->bulk_leases[slot];
    message->flags |= MessageFlagHasBulk;
    message->bulk_slot = slot;
    message->bulk_bytes = lease.bytes;
    message->bulk_crc32 = lease.crc32;
}

inline Message make_completion(const Message& request, Status status,
                               uint64_t generation) noexcept {
    Message completion = make_message(generation, request.sequence, request.owner_pid,
                                      request.client_slot,
                                      static_cast<Opcode>(request.opcode),
                                      request.object_id, request.fence_value);
    completion.flags |= MessageFlagCompletion;
    completion.status = static_cast<int32_t>(status);
    return completion;
}

} // namespace rgpu::phase3
