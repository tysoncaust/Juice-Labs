#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct Args {
    std::wstring pipe_name;
    std::filesystem::path info_path;
    std::filesystem::path evidence_path;
    int duration_seconds = 15;
    int connect_timeout_seconds = 20;
};

struct FormatInfo {
    std::string ffmpeg_sample_format;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits_per_sample = 0;
    std::uint16_t block_align = 0;
};

struct CaptureStats {
    std::uint64_t packets = 0;
    std::uint64_t frames = 0;
    std::uint64_t bytes = 0;
    std::uint64_t silent_frames = 0;
    std::uint64_t continuity_silence_frames = 0;
    bool pipe_closed_by_reader = false;
};

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    HANDLE get() const { return value_; }
    explicit operator bool() const { return value_ != nullptr && value_ != INVALID_HANDLE_VALUE; }
    HANDLE release() {
        HANDLE value = value_;
        value_ = nullptr;
        return value;
    }
    void reset(HANDLE value = nullptr) {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
        value_ = value;
    }
private:
    HANDLE value_ = nullptr;
};

std::string utf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                              static_cast<int>(value.size()),
                                              nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        throw std::runtime_error("WideCharToMultiByte size query failed");
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                             static_cast<int>(value.size()),
                                             result.data(), required,
                                             nullptr, nullptr);
    if (written != required) {
        throw std::runtime_error("WideCharToMultiByte conversion failed");
    }
    return result;
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20U) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<unsigned int>(ch) << std::dec;
            } else {
                out << static_cast<char>(ch);
            }
            break;
        }
    }
    return out.str();
}

[[noreturn]] void throw_hresult(const char* operation, HRESULT hr) {
    std::ostringstream out;
    out << operation << " failed with HRESULT 0x" << std::hex
        << static_cast<unsigned long>(hr);
    throw std::runtime_error(out.str());
}

void check_hresult(const char* operation, HRESULT hr) {
    if (FAILED(hr)) {
        throw_hresult(operation, hr);
    }
}

Args parse_args(int argc, wchar_t** argv) {
    Args args;
    for (int index = 1; index < argc; ++index) {
        const std::wstring key = argv[index];
        auto require_value = [&](const wchar_t* name) -> std::wstring {
            if (index + 1 >= argc) {
                throw std::runtime_error("missing value for " + utf8(name));
            }
            ++index;
            return argv[index];
        };

        if (key == L"--pipe") {
            args.pipe_name = require_value(L"--pipe");
        } else if (key == L"--info") {
            args.info_path = require_value(L"--info");
        } else if (key == L"--evidence") {
            args.evidence_path = require_value(L"--evidence");
        } else if (key == L"--duration") {
            args.duration_seconds = std::stoi(require_value(L"--duration"));
        } else if (key == L"--connect-timeout") {
            args.connect_timeout_seconds = std::stoi(require_value(L"--connect-timeout"));
        } else {
            throw std::runtime_error("unknown argument: " + utf8(key));
        }
    }

    if (args.pipe_name.rfind(L"\\\\.\\pipe\\rgpu-", 0) != 0) {
        throw std::runtime_error("--pipe must use the \\\\.\\pipe\\rgpu-* namespace");
    }
    if (args.info_path.empty() || args.evidence_path.empty()) {
        throw std::runtime_error("--info and --evidence are required");
    }
    if (args.duration_seconds < 1 || args.duration_seconds > 3600) {
        throw std::runtime_error("--duration must be between 1 and 3600 seconds");
    }
    if (args.connect_timeout_seconds < 1 || args.connect_timeout_seconds > 120) {
        throw std::runtime_error("--connect-timeout must be between 1 and 120 seconds");
    }
    return args;
}

FormatInfo describe_format(const WAVEFORMATEX* format) {
    if (format == nullptr) {
        throw std::runtime_error("audio mix format is null");
    }

    bool is_float = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool is_pcm = format->wFormatTag == WAVE_FORMAT_PCM;

    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            throw std::runtime_error("invalid WAVEFORMATEXTENSIBLE size");
        }
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        is_float = IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
        is_pcm = IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != FALSE;
    }

    FormatInfo result;
    result.sample_rate = format->nSamplesPerSec;
    result.channels = format->nChannels;
    result.bits_per_sample = format->wBitsPerSample;
    result.block_align = format->nBlockAlign;

    if (is_float && format->wBitsPerSample == 32) {
        result.ffmpeg_sample_format = "f32le";
    } else if (is_pcm && format->wBitsPerSample == 16) {
        result.ffmpeg_sample_format = "s16le";
    } else if (is_pcm && format->wBitsPerSample == 24) {
        result.ffmpeg_sample_format = "s24le";
    } else if (is_pcm && format->wBitsPerSample == 32) {
        result.ffmpeg_sample_format = "s32le";
    } else {
        std::ostringstream out;
        out << "unsupported loopback mix format tag=" << format->wFormatTag
            << " bits=" << format->wBitsPerSample;
        throw std::runtime_error(out.str());
    }

    if (result.sample_rate == 0 || result.channels == 0 || result.block_align == 0) {
        throw std::runtime_error("invalid loopback mix format dimensions");
    }
    return result;
}

void ensure_parent(const std::filesystem::path& path) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
}

void write_ready_json(const Args& args, const FormatInfo& format) {
    ensure_parent(args.info_path);
    std::ofstream out(args.info_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to create audio info file");
    }
    out << "{\n"
        << "  \"schema\": \"rgpu-wasapi-loopback-ready-v1\",\n"
        << "  \"ready\": true,\n"
        << "  \"pipe\": \"" << json_escape(utf8(args.pipe_name)) << "\",\n"
        << "  \"ffmpeg_sample_format\": \"" << format.ffmpeg_sample_format << "\",\n"
        << "  \"sample_rate\": " << format.sample_rate << ",\n"
        << "  \"channels\": " << format.channels << ",\n"
        << "  \"bits_per_sample\": " << format.bits_per_sample << ",\n"
        << "  \"block_align\": " << format.block_align << "\n"
        << "}\n";
}

void write_evidence_json(const Args& args, const FormatInfo& format,
                         const CaptureStats& stats, bool passed,
                         const std::string& error_message) {
    ensure_parent(args.evidence_path);
    std::ofstream out(args.evidence_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to create audio evidence file");
    }
    out << "{\n"
        << "  \"schema\": \"rgpu-wasapi-loopback-evidence-v1\",\n"
        << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
        << "  \"capture\": \"WASAPI shared-mode default-render loopback\",\n"
        << "  \"ffmpeg_sample_format\": \"" << format.ffmpeg_sample_format << "\",\n"
        << "  \"sample_rate\": " << format.sample_rate << ",\n"
        << "  \"channels\": " << format.channels << ",\n"
        << "  \"bits_per_sample\": " << format.bits_per_sample << ",\n"
        << "  \"block_align\": " << format.block_align << ",\n"
        << "  \"duration_seconds\": " << args.duration_seconds << ",\n"
        << "  \"packets\": " << stats.packets << ",\n"
        << "  \"frames\": " << stats.frames << ",\n"
        << "  \"bytes\": " << stats.bytes << ",\n"
        << "  \"silent_frames\": " << stats.silent_frames << ",\n"
        << "  \"continuity_silence_frames\": " << stats.continuity_silence_frames << ",\n"
        << "  \"pipe_closed_by_reader\": "
        << (stats.pipe_closed_by_reader ? "true" : "false") << ",\n"
        << "  \"process_injection\": false,\n"
        << "  \"game_process_access\": false,\n"
        << "  \"error\": \"" << json_escape(error_message) << "\"\n"
        << "}\n";
}

bool write_all(HANDLE pipe, const std::uint8_t* data, std::size_t bytes,
               bool& pipe_closed) {
    std::size_t offset = 0;
    while (offset < bytes) {
        const std::size_t remaining = bytes - offset;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 1U << 20U));
        DWORD written = 0;
        if (!WriteFile(pipe, data + offset, chunk, &written, nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA || error == ERROR_PIPE_NOT_CONNECTED) {
                pipe_closed = true;
                return false;
            }
            std::ostringstream out;
            out << "WriteFile failed with Win32 error " << error;
            throw std::runtime_error(out.str());
        }
        if (written == 0) {
            throw std::runtime_error("WriteFile completed without progress");
        }
        offset += written;
    }
    return true;
}

CaptureStats capture_loop(const Args& args, const FormatInfo& format,
                          HANDLE pipe, IAudioClient* audio_client,
                          IAudioCaptureClient* capture_client,
                          HANDLE sample_event) {
    CaptureStats stats;
    check_hresult("IAudioClient::Start", audio_client->Start());
    const auto start_at = std::chrono::steady_clock::now();
    const auto stop_at = start_at + std::chrono::seconds(args.duration_seconds);
    std::vector<std::uint8_t> silence;

    auto write_continuity_silence = [&](std::uint64_t frames) -> bool {
        if (frames == 0) return true;
        const std::size_t bytes = static_cast<std::size_t>(frames) * format.block_align;
        silence.assign(bytes, 0U);
        bool pipe_closed = false;
        const bool wrote = write_all(pipe, silence.data(), bytes, pipe_closed);
        stats.frames += frames;
        stats.bytes += bytes;
        stats.silent_frames += frames;
        stats.continuity_silence_frames += frames;
        if (!wrote) {
            stats.pipe_closed_by_reader = pipe_closed;
        }
        return wrote;
    };

    try {
        while (std::chrono::steady_clock::now() < stop_at) {
            const DWORD wait_result = WaitForSingleObject(sample_event, 250);
            if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_TIMEOUT) {
                std::ostringstream out;
                out << "WaitForSingleObject failed with result " << wait_result;
                throw std::runtime_error(out.str());
            }

            UINT32 packet_frames = 0;
            check_hresult("IAudioCaptureClient::GetNextPacketSize",
                          capture_client->GetNextPacketSize(&packet_frames));
            while (packet_frames > 0) {
                BYTE* audio_data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                check_hresult("IAudioCaptureClient::GetBuffer",
                              capture_client->GetBuffer(&audio_data, &frames, &flags,
                                                        nullptr, nullptr));

                const std::size_t bytes = static_cast<std::size_t>(frames) * format.block_align;
                const std::uint8_t* source = audio_data;
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0U || audio_data == nullptr) {
                    silence.assign(bytes, 0U);
                    source = silence.data();
                    stats.silent_frames += frames;
                }

                bool pipe_closed = false;
                const bool wrote = write_all(pipe, source, bytes, pipe_closed);
                check_hresult("IAudioCaptureClient::ReleaseBuffer",
                              capture_client->ReleaseBuffer(frames));

                ++stats.packets;
                stats.frames += frames;
                stats.bytes += bytes;
                if (!wrote) {
                    stats.pipe_closed_by_reader = pipe_closed;
                    return stats;
                }

                check_hresult("IAudioCaptureClient::GetNextPacketSize",
                              capture_client->GetNextPacketSize(&packet_frames));
            }

            const auto now = std::chrono::steady_clock::now();
            const double elapsed_seconds =
                std::chrono::duration<double>(now - start_at).count();
            const auto expected_frames = static_cast<std::uint64_t>(
                std::min<double>(elapsed_seconds, static_cast<double>(args.duration_seconds)) *
                static_cast<double>(format.sample_rate));
            const std::uint64_t tolerance_frames =
                std::max<std::uint64_t>(1, format.sample_rate / 10U);
            if (expected_frames > stats.frames + tolerance_frames) {
                if (!write_continuity_silence(expected_frames - stats.frames)) {
                    return stats;
                }
            }
        }
    } catch (...) {
        audio_client->Stop();
        throw;
    }

    check_hresult("IAudioClient::Stop", audio_client->Stop());
    return stats;
}

int run(const Args& args) {
    check_hresult("CoInitializeEx", CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    struct CoGuard {
        ~CoGuard() { CoUninitialize(); }
    } co_guard;

    ComPtr<IMMDeviceEnumerator> enumerator;
    check_hresult("CoCreateInstance(MMDeviceEnumerator)",
                  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                   CLSCTX_ALL, IID_PPV_ARGS(&enumerator)));

    ComPtr<IMMDevice> endpoint;
    check_hresult("IMMDeviceEnumerator::GetDefaultAudioEndpoint",
                  enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint));

    ComPtr<IAudioClient> audio_client;
    check_hresult("IMMDevice::Activate(IAudioClient)",
                  endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                     nullptr, &audio_client));

    WAVEFORMATEX* raw_format = nullptr;
    check_hresult("IAudioClient::GetMixFormat", audio_client->GetMixFormat(&raw_format));
    struct FormatGuard {
        WAVEFORMATEX* value;
        ~FormatGuard() { CoTaskMemFree(value); }
    } format_guard{raw_format};
    const FormatInfo format = describe_format(raw_format);

    const REFERENCE_TIME buffer_duration = 2LL * 10'000'000LL;
    const DWORD stream_flags = AUDCLNT_STREAMFLAGS_LOOPBACK |
                               AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                               AUDCLNT_STREAMFLAGS_NOPERSIST;
    check_hresult("IAudioClient::Initialize",
                  audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                           stream_flags,
                                           buffer_duration,
                                           0,
                                           raw_format,
                                           nullptr));

    Handle sample_event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!sample_event) {
        throw std::runtime_error("CreateEventW for audio samples failed");
    }
    check_hresult("IAudioClient::SetEventHandle",
                  audio_client->SetEventHandle(sample_event.get()));

    ComPtr<IAudioCaptureClient> capture_client;
    check_hresult("IAudioClient::GetService(IAudioCaptureClient)",
                  audio_client->GetService(IID_PPV_ARGS(&capture_client)));

    Handle pipe(CreateNamedPipeW(args.pipe_name.c_str(),
                                 PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                 1,
                                 1U << 20U,
                                 1U << 20U,
                                 0,
                                 nullptr));
    if (!pipe || pipe.get() == INVALID_HANDLE_VALUE) {
        std::ostringstream out;
        out << "CreateNamedPipeW failed with Win32 error " << GetLastError();
        throw std::runtime_error(out.str());
    }

    write_ready_json(args, format);
    std::cout << "WASAPI_LOOPBACK_READY=TRUE\n"
              << "PIPE=" << utf8(args.pipe_name) << "\n"
              << "SAMPLE_FORMAT=" << format.ffmpeg_sample_format << "\n"
              << "SAMPLE_RATE=" << format.sample_rate << "\n"
              << "CHANNELS=" << format.channels << "\n";
    std::cout.flush();

    Handle connect_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!connect_event) {
        throw std::runtime_error("CreateEventW for named-pipe connection failed");
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = connect_event.get();
    BOOL connect_result = ConnectNamedPipe(pipe.get(), &overlapped);
    if (connect_result == FALSE) {
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) {
            SetEvent(connect_event.get());
        } else if (error != ERROR_IO_PENDING) {
            std::ostringstream out;
            out << "ConnectNamedPipe failed with Win32 error " << error;
            throw std::runtime_error(out.str());
        }
    }

    const DWORD wait_result = WaitForSingleObject(
        connect_event.get(), static_cast<DWORD>(args.connect_timeout_seconds * 1000));
    if (wait_result != WAIT_OBJECT_0) {
        CancelIoEx(pipe.get(), &overlapped);
        throw std::runtime_error("timed out waiting for FFmpeg to open the audio pipe");
    }

    DWORD transferred = 0;
    if (!GetOverlappedResult(pipe.get(), &overlapped, &transferred, FALSE)) {
        const DWORD error = GetLastError();
        if (error != ERROR_PIPE_CONNECTED) {
            std::ostringstream out;
            out << "GetOverlappedResult failed with Win32 error " << error;
            throw std::runtime_error(out.str());
        }
    }

    const CaptureStats stats = capture_loop(args, format, pipe.get(), audio_client.Get(),
                                             capture_client.Get(), sample_event.get());
    FlushFileBuffers(pipe.get());
    DisconnectNamedPipe(pipe.get());

    const std::uint64_t minimum_frames =
        static_cast<std::uint64_t>(format.sample_rate) *
        static_cast<std::uint64_t>(std::max(1, args.duration_seconds - 2));
    const bool passed = stats.frames >= minimum_frames && stats.bytes > 0;
    write_evidence_json(args, format, stats, passed, "");

    std::cout << "WASAPI_LOOPBACK_CAPTURE=" << (passed ? "PASS" : "FAIL") << "\n"
              << "FRAMES=" << stats.frames << "\n"
              << "BYTES=" << stats.bytes << "\n"
              << "SILENT_FRAMES=" << stats.silent_frames << "\n"
              << "CONTINUITY_SILENCE_FRAMES=" << stats.continuity_silence_frames << "\n"
              << "EVIDENCE=" << args.evidence_path.u8string() << "\n";
    return passed ? 0 : 2;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    FormatInfo unknown_format;
    CaptureStats no_stats;
    Args args;
    try {
        args = parse_args(argc, argv);
        return run(args);
    } catch (const std::exception& error) {
        std::cerr << "WASAPI_LOOPBACK_CAPTURE=FAIL\nERROR=" << error.what() << "\n";
        try {
            if (!args.evidence_path.empty()) {
                write_evidence_json(args, unknown_format, no_stats, false, error.what());
            }
        } catch (...) {
        }
        return 1;
    }
}
