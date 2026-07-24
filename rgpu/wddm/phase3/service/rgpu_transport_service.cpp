#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

#include "../shared/rgpu_phase3_queue.h"

using namespace rgpu::phase3;

namespace {

struct Handles {
    HANDLE mapping = nullptr;
    HANDLE request_event = nullptr;
    HANDLE completion_events[kClientCount]{};
    SharedState* state = nullptr;

    ~Handles() {
        if (state) UnmapViewOfFile(state);
        for (HANDLE event : completion_events) {
            if (event) CloseHandle(event);
        }
        if (request_event) CloseHandle(request_event);
        if (mapping) CloseHandle(mapping);
    }
};

struct BatchCompletionPayload {
    uint32_t bulk_hash;
    uint32_t bulk_bytes;
    uint64_t fence_value;
};

bool open_shared(Handles& handles, uint64_t* generation) {
    handles.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                         0, sizeof(SharedState), kMappingName);
    if (!handles.mapping) return false;
    handles.state = static_cast<SharedState*>(MapViewOfFile(
        handles.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!handles.state) return false;
    handles.request_event = CreateEventW(nullptr, FALSE, FALSE, kRequestEventName);
    if (!handles.request_event) return false;
    for (uint32_t index = 0; index < kClientCount; ++index) {
        wchar_t name[96]{};
        completion_event_name(index, name, _countof(name));
        handles.completion_events[index] = CreateEventW(nullptr, FALSE, FALSE, name);
        if (!handles.completion_events[index]) return false;
    }
    if (!initialize_service_state(handles.state) || !state_valid(handles.state)) return false;

    InterlockedExchange(&handles.state->service_alive, 0);
    initialize_ring(&handles.state->requests);
    for (uint32_t index = 0; index < kClientCount; ++index) {
        ClientChannel* channel = &handles.state->clients[index];
        channel->connection_generation = 0;
        channel->heartbeat_tick = 0;
        channel->outstanding_count = 0;
        channel->outstanding_bulk_bytes = 0;
        initialize_ring(&channel->completions);
        InterlockedExchange(&channel->owner_pid, 0);
    }
    for (uint32_t index = 0; index < kBulkSlotCount; ++index) {
        release_bulk_slot(handles.state, index);
    }
    InterlockedExchange(&handles.state->device_lost, 0);
    *generation = static_cast<uint64_t>(
        InterlockedIncrement64(&handles.state->connection_generation));
    MemoryBarrier();
    InterlockedExchange(&handles.state->service_alive, 1);
    return true;
}

bool owner_process_exists(uint32_t owner_pid) {
    if (owner_pid == 0) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, owner_pid);
    if (!process) return false;
    CloseHandle(process);
    return true;
}

bool push_completion(Handles& handles, const Message& completion, DWORD timeout_ms) {
    if (completion.client_slot >= kClientCount) return false;
    ClientChannel* channel = &handles.state->clients[completion.client_slot];
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        if (try_push(&channel->completions, completion)) {
            SetEvent(handles.completion_events[completion.client_slot]);
            return true;
        }
        Sleep(1);
    }
    return false;
}

void release_batch_resources(Handles& handles, const Message& request) {
    if ((request.flags & MessageFlagHasBulk) != 0 &&
        request.bulk_slot < kBulkSlotCount) {
        release_bulk_slot(handles.state, request.bulk_slot);
    }
    if (request.client_slot < kClientCount) {
        release_process_credit(handles.state, request.client_slot, request.bulk_bytes);
    }
}

Message process_message(Handles& handles, const Message& request,
                        uint64_t* generation, bool* shutdown) {
    if (!valid_message_structure(request)) {
        return make_completion(request, Status::BadMessage, *generation);
    }
    if (request.connection_generation != *generation) {
        return make_completion(request, Status::StaleGeneration, *generation);
    }
    if (!client_owned_by(handles.state, request.client_slot,
                         request.owner_pid, *generation)) {
        return make_completion(request, Status::InvalidClient, *generation);
    }
    if (!owner_process_exists(request.owner_pid) ||
        !object_owned_by(request.object_id, request.owner_pid)) {
        return make_completion(request, Status::InvalidOwner, *generation);
    }

    InterlockedExchange64(&handles.state->clients[request.client_slot].heartbeat_tick,
                          static_cast<LONG64>(GetTickCount64()));

    switch (static_cast<Opcode>(request.opcode)) {
        case Opcode::Ping: {
            static constexpr char reply[] = "pong:transport-v2-per-process-channel";
            Message completion = make_completion(request, Status::Ok, *generation);
            completion.inline_bytes = static_cast<uint32_t>(sizeof(reply) - 1);
            std::memcpy(completion.inline_payload, reply, completion.inline_bytes);
            completion.inline_crc32 = crc32(completion.inline_payload,
                                             completion.inline_bytes);
            return completion;
        }
        case Opcode::SubmitBatch: {
            if (!validate_bulk_lease(handles.state, request)) {
                release_batch_resources(handles, request);
                return make_completion(request, Status::BulkValidationFailed,
                                       *generation);
            }
            const BulkLease& lease = handles.state->bulk_leases[request.bulk_slot];
            BatchCompletionPayload payload{
                lease.crc32 ^ 0xA5C31F72u,
                lease.bytes,
                request.fence_value,
            };
            Message completion = make_completion(request, Status::Ok, *generation);
            completion.inline_bytes = sizeof(payload);
            std::memcpy(completion.inline_payload, &payload, sizeof(payload));
            completion.inline_crc32 = crc32(completion.inline_payload,
                                             completion.inline_bytes);
            release_batch_resources(handles, request);
            return completion;
        }
        case Opcode::Cancel:
            return make_completion(request, Status::Cancelled, *generation);
        case Opcode::Reset: {
            InterlockedExchange(&handles.state->device_lost, 0);
            *generation = static_cast<uint64_t>(
                InterlockedIncrement64(&handles.state->connection_generation));
            for (uint32_t index = 0; index < kClientCount; ++index) {
                ClientChannel* channel = &handles.state->clients[index];
                if (index == request.client_slot &&
                    InterlockedCompareExchange(&channel->owner_pid, 0, 0) ==
                    static_cast<LONG>(request.owner_pid)) {
                    InterlockedExchange64(&channel->connection_generation,
                                          static_cast<LONG64>(*generation));
                } else if (InterlockedCompareExchange(&channel->owner_pid, 0, 0) != 0) {
                    InterlockedExchange64(&channel->connection_generation, 0);
                }
            }
            return make_completion(request, Status::Ok, *generation);
        }
        case Opcode::DeviceLost:
            InterlockedExchange(&handles.state->device_lost, 1);
            return make_completion(request, Status::DeviceLost, *generation);
        case Opcode::Shutdown:
            *shutdown = true;
            return make_completion(request, Status::Ok, *generation);
        default:
            return make_completion(request, Status::Unsupported, *generation);
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    DWORD idle_timeout_ms = 15000;
    if (argc == 3 && std::wstring(argv[1]) == L"--idle-timeout-ms") {
        const int requested = _wtoi(argv[2]);
        idle_timeout_ms = requested > 0 ? static_cast<DWORD>(requested) : 15000;
    }

    Handles handles;
    uint64_t generation = 0;
    if (!open_shared(handles, &generation)) {
        std::cerr << "PHASE3_SERVICE=FAIL open_shared error=" << GetLastError() << "\n";
        return 2;
    }

    uint64_t processed = 0;
    uint64_t out_of_order_pairs = 0;
    bool shutdown = false;
    bool pending_submit = false;
    Message pending_completion{};

    while (!shutdown) {
        const DWORD wait = WaitForSingleObject(handles.request_event, idle_timeout_ms);
        if (wait == WAIT_TIMEOUT) {
            std::cerr << "PHASE3_SERVICE=FAIL request_timeout processed=" << processed << "\n";
            InterlockedExchange(&handles.state->service_alive, 0);
            return 3;
        }
        if (wait != WAIT_OBJECT_0) {
            std::cerr << "PHASE3_SERVICE=FAIL wait_error=" << GetLastError() << "\n";
            InterlockedExchange(&handles.state->service_alive, 0);
            return 4;
        }

        Message request{};
        while (try_pop(&handles.state->requests, &request)) {
            Message completion = process_message(handles, request, &generation, &shutdown);
            const bool is_submit = static_cast<Opcode>(request.opcode) == Opcode::SubmitBatch &&
                                   completion.status == static_cast<int32_t>(Status::Ok);
            if (is_submit) {
                if (!pending_submit) {
                    pending_completion = completion;
                    pending_submit = true;
                } else {
                    if (!push_completion(handles, completion, 5000) ||
                        !push_completion(handles, pending_completion, 5000)) {
                        std::cerr << "PHASE3_SERVICE=FAIL completion_backpressure_timeout\n";
                        InterlockedExchange(&handles.state->service_alive, 0);
                        return 5;
                    }
                    pending_submit = false;
                    ++out_of_order_pairs;
                }
            } else {
                if (pending_submit) {
                    if (!push_completion(handles, pending_completion, 5000)) {
                        std::cerr << "PHASE3_SERVICE=FAIL pending_completion_timeout\n";
                        InterlockedExchange(&handles.state->service_alive, 0);
                        return 6;
                    }
                    pending_submit = false;
                }
                if (!push_completion(handles, completion, 5000)) {
                    std::cerr << "PHASE3_SERVICE=FAIL completion_queue_timeout\n";
                    InterlockedExchange(&handles.state->service_alive, 0);
                    return 7;
                }
            }
            ++processed;
            if (shutdown) break;
        }
    }

    if (pending_submit && !push_completion(handles, pending_completion, 5000)) {
        std::cerr << "PHASE3_SERVICE=FAIL final_pending_completion_timeout\n";
        InterlockedExchange(&handles.state->service_alive, 0);
        return 8;
    }

    InterlockedExchange(&handles.state->service_alive, 0);
    std::cout << "PHASE3_SERVICE=PASS requests=" << processed
              << " generation=" << generation
              << " control_slots=" << kControlCapacity
              << " client_channels=" << kClientCount
              << " per_client_completion_slots=" << kClientCompletionCapacity
              << " bulk_arena_bytes=" << kBulkArenaBytes
              << " out_of_order_pairs=" << out_of_order_pairs
              << " kernel_network_calls=0\n";
    return 0;
}
