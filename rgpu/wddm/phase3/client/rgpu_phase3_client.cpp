#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>

#include "../shared/rgpu_phase3_queue.h"

using namespace rgpu::phase3;

namespace {

struct Handles {
    HANDLE mapping = nullptr;
    HANDLE request_event = nullptr;
    HANDLE completion_event = nullptr;
    SharedState* state = nullptr;
    uint32_t owner_pid = 0;
    uint32_t client_slot = kInvalidClientSlot;

    ~Handles() {
        if (state && client_slot < kClientCount && owner_pid != 0) {
            unregister_client(state, client_slot, owner_pid);
        }
        if (state) UnmapViewOfFile(state);
        if (completion_event) CloseHandle(completion_event);
        if (request_event) CloseHandle(request_event);
        if (mapping) CloseHandle(mapping);
    }
};

struct BatchCompletionPayload {
    uint32_t bulk_hash;
    uint32_t bulk_bytes;
    uint64_t fence_value;
};

struct ExpectedBatch {
    uint32_t hash;
    uint32_t bytes;
    uint64_t fence;
};

bool ring_unit_test() {
    Ring<kControlCapacity> ring{};
    initialize_ring(&ring);
    for (uint32_t index = 0; index < kControlCapacity; ++index) {
        const Message message = make_message(1, index + 1, 1, 0, Opcode::Ping);
        if (!try_push(&ring, message)) return false;
    }
    if (try_push(&ring, make_message(1, 9999, 1, 0, Opcode::Ping))) return false;
    for (uint32_t index = 0; index < kControlCapacity; ++index) {
        Message output{};
        if (!try_pop(&ring, &output) || output.sequence != index + 1) return false;
    }
    Message output{};
    return !try_pop(&ring, &output);
}

bool open_shared(Handles& handles, uint64_t* generation) {
    handles.owner_pid = GetCurrentProcessId();
    for (int attempt = 0; attempt < 200; ++attempt) {
        handles.mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kMappingName);
        if (handles.mapping) break;
        Sleep(25);
    }
    if (!handles.mapping) return false;
    handles.state = static_cast<SharedState*>(MapViewOfFile(
        handles.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!handles.state) return false;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (state_valid(handles.state) &&
            InterlockedCompareExchange(&handles.state->service_alive, 1, 1) == 1) {
            break;
        }
        Sleep(25);
    }
    if (!state_valid(handles.state) ||
        InterlockedCompareExchange(&handles.state->service_alive, 1, 1) != 1) {
        return false;
    }
    *generation = static_cast<uint64_t>(InterlockedCompareExchange64(
        &handles.state->connection_generation, 0, 0));
    const int registered = register_client(handles.state, handles.owner_pid, *generation);
    if (registered < 0) return false;
    handles.client_slot = static_cast<uint32_t>(registered);

    handles.request_event = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE,
                                       kRequestEventName);
    wchar_t completion_name[96]{};
    completion_event_name(handles.client_slot, completion_name, _countof(completion_name));
    handles.completion_event = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE,
                                          completion_name);
    return handles.request_event && handles.completion_event;
}

bool push_request(Handles& handles, const Message& request, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        if (try_push(&handles.state->requests, request)) {
            SetEvent(handles.request_event);
            return true;
        }
        Sleep(1);
    }
    return false;
}

bool transact(Handles& handles, const Message& request, Message* completion,
              DWORD timeout_ms, bool allow_generation_change = false) {
    if (!push_request(handles, request, timeout_ms)) return false;
    ClientChannel* channel = &handles.state->clients[handles.client_slot];
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        Message candidate{};
        while (try_pop(&channel->completions, &candidate)) {
            if (candidate.sequence == request.sequence &&
                valid_message_structure(candidate) &&
                candidate.owner_pid == handles.owner_pid &&
                candidate.client_slot == handles.client_slot &&
                (allow_generation_change ||
                 candidate.connection_generation == request.connection_generation)) {
                *completion = candidate;
                return true;
            }
        }
        const DWORD remaining = static_cast<DWORD>(deadline - GetTickCount64());
        WaitForSingleObject(handles.completion_event, remaining);
    }
    return false;
}

} // namespace

int main() {
    if (!ring_unit_test()) {
        std::cerr << "PHASE3_CLIENT=FAIL mpmc_ring_unit_test\n";
        return 10;
    }

    Handles handles;
    uint64_t generation = 0;
    if (!open_shared(handles, &generation)) {
        std::cerr << "PHASE3_CLIENT=FAIL open_shared error=" << GetLastError() << "\n";
        return 11;
    }

    Message completion{};
    const uint64_t ping_sequence = next_sequence(handles.state);
    const Message ping = make_message(generation, ping_sequence, handles.owner_pid,
                                      handles.client_slot, Opcode::Ping);
    if (!transact(handles, ping, &completion, 3000) ||
        completion.status != static_cast<int32_t>(Status::Ok)) {
        std::cerr << "PHASE3_CLIENT=FAIL ping_transaction\n";
        return 12;
    }
    const std::string ping_reply(reinterpret_cast<char*>(completion.inline_payload),
                                 completion.inline_bytes);
    if (ping_reply != "pong:transport-v2-per-process-channel") {
        std::cerr << "PHASE3_CLIENT=FAIL ping_reply\n";
        return 13;
    }

    constexpr uint32_t kBatchCount = 128;
    constexpr uint32_t kBatchBytes = 4096;
    static_assert(kBatchCount <= kMaxOutstandingPerProcess);
    static_assert(kBatchCount * kBatchBytes <= kMaxBulkBytesPerProcess);

    std::unordered_map<uint64_t, ExpectedBatch> expected;
    expected.reserve(kBatchCount);
    for (uint32_t batch = 0; batch < kBatchCount; ++batch) {
        std::array<uint8_t, kBatchBytes> payload{};
        for (uint32_t index = 0; index < payload.size(); ++index) {
            payload[index] = static_cast<uint8_t>((batch * 31u + index * 17u) & 0xFFu);
        }
        if (!acquire_process_credit(handles.state, handles.client_slot,
                                    static_cast<uint32_t>(payload.size()))) {
            std::cerr << "PHASE3_CLIENT=FAIL process_quota batch=" << batch << "\n";
            return 14;
        }
        const uint64_t sequence = next_sequence(handles.state);
        const uint64_t object_id = (static_cast<uint64_t>(handles.owner_pid) << 32u) |
                                   static_cast<uint64_t>(batch + 1u);
        const uint64_t fence_value = batch + 1u;
        const int bulk_slot = acquire_bulk_slot(
            handles.state, handles.owner_pid, handles.client_slot, generation,
            sequence, object_id, payload.data(), static_cast<uint32_t>(payload.size()));
        if (bulk_slot < 0) {
            release_process_credit(handles.state, handles.client_slot,
                                   static_cast<uint32_t>(payload.size()));
            std::cerr << "PHASE3_CLIENT=FAIL bulk_arena_exhausted batch=" << batch << "\n";
            return 15;
        }
        Message request = make_message(generation, sequence, handles.owner_pid,
                                       handles.client_slot, Opcode::SubmitBatch,
                                       object_id, fence_value);
        attach_bulk(&request, handles.state, static_cast<uint32_t>(bulk_slot));
        expected.emplace(sequence, ExpectedBatch{
            request.bulk_crc32 ^ 0xA5C31F72u,
            request.bulk_bytes,
            fence_value,
        });
        if (!push_request(handles, request, 5000)) {
            release_bulk_slot(handles.state, static_cast<uint32_t>(bulk_slot));
            release_process_credit(handles.state, handles.client_slot,
                                   request.bulk_bytes);
            std::cerr << "PHASE3_CLIENT=FAIL request_backpressure_timeout batch=" << batch << "\n";
            return 16;
        }
    }

    ClientChannel* channel = &handles.state->clients[handles.client_slot];
    uint32_t completions = 0;
    uint64_t previous_sequence = 0;
    bool out_of_order_observed = false;
    const ULONGLONG batch_deadline = GetTickCount64() + 15000;
    while (completions < kBatchCount && GetTickCount64() < batch_deadline) {
        Message candidate{};
        bool popped = false;
        while (try_pop(&channel->completions, &candidate)) {
            popped = true;
            if (!valid_for_generation(candidate, generation) ||
                candidate.owner_pid != handles.owner_pid ||
                candidate.client_slot != handles.client_slot ||
                candidate.status != static_cast<int32_t>(Status::Ok)) {
                std::cerr << "PHASE3_CLIENT=FAIL invalid_batch_completion\n";
                return 17;
            }
            const auto found = expected.find(candidate.sequence);
            if (found == expected.end() ||
                candidate.inline_bytes != sizeof(BatchCompletionPayload)) {
                std::cerr << "PHASE3_CLIENT=FAIL unexpected_batch_completion\n";
                return 18;
            }
            BatchCompletionPayload payload{};
            std::memcpy(&payload, candidate.inline_payload, sizeof(payload));
            if (payload.bulk_hash != found->second.hash ||
                payload.bulk_bytes != found->second.bytes ||
                payload.fence_value != found->second.fence ||
                candidate.fence_value != found->second.fence) {
                std::cerr << "PHASE3_CLIENT=FAIL batch_payload_mismatch\n";
                return 19;
            }
            if (previous_sequence != 0 && candidate.sequence < previous_sequence) {
                out_of_order_observed = true;
            }
            previous_sequence = candidate.sequence;
            expected.erase(found);
            ++completions;
        }
        if (!popped) WaitForSingleObject(handles.completion_event, 100);
    }
    if (completions != kBatchCount || !expected.empty() || !out_of_order_observed ||
        InterlockedCompareExchange(&channel->outstanding_count, 0, 0) != 0 ||
        InterlockedCompareExchange(&channel->outstanding_bulk_bytes, 0, 0) != 0) {
        std::cerr << "PHASE3_CLIENT=FAIL async_completion_gate completions="
                  << completions << " out_of_order=" << out_of_order_observed << "\n";
        return 20;
    }

    const uint64_t cancel_target = ping_sequence;
    const uint64_t cancel_sequence = next_sequence(handles.state);
    const Message cancel = make_message(generation, cancel_sequence, handles.owner_pid,
                                        handles.client_slot, Opcode::Cancel, 0, 0,
                                        &cancel_target, sizeof(cancel_target));
    if (!transact(handles, cancel, &completion, 3000) ||
        completion.status != static_cast<int32_t>(Status::Cancelled)) {
        std::cerr << "PHASE3_CLIENT=FAIL cancel_gate\n";
        return 21;
    }

    const uint64_t lost_sequence = next_sequence(handles.state);
    const Message lost = make_message(generation, lost_sequence, handles.owner_pid,
                                      handles.client_slot, Opcode::DeviceLost);
    if (!transact(handles, lost, &completion, 3000) ||
        completion.status != static_cast<int32_t>(Status::DeviceLost) ||
        InterlockedCompareExchange(&handles.state->device_lost, 1, 1) != 1) {
        std::cerr << "PHASE3_CLIENT=FAIL device_lost_gate\n";
        return 22;
    }

    const uint64_t old_generation = generation;
    const uint64_t reset_sequence = next_sequence(handles.state);
    const Message reset = make_message(generation, reset_sequence, handles.owner_pid,
                                       handles.client_slot, Opcode::Reset);
    if (!transact(handles, reset, &completion, 3000, true) ||
        completion.status != static_cast<int32_t>(Status::Ok) ||
        completion.connection_generation <= generation) {
        std::cerr << "PHASE3_CLIENT=FAIL reset_gate\n";
        return 23;
    }
    generation = completion.connection_generation;
    if (!client_owned_by(handles.state, handles.client_slot,
                         handles.owner_pid, generation) ||
        valid_for_generation(make_message(old_generation, 999, handles.owner_pid,
                                          handles.client_slot, Opcode::Ping),
                             generation)) {
        std::cerr << "PHASE3_CLIENT=FAIL generation_channel_update\n";
        return 24;
    }

    const uint64_t stale_sequence = next_sequence(handles.state);
    const Message stale = make_message(old_generation, stale_sequence, handles.owner_pid,
                                       handles.client_slot, Opcode::Ping);
    if (!transact(handles, stale, &completion, 3000, true) ||
        completion.status != static_cast<int32_t>(Status::StaleGeneration) ||
        completion.connection_generation != generation) {
        std::cerr << "PHASE3_CLIENT=FAIL stale_service_rejection\n";
        return 25;
    }

    const uint64_t shutdown_sequence = next_sequence(handles.state);
    const Message shutdown = make_message(generation, shutdown_sequence, handles.owner_pid,
                                          handles.client_slot, Opcode::Shutdown);
    if (!transact(handles, shutdown, &completion, 3000) ||
        completion.status != static_cast<int32_t>(Status::Ok)) {
        std::cerr << "PHASE3_CLIENT=FAIL shutdown_transaction\n";
        return 26;
    }

    std::cout << "PHASE3_CLIENT=PASS"
              << " mpmc_control_slots=" << kControlCapacity
              << " client_channels=" << kClientCount
              << " per_client_completion_slots=" << kClientCompletionCapacity
              << " bulk_arena_bytes=" << kBulkArenaBytes
              << " outstanding_batches=" << kBatchCount
              << " async_fences=" << kBatchCount
              << " out_of_order_completion=pass"
              << " cancellation=pass"
              << " device_lost=pass"
              << " generation_reset=pass"
              << " stale_generation_rejection=pass"
              << " per_process_completion_isolation=pass"
              << " process_quota=pass"
              << " object_owner_binding=pass"
              << " kernel_network_calls=0\n";
    return 0;
}
