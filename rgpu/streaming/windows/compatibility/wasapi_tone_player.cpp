#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Args {
    int duration_seconds = 5;
    double frequency_hz = 997.0;
    double amplitude = 0.03;
    std::filesystem::path evidence_path;
};

struct FormatInfo {
    enum class Kind { Float32, Pcm16, Pcm24, Pcm32 } kind = Kind::Float32;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits_per_sample = 0;
    std::uint16_t block_align = 0;
    std::string name;
};

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { if (value_) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
private:
    HANDLE value_ = nullptr;
};

[[noreturn]] void throw_hresult(const char* operation, HRESULT hr) {
    std::ostringstream out;
    out << operation << " failed with HRESULT 0x" << std::hex
        << static_cast<unsigned long>(hr);
    throw std::runtime_error(out.str());
}

void check_hresult(const char* operation, HRESULT hr) {
    if (FAILED(hr)) throw_hresult(operation, hr);
}

Args parse_args(int argc, wchar_t** argv) {
    Args args;
    for (int index = 1; index < argc; ++index) {
        const std::wstring key = argv[index];
        auto value = [&]() -> std::wstring {
            if (index + 1 >= argc) throw std::runtime_error("missing argument value");
            return argv[++index];
        };
        if (key == L"--duration") {
            args.duration_seconds = std::stoi(value());
        } else if (key == L"--frequency") {
            args.frequency_hz = std::stod(value());
        } else if (key == L"--amplitude") {
            args.amplitude = std::stod(value());
        } else if (key == L"--evidence") {
            args.evidence_path = value();
        } else {
            throw std::runtime_error("unknown argument");
        }
    }
    if (args.duration_seconds < 1 || args.duration_seconds > 300) {
        throw std::runtime_error("duration must be between 1 and 300 seconds");
    }
    if (args.frequency_hz < 20.0 || args.frequency_hz > 20000.0) {
        throw std::runtime_error("frequency is outside the audible range");
    }
    if (args.amplitude <= 0.0 || args.amplitude > 0.25) {
        throw std::runtime_error("amplitude must be above 0 and no more than 0.25");
    }
    return args;
}

FormatInfo describe_format(const WAVEFORMATEX* format) {
    if (!format) throw std::runtime_error("null mix format");
    bool is_float = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool is_pcm = format->wFormatTag == WAVE_FORMAT_PCM;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            throw std::runtime_error("invalid extensible format");
        }
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        is_float = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
        is_pcm = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != FALSE;
    }
    FormatInfo result;
    result.sample_rate = format->nSamplesPerSec;
    result.channels = format->nChannels;
    result.bits_per_sample = format->wBitsPerSample;
    result.block_align = format->nBlockAlign;
    if (is_float && format->wBitsPerSample == 32) {
        result.kind = FormatInfo::Kind::Float32;
        result.name = "f32le";
    } else if (is_pcm && format->wBitsPerSample == 16) {
        result.kind = FormatInfo::Kind::Pcm16;
        result.name = "s16le";
    } else if (is_pcm && format->wBitsPerSample == 24) {
        result.kind = FormatInfo::Kind::Pcm24;
        result.name = "s24le";
    } else if (is_pcm && format->wBitsPerSample == 32) {
        result.kind = FormatInfo::Kind::Pcm32;
        result.name = "s32le";
    } else {
        throw std::runtime_error("unsupported default render mix format");
    }
    return result;
}

void write_sample(std::uint8_t* target, FormatInfo::Kind kind, double value) {
    value = std::clamp(value, -1.0, 1.0);
    switch (kind) {
    case FormatInfo::Kind::Float32: {
        const float sample = static_cast<float>(value);
        std::memcpy(target, &sample, sizeof(sample));
        break;
    }
    case FormatInfo::Kind::Pcm16: {
        const auto sample = static_cast<std::int16_t>(
            std::llround(value * static_cast<double>(std::numeric_limits<std::int16_t>::max())));
        std::memcpy(target, &sample, sizeof(sample));
        break;
    }
    case FormatInfo::Kind::Pcm24: {
        const auto sample = static_cast<std::int32_t>(std::llround(value * 8388607.0));
        target[0] = static_cast<std::uint8_t>(sample & 0xff);
        target[1] = static_cast<std::uint8_t>((sample >> 8) & 0xff);
        target[2] = static_cast<std::uint8_t>((sample >> 16) & 0xff);
        break;
    }
    case FormatInfo::Kind::Pcm32: {
        const auto sample = static_cast<std::int32_t>(
            std::llround(value * static_cast<double>(std::numeric_limits<std::int32_t>::max())));
        std::memcpy(target, &sample, sizeof(sample));
        break;
    }
    }
}

std::size_t bytes_per_sample(FormatInfo::Kind kind) {
    switch (kind) {
    case FormatInfo::Kind::Float32: return 4;
    case FormatInfo::Kind::Pcm16: return 2;
    case FormatInfo::Kind::Pcm24: return 3;
    case FormatInfo::Kind::Pcm32: return 4;
    }
    return 0;
}

void fill_tone(BYTE* data, UINT32 frames, const FormatInfo& format,
               double frequency, double amplitude, std::uint64_t start_frame) {
    const std::size_t sample_bytes = bytes_per_sample(format.kind);
    for (UINT32 frame = 0; frame < frames; ++frame) {
        const double phase = 2.0 * kPi * frequency *
            static_cast<double>(start_frame + frame) / static_cast<double>(format.sample_rate);
        const double sample = amplitude * std::sin(phase);
        for (std::uint16_t channel = 0; channel < format.channels; ++channel) {
            const std::size_t offset = static_cast<std::size_t>(frame) * format.block_align +
                                       static_cast<std::size_t>(channel) * sample_bytes;
            write_sample(data + offset, format.kind, sample);
        }
    }
}

void write_evidence(const Args& args, const FormatInfo& format,
                    std::uint64_t frames, bool passed, const std::string& error) {
    if (args.evidence_path.empty()) return;
    if (args.evidence_path.has_parent_path()) {
        std::filesystem::create_directories(args.evidence_path.parent_path());
    }
    std::ofstream out(args.evidence_path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("failed to write tone evidence");
    out << "{\n"
        << "  \"schema\": \"rgpu-wasapi-tone-v1\",\n"
        << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
        << "  \"render\": \"WASAPI shared-mode default-render endpoint\",\n"
        << "  \"sample_format\": \"" << format.name << "\",\n"
        << "  \"sample_rate\": " << format.sample_rate << ",\n"
        << "  \"channels\": " << format.channels << ",\n"
        << "  \"duration_seconds\": " << args.duration_seconds << ",\n"
        << "  \"frequency_hz\": " << args.frequency_hz << ",\n"
        << "  \"amplitude\": " << args.amplitude << ",\n"
        << "  \"frames_rendered\": " << frames << ",\n"
        << "  \"game_process_access\": false,\n"
        << "  \"error\": \"" << error << "\"\n"
        << "}\n";
}

int run(const Args& args) {
    check_hresult("CoInitializeEx", CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    struct Guard { ~Guard() { CoUninitialize(); } } guard;

    ComPtr<IMMDeviceEnumerator> enumerator;
    check_hresult("CoCreateInstance", CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                  CLSCTX_ALL, IID_PPV_ARGS(&enumerator)));
    ComPtr<IMMDevice> endpoint;
    check_hresult("GetDefaultAudioEndpoint", enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint));
    ComPtr<IAudioClient> client;
    check_hresult("Activate IAudioClient", endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                  nullptr, &client));

    WAVEFORMATEX* raw = nullptr;
    check_hresult("GetMixFormat", client->GetMixFormat(&raw));
    struct FormatGuard { WAVEFORMATEX* value; ~FormatGuard() { CoTaskMemFree(value); } } format_guard{raw};
    const FormatInfo format = describe_format(raw);

    const REFERENCE_TIME duration = 2LL * 10'000'000LL;
    check_hresult("IAudioClient::Initialize", client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                  duration, 0, raw, nullptr));
    Handle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!event) throw std::runtime_error("CreateEventW failed");
    check_hresult("SetEventHandle", client->SetEventHandle(event.get()));

    UINT32 buffer_frames = 0;
    check_hresult("GetBufferSize", client->GetBufferSize(&buffer_frames));
    ComPtr<IAudioRenderClient> render;
    check_hresult("GetService IAudioRenderClient", client->GetService(IID_PPV_ARGS(&render)));

    BYTE* initial = nullptr;
    check_hresult("GetBuffer initial", render->GetBuffer(buffer_frames, &initial));
    fill_tone(initial, buffer_frames, format, args.frequency_hz, args.amplitude, 0);
    check_hresult("ReleaseBuffer initial", render->ReleaseBuffer(buffer_frames, 0));
    std::uint64_t rendered_frames = buffer_frames;

    check_hresult("IAudioClient::Start", client->Start());
    const auto stop_at = std::chrono::steady_clock::now() + std::chrono::seconds(args.duration_seconds);
    while (std::chrono::steady_clock::now() < stop_at) {
        const DWORD wait = WaitForSingleObject(event.get(), 500);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) {
            throw std::runtime_error("audio render event wait failed");
        }
        UINT32 padding = 0;
        check_hresult("GetCurrentPadding", client->GetCurrentPadding(&padding));
        if (padding > buffer_frames) throw std::runtime_error("invalid render padding");
        const UINT32 available = buffer_frames - padding;
        if (available == 0) continue;
        BYTE* data = nullptr;
        check_hresult("GetBuffer", render->GetBuffer(available, &data));
        fill_tone(data, available, format, args.frequency_hz, args.amplitude, rendered_frames);
        check_hresult("ReleaseBuffer", render->ReleaseBuffer(available, 0));
        rendered_frames += available;
    }
    check_hresult("IAudioClient::Stop", client->Stop());

    const std::uint64_t minimum = static_cast<std::uint64_t>(format.sample_rate) *
                                  static_cast<std::uint64_t>(std::max(1, args.duration_seconds - 1));
    const bool passed = rendered_frames >= minimum;
    write_evidence(args, format, rendered_frames, passed, "");
    std::cout << "WASAPI_TONE_RENDER=" << (passed ? "PASS" : "FAIL") << "\n"
              << "SAMPLE_FORMAT=" << format.name << "\n"
              << "SAMPLE_RATE=" << format.sample_rate << "\n"
              << "CHANNELS=" << format.channels << "\n"
              << "FRAMES_RENDERED=" << rendered_frames << "\n";
    return passed ? 0 : 2;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    Args args;
    FormatInfo empty;
    try {
        args = parse_args(argc, argv);
        return run(args);
    } catch (const std::exception& error) {
        std::cerr << "WASAPI_TONE_RENDER=FAIL\nERROR=" << error.what() << "\n";
        try { write_evidence(args, empty, 0, false, error.what()); } catch (...) {}
        return 1;
    }
}
