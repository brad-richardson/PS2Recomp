#include "ps2_snd_audio_output.h"

#include "ps2_snd_spike.h"
#include "raylib.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

namespace
{
    constexpr uint32_t kSourceRate = 48000u; // SNDDRV output rate (AU9: tag-1 upsampled 3->4)
    constexpr size_t kWavLimitBytes = 200000000u;

    struct Output
    {
        AudioStream stream{};
        bool ready = false;
        uint32_t rate = kSourceRate;
        std::string wavPath;
        std::fstream wavFile;
        size_t wavBytes = 0;
        double phase = 0.0;
        uint32_t previous = 0;
        uint32_t next = 0;
        bool havePair = false;
        std::chrono::steady_clock::time_point statsAt{};
        uint64_t lastUnderruns = 0;
        uint64_t lastOverflows = 0;
    } g_output;

    int16_t unpackLeft(uint32_t frame) { return static_cast<int16_t>(frame & 0xffffu); }
    int16_t unpackRight(uint32_t frame) { return static_cast<int16_t>(frame >> 16); }

    void recordWav(const int16_t *samples, size_t count)
    {
        if (!g_output.wavFile.is_open())
            return;
        const size_t writable = std::min(count * sizeof(int16_t), kWavLimitBytes - g_output.wavBytes);
        if (!writable)
            return;
        g_output.wavFile.seekp(44 + static_cast<std::streamoff>(g_output.wavBytes));
        g_output.wavFile.write(reinterpret_cast<const char *>(samples), writable);
        g_output.wavBytes += writable;
        // The wall-capped boot harness terminates the runner with SIGTERM.
        // Keep the on-disk header valid after every callback for that path.
        g_output.wavFile.seekp(4);
        const uint32_t riffBytes = 36u + static_cast<uint32_t>(g_output.wavBytes);
        const char riff[4] = {static_cast<char>(riffBytes), static_cast<char>(riffBytes >> 8),
                              static_cast<char>(riffBytes >> 16), static_cast<char>(riffBytes >> 24)};
        g_output.wavFile.write(riff, sizeof(riff));
        g_output.wavFile.seekp(40);
        const uint32_t dataBytes = static_cast<uint32_t>(g_output.wavBytes);
        const char data[4] = {static_cast<char>(dataBytes), static_cast<char>(dataBytes >> 8),
                              static_cast<char>(dataBytes >> 16), static_cast<char>(dataBytes >> 24)};
        g_output.wavFile.write(data, sizeof(data));
        g_output.wavFile.flush();
    }

    bool nextInputFrame(uint32_t &frame)
    {
        if (ps2_snd_spike::pcmRing().pop(frame))
            return true;
        ps2_snd_spike::pcmRing().noteUnderrun();
        frame = 0;
        return false;
    }

    void audioCallback(void *buffer, unsigned int frames)
    {
        auto *output = static_cast<int16_t *>(buffer);
        const size_t samples = static_cast<size_t>(frames) * 2u;
        if (g_output.rate == kSourceRate)
        {
            for (unsigned int i = 0; i < frames; ++i)
            {
                uint32_t frame = 0;
                nextInputFrame(frame);
                output[2u * i] = unpackLeft(frame);
                output[2u * i + 1u] = unpackRight(frame);
            }
        }
        else
        {
            for (unsigned int i = 0; i < frames; ++i)
            {
                if (!g_output.havePair)
                {
                    nextInputFrame(g_output.previous);
                    nextInputFrame(g_output.next);
                    g_output.havePair = true;
                }
                const double t = g_output.phase;
                const auto interpolate = [t](int16_t a, int16_t b)
                {
                    return static_cast<int16_t>(std::clamp(
                        static_cast<int>(a + (b - a) * t), -32768, 32767));
                };
                output[2u * i] = interpolate(unpackLeft(g_output.previous), unpackLeft(g_output.next));
                output[2u * i + 1u] = interpolate(unpackRight(g_output.previous), unpackRight(g_output.next));
                g_output.phase += static_cast<double>(kSourceRate) / g_output.rate;
                while (g_output.phase >= 1.0)
                {
                    g_output.previous = g_output.next;
                    nextInputFrame(g_output.next);
                    g_output.phase -= 1.0;
                }
            }
        }
        recordWav(output, samples);

        const auto now = std::chrono::steady_clock::now();
        if (g_output.statsAt == std::chrono::steady_clock::time_point{})
            g_output.statsAt = now;
        if (now - g_output.statsAt >= std::chrono::minutes(1))
        {
            const uint64_t underruns = ps2_snd_spike::pcmRing().underruns();
            const uint64_t overflows = ps2_snd_spike::pcmRing().overflows();
            std::cerr << "[snd-output] wall-minute underruns=" << (underruns - g_output.lastUnderruns)
                      << " overflows=" << (overflows - g_output.lastOverflows)
                      << " total-underruns=" << underruns << " total-overflows=" << overflows << '\n';
            g_output.lastUnderruns = underruns;
            g_output.lastOverflows = overflows;
            g_output.statsAt = now;
        }
    }

    void writeU16(std::ostream &file, uint16_t value)
    {
        const char bytes[2] = {static_cast<char>(value), static_cast<char>(value >> 8)};
        file.write(bytes, sizeof(bytes));
    }

    void writeU32(std::ostream &file, uint32_t value)
    {
        const char bytes[4] = {static_cast<char>(value), static_cast<char>(value >> 8),
                               static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
        file.write(bytes, sizeof(bytes));
    }

    void saveWav()
    {
        if (g_output.wavPath.empty())
            return;
        if (g_output.wavFile.is_open())
            g_output.wavFile.close();
        if (g_output.wavFile.fail() || g_output.wavBytes == 0)
            std::cerr << "[snd-output] failed writing WAV: " << g_output.wavPath << '\n';
        else
            std::cerr << "[snd-output] WAV " << g_output.wavPath << " bytes=" << g_output.wavBytes
                      << " rate=" << g_output.rate << '\n';
    }
}

namespace ps2_snd_audio_output
{
bool initialize()
{
    if (!IsAudioDeviceReady())
        return false;
    g_output.stream = LoadAudioStream(kSourceRate, 16, 2);
    if (!IsAudioStreamValid(g_output.stream))
    {
        g_output.rate = 48000u;
        g_output.stream = LoadAudioStream(g_output.rate, 16, 2);
        if (!IsAudioStreamValid(g_output.stream))
            return false;
        std::cerr << "[snd-output] 48 kHz stream unavailable\n";
    }
    SetAudioStreamCallback(g_output.stream, audioCallback);
    if (const char *wav = std::getenv("PS2X_SOUND_WAV"); wav && *wav)
    {
        g_output.wavPath = wav;
        g_output.wavFile.open(g_output.wavPath,
                              std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        if (g_output.wavFile)
        {
            g_output.wavFile.write("RIFF", 4);
            writeU32(g_output.wavFile, 36u);
            g_output.wavFile.write("WAVEfmt ", 8);
            writeU32(g_output.wavFile, 16u);
            writeU16(g_output.wavFile, 1u);
            writeU16(g_output.wavFile, 2u);
            writeU32(g_output.wavFile, g_output.rate);
            writeU32(g_output.wavFile, g_output.rate * 4u);
            writeU16(g_output.wavFile, 4u);
            writeU16(g_output.wavFile, 16u);
            g_output.wavFile.write("data", 4);
            writeU32(g_output.wavFile, 0u);
            g_output.wavFile.flush();
        }
        else
            std::cerr << "[snd-output] failed opening WAV: " << g_output.wavPath << '\n';
    }
    PlayAudioStream(g_output.stream);
    g_output.ready = true;
    std::cerr << "[snd-output] stream rate=" << g_output.rate << " channels=2 bits=16\n";
    return true;
}

void shutdown()
{
    if (g_output.ready)
    {
        StopAudioStream(g_output.stream);
        UnloadAudioStream(g_output.stream);
        g_output.ready = false;
    }
    saveWav();
}
}
