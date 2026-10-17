#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Audio: 2D sound on miniaudio. .wav and .mp3 clips play one-shot or looping on a pool of kMaxVoices voices, each on
// one of a fixed set of buses (gain = sound volume * bus volume * Master volume). Owned by LuaSystems for a Play
// session (kast.audio) and by the editor for clip Preview. Every file is read through kast::readFile, so a mounted
// game.pak works. See docs/engine/audio.html.
namespace kast {

enum class AudioBus { Master, Music, SFX, UI, Voice };
constexpr int kAudioBusCount = 5;
constexpr int kMaxVoices = 32;

const char* busName(AudioBus bus); // "Master", "Music", "SFX", "UI", "Voice"
std::optional<AudioBus> busFromName(std::string_view name);

// A bus's default level, saved in project.kast ("audio").
struct BusMix {
    float volume = 1.0f; // 0..1
    bool muted = false;
    bool operator==(const BusMix&) const = default;
};
using AudioMix = std::array<BusMix, kAudioBusCount>; // indexed by AudioBus

// path ends in .wav or .mp3, ignoring case.
bool isAudioFile(std::string_view path);

// A clip's native format.
struct ClipInfo {
    uint64_t frames = 0;
    uint32_t channels = 0;
    uint32_t sampleRate = 0;
};
// Reads and decodes the header of a project-relative clip under root. nullopt + error if the path is bad, the file
// is not a .wav / .mp3, can't be read or can't be decoded.
std::optional<ClipInfo> clipInfo(const std::filesystem::path& root, const std::string& path, std::string& error);

using AudioHandle = uint64_t; // 0 never names a voice; < 2^53, so exact as a Lua number

class Audio {
public:
    // Device: the default output device, else the null backend (one warning). Null: the null backend as if no device
    // was found (same warning). Offline: no device; render() pulls the mix.
    enum class Output { Device, Null, Offline };
    struct PlayOptions {
        AudioBus bus = AudioBus::SFX;
        float volume = 1.0f; // 0..1
        bool loop = false;
    };
    struct Voice {
        AudioHandle handle;
        std::string clip;
        AudioBus bus;
        float volume;
        bool loop;
        bool paused; // by pause(handle)
    };

    // What Audio objects constructed without an output use (default Device): tests pick Offline, the editor's
    // --no-audio-device Null.
    static void setDefaultOutput(Output output);
    static Output defaultOutput();

    explicit Audio(Output output = defaultOutput());
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    // Starts a session: stops every voice, drops the clip cache (files may have changed), sets every bus to 1.0
    // unmuted and resolves clip paths against root (nullopt: play() fails with "no project"). Opens the device the
    // first time.
    void start(const std::optional<std::filesystem::path>& root);
    // Stops and frees every voice. The device stays open for the next session.
    void stop() { stopAll(); }

    // Starts path (project-relative). Music clips are decoded while they play; the others are decoded once and cached.
    // Takes the oldest non-looping voice if all are busy. Returns 0 and logs a warning if all are looping. Throws
    // std::invalid_argument for a bad path, extension or volume, or a file that can't be read or decoded.
    AudioHandle play(const std::string& path, const PlayOptions& options);
    // A stale handle (ended, stopped or stolen voice) is ignored.
    void stop(AudioHandle handle);
    void pause(AudioHandle handle);
    void resume(AudioHandle handle);
    void setVolume(AudioHandle handle, float volume); // throws std::invalid_argument outside 0..1
    bool isPlaying(AudioHandle handle) const;         // live and not paused by pause(handle)
    void stopAll();
    void stopAll(AudioBus bus);

    void setBusVolume(AudioBus bus, float volume); // throws std::invalid_argument outside 0..1
    float busVolume(AudioBus bus) const;
    void setBusMuted(AudioBus bus, bool muted);
    bool busMuted(AudioBus bus) const;
    void setMix(const AudioMix& mix); // throws std::invalid_argument if a volume is outside 0..1

    // Holds every voice where it is (editor Pause); false continues them. pause(handle) states are kept.
    void setPaused(bool paused);
    // Frees the voices whose one-shot clip ended. Call once per frame.
    void update();
    std::vector<Voice> voices() const; // live voices, oldest first
    size_t voiceCount() const;

    uint32_t sampleRate() const;
    uint32_t channels() const;
    // Offline only: mixes frames into out (interleaved, channels() floats per frame), then update()s.
    void render(float* out, uint32_t frames);

private:
    struct Impl;
    Output output_;
    std::unique_ptr<Impl> impl_; // created by the first start()
};

} // namespace kast
