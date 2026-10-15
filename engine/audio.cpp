#include "audio.h"

#include "files.h"
#include "log.h"
#include "project.h"

#define MA_NO_FLAC     // .wav and .mp3 only
#define MA_NO_ENCODING // playback only
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <stdexcept>
#include <unordered_map>

namespace kast {
namespace {

constexpr const char* kBusNames[kAudioBusCount] = {"Master", "Music", "SFX", "UI", "Voice"};
constexpr uint32_t kOfflineSampleRate = 48000;
constexpr uint32_t kOfflineChannels = 2;
constexpr int kSlotBits = 8; // handle = generation << kSlotBits | slot

bool endsWith(std::string_view path, std::string_view extension)
{
    return path.size() >= extension.size() &&
           std::ranges::equal(path.substr(path.size() - extension.size()), extension,
                              [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
}

void checkVolume(float volume)
{
    if (!(volume >= 0.0f && volume <= 1.0f))
        throw std::invalid_argument(std::format("volume must be between 0 and 1, got {}", volume));
}

// A decoder config for path's format (by extension), converting to format / channels / sampleRate (0 = native).
ma_decoder_config decoderConfig(const std::string& path, ma_format format, uint32_t channels, uint32_t sampleRate)
{
    ma_decoder_config config = ma_decoder_config_init(format, channels, sampleRate);
    config.encodingFormat = endsWith(path, ".mp3") ? ma_encoding_format_mp3 : ma_encoding_format_wav;
    return config;
}

// The bytes of a project-relative .wav / .mp3 under root. Throws std::invalid_argument with the reason.
std::string readClip(const std::optional<std::filesystem::path>& root, const std::string& path)
{
    if (!isAudioFile(path))
        throw std::invalid_argument(std::format("'{}' is not a .wav or .mp3 file", path));
    if (!root)
        throw std::invalid_argument(std::format("cannot load '{}': no project", path));
    std::string error;
    const std::optional<std::filesystem::path> file = resolveProjectPath(*root, path, error);
    if (!file)
        throw std::invalid_argument(std::format("'{}': {}", path, error));
    std::string bytes;
    if (!readFile(*file, bytes))
        throw std::invalid_argument(std::format("cannot read '{}'", path));
    return bytes;
}

[[noreturn]] void cannotDecode(const std::string& path)
{
    throw std::invalid_argument(std::format("cannot decode '{}' (corrupt or not a {} file)", path,
                                            endsWith(path, ".mp3") ? "MP3" : "WAV"));
}

// A fully decoded clip in the engine's format (f32, engine channels and rate).
struct Clip {
    std::vector<float> samples;
    uint64_t frames = 0;
};

// A Music clip decoded while it plays: the decoder reads from bytes, so neither may move.
struct Stream {
    std::string bytes;
    ma_decoder decoder;
};

struct Slot {
    bool live = false;
    uint32_t generation = 0;
    uint64_t order = 0; // play order; the lowest non-looping voice is stolen first
    ma_sound sound;
    ma_audio_buffer_ref buffer;           // cached clips: reads clip's samples
    std::shared_ptr<const Clip> clip;     // keeps the samples alive while the voice plays
    std::unique_ptr<Stream> stream;       // Music clips
    std::string path;
    AudioBus bus = AudioBus::SFX;
    float volume = 1.0f;
    bool loop = false;
    bool paused = false; // by Audio::pause(handle)
};

} // namespace

const char* busName(AudioBus bus) { return kBusNames[static_cast<int>(bus)]; }

std::optional<AudioBus> busFromName(std::string_view name)
{
    for (int i = 0; i < kAudioBusCount; ++i)
        if (name == kBusNames[i])
            return static_cast<AudioBus>(i);
    return std::nullopt;
}

bool isAudioFile(std::string_view path) { return endsWith(path, ".wav") || endsWith(path, ".mp3"); }

std::optional<ClipInfo> clipInfo(const std::filesystem::path& root, const std::string& path, std::string& error)
{
    try {
        const std::string bytes = readClip(root, path);
        const ma_decoder_config config = decoderConfig(path, ma_format_unknown, 0, 0); // native format
        ma_decoder decoder;
        if (ma_decoder_init_memory(bytes.data(), bytes.size(), &config, &decoder) != MA_SUCCESS)
            cannotDecode(path);
        ClipInfo info{.channels = decoder.outputChannels, .sampleRate = decoder.outputSampleRate};
        const ma_result result = ma_decoder_get_length_in_pcm_frames(&decoder, &info.frames);
        ma_decoder_uninit(&decoder);
        if (result != MA_SUCCESS || info.frames == 0)
            cannotDecode(path);
        return info;
    } catch (const std::invalid_argument& e) {
        error = e.what();
        return std::nullopt;
    }
}

struct Audio::Impl {
    explicit Impl(Output output);
    ~Impl();

    Slot* find(AudioHandle handle);
    const Slot* find(AudioHandle handle) const;
    std::shared_ptr<const Clip> clip(const std::string& path);
    void release(Slot& slot);
    void applyBus(AudioBus bus);

    bool hasContext = false;
    ma_context context; // the null backend's, when there is no device
    ma_engine engine;
    std::array<ma_sound_group, kAudioBusCount> groups; // Master is the others' parent
    AudioMix mix{};
    std::array<Slot, kMaxVoices> slots;
    std::unordered_map<std::string, std::shared_ptr<const Clip>> cache; // by project-relative path
    std::optional<std::filesystem::path> root;
    uint64_t nextOrder = 0;
    bool paused = false;
};

Audio::Impl::Impl(Output output)
{
    ma_engine_config config = ma_engine_config_init();
    if (output == Output::Offline) {
        config.noDevice = MA_TRUE;
        config.channels = kOfflineChannels;
        config.sampleRate = kOfflineSampleRate;
    }
    if (output != Output::Device || ma_engine_init(&config, &engine) != MA_SUCCESS) {
        if (output != Output::Offline) {
            log::warn("No audio output device: sound plays on the null backend (silent)");
            const ma_backend backend = ma_backend_null;
            if (ma_context_init(&backend, 1, nullptr, &context) != MA_SUCCESS)
                throw std::runtime_error("Cannot start the null audio backend");
            hasContext = true;
            config.pContext = &context;
        }
        if (ma_engine_init(&config, &engine) != MA_SUCCESS) {
            if (hasContext)
                ma_context_uninit(&context);
            throw std::runtime_error("Cannot start the audio engine");
        }
    }
    for (int i = 0; i < kAudioBusCount; ++i)
        ma_sound_group_init(&engine, 0, i == 0 ? nullptr : &groups[0], &groups[i]);
}

Audio::Impl::~Impl()
{
    for (Slot& slot : slots)
        if (slot.live)
            release(slot);
    for (int i = kAudioBusCount - 1; i >= 0; --i) // children before Master
        ma_sound_group_uninit(&groups[i]);
    ma_engine_uninit(&engine);
    if (hasContext)
        ma_context_uninit(&context);
}

Slot* Audio::Impl::find(AudioHandle handle)
{
    const size_t index = handle & ((1u << kSlotBits) - 1);
    if (index >= slots.size() || !slots[index].live || slots[index].generation != handle >> kSlotBits)
        return nullptr;
    return &slots[index];
}

const Slot* Audio::Impl::find(AudioHandle handle) const { return const_cast<Impl*>(this)->find(handle); }

std::shared_ptr<const Clip> Audio::Impl::clip(const std::string& path)
{
    if (const auto it = cache.find(path); it != cache.end())
        return it->second;
    const std::string bytes = readClip(root, path);
    ma_decoder_config config = decoderConfig(path, ma_format_f32, ma_engine_get_channels(&engine),
                                             ma_engine_get_sample_rate(&engine));
    ma_uint64 frames = 0;
    void* data = nullptr;
    if (ma_decode_memory(bytes.data(), bytes.size(), &config, &frames, &data) != MA_SUCCESS || frames == 0) {
        ma_free(data, nullptr);
        cannotDecode(path);
    }
    auto decoded = std::make_shared<Clip>();
    decoded->frames = frames;
    const float* samples = static_cast<const float*>(data);
    decoded->samples.assign(samples, samples + frames * config.channels);
    ma_free(data, nullptr);
    cache[path] = decoded;
    return decoded;
}

void Audio::Impl::release(Slot& slot)
{
    ma_sound_uninit(&slot.sound);
    if (slot.stream)
        ma_decoder_uninit(&slot.stream->decoder);
    slot.stream.reset();
    slot.clip.reset();
    slot.live = false;
}

void Audio::Impl::applyBus(AudioBus bus)
{
    const BusMix& level = mix[static_cast<int>(bus)];
    ma_sound_group_set_volume(&groups[static_cast<int>(bus)], level.muted ? 0.0f : level.volume);
}

namespace {
Audio::Output g_defaultOutput = Audio::Output::Device;
}

void Audio::setDefaultOutput(Output output) { g_defaultOutput = output; }
Audio::Output Audio::defaultOutput() { return g_defaultOutput; }

Audio::Audio(Output output) : output_(output) {}
Audio::~Audio() = default;

void Audio::start(const std::optional<std::filesystem::path>& root)
{
    if (!impl_)
        impl_ = std::make_unique<Impl>(output_);
    stopAll();
    impl_->cache.clear();
    impl_->root = root;
    impl_->paused = false;
    setMix({});
}

AudioHandle Audio::play(const std::string& path, const PlayOptions& options)
{
    checkVolume(options.volume);
    if (!impl_)
        throw std::invalid_argument("audio is not running");
    Impl& a = *impl_;
    // Load before taking a voice, so a bad clip never steals one.
    std::shared_ptr<const Clip> clip;
    std::unique_ptr<Stream> stream;
    if (options.bus == AudioBus::Music) {
        stream = std::make_unique<Stream>();
        stream->bytes = readClip(a.root, path);
        const ma_decoder_config config = decoderConfig(path, ma_format_f32, ma_engine_get_channels(&a.engine),
                                                       ma_engine_get_sample_rate(&a.engine));
        if (ma_decoder_init_memory(stream->bytes.data(), stream->bytes.size(), &config, &stream->decoder) != MA_SUCCESS)
            cannotDecode(path);
    } else {
        clip = a.clip(path);
    }

    update();
    auto free = std::ranges::find_if(a.slots, [](const Slot& s) { return !s.live; });
    if (free == a.slots.end()) {
        Slot* oldest = nullptr;
        for (Slot& slot : a.slots)
            if (!slot.loop && (!oldest || slot.order < oldest->order))
                oldest = &slot;
        if (!oldest) {
            if (stream)
                ma_decoder_uninit(&stream->decoder);
            log::warn(std::format("Sound '{}' dropped: all {} voices are looping", path, kMaxVoices));
            return 0;
        }
        a.release(*oldest);
        free = a.slots.begin() + (oldest - a.slots.data());
    }

    Slot& slot = *free;
    ma_data_source* source = nullptr;
    if (stream) {
        source = &stream->decoder;
    } else {
        ma_audio_buffer_ref_init(ma_format_f32, ma_engine_get_channels(&a.engine), clip->samples.data(), clip->frames,
                                 &slot.buffer);
        slot.buffer.sampleRate = ma_engine_get_sample_rate(&a.engine); // init leaves it 0 (miniaudio 0.11)
        source = &slot.buffer;
    }
    if (ma_sound_init_from_data_source(&a.engine, source, MA_SOUND_FLAG_NO_SPATIALIZATION,
                                       &a.groups[static_cast<int>(options.bus)], &slot.sound) != MA_SUCCESS) {
        if (stream)
            ma_decoder_uninit(&stream->decoder);
        throw std::invalid_argument(std::format("cannot play '{}'", path));
    }
    slot.live = true;
    ++slot.generation;
    slot.order = a.nextOrder++;
    slot.clip = std::move(clip);
    slot.stream = std::move(stream);
    slot.path = path;
    slot.bus = options.bus;
    slot.volume = options.volume;
    slot.loop = options.loop;
    slot.paused = false;
    ma_sound_set_volume(&slot.sound, options.volume);
    ma_sound_set_looping(&slot.sound, options.loop ? MA_TRUE : MA_FALSE);
    if (!a.paused)
        ma_sound_start(&slot.sound);
    return (static_cast<AudioHandle>(slot.generation) << kSlotBits) | static_cast<AudioHandle>(free - a.slots.begin());
}

void Audio::stop(AudioHandle handle)
{
    if (Slot* slot = impl_ ? impl_->find(handle) : nullptr)
        impl_->release(*slot);
}

void Audio::pause(AudioHandle handle)
{
    if (Slot* slot = impl_ ? impl_->find(handle) : nullptr) {
        slot->paused = true;
        ma_sound_stop(&slot->sound); // keeps the cursor
    }
}

void Audio::resume(AudioHandle handle)
{
    if (Slot* slot = impl_ ? impl_->find(handle) : nullptr) {
        slot->paused = false;
        if (!impl_->paused)
            ma_sound_start(&slot->sound);
    }
}

void Audio::setVolume(AudioHandle handle, float volume)
{
    checkVolume(volume);
    if (Slot* slot = impl_ ? impl_->find(handle) : nullptr) {
        slot->volume = volume;
        ma_sound_set_volume(&slot->sound, volume);
    }
}

bool Audio::isPlaying(AudioHandle handle) const
{
    const Slot* slot = impl_ ? impl_->find(handle) : nullptr;
    return slot && !slot->paused;
}

void Audio::stopAll()
{
    if (impl_)
        for (Slot& slot : impl_->slots)
            if (slot.live)
                impl_->release(slot);
}

void Audio::stopAll(AudioBus bus)
{
    if (impl_)
        for (Slot& slot : impl_->slots)
            if (slot.live && slot.bus == bus)
                impl_->release(slot);
}

void Audio::setBusVolume(AudioBus bus, float volume)
{
    checkVolume(volume);
    if (!impl_)
        return;
    impl_->mix[static_cast<int>(bus)].volume = volume;
    impl_->applyBus(bus);
}

float Audio::busVolume(AudioBus bus) const { return impl_ ? impl_->mix[static_cast<int>(bus)].volume : 1.0f; }

void Audio::setBusMuted(AudioBus bus, bool muted)
{
    if (!impl_)
        return;
    impl_->mix[static_cast<int>(bus)].muted = muted;
    impl_->applyBus(bus);
}

bool Audio::busMuted(AudioBus bus) const { return impl_ && impl_->mix[static_cast<int>(bus)].muted; }

void Audio::setMix(const AudioMix& mix)
{
    for (const BusMix& level : mix)
        checkVolume(level.volume);
    if (!impl_)
        return;
    impl_->mix = mix;
    for (int i = 0; i < kAudioBusCount; ++i)
        impl_->applyBus(static_cast<AudioBus>(i));
}

void Audio::setPaused(bool paused)
{
    if (!impl_ || impl_->paused == paused)
        return;
    impl_->paused = paused;
    for (Slot& slot : impl_->slots)
        if (slot.live && !slot.paused) {
            if (paused)
                ma_sound_stop(&slot.sound);
            else
                ma_sound_start(&slot.sound);
        }
}

void Audio::update()
{
    if (impl_)
        for (Slot& slot : impl_->slots)
            if (slot.live && !slot.loop && ma_sound_at_end(&slot.sound))
                impl_->release(slot);
}

std::vector<Audio::Voice> Audio::voices() const
{
    std::vector<const Slot*> live;
    if (impl_)
        for (const Slot& slot : impl_->slots)
            if (slot.live)
                live.push_back(&slot);
    std::ranges::sort(live, {}, [](const Slot* slot) { return slot->order; });
    std::vector<Voice> out;
    for (const Slot* slot : live)
        out.push_back({(static_cast<AudioHandle>(slot->generation) << kSlotBits) |
                           static_cast<AudioHandle>(slot - impl_->slots.data()),
                       slot->path, slot->bus, slot->volume, slot->loop, slot->paused});
    return out;
}

size_t Audio::voiceCount() const
{
    return impl_ ? static_cast<size_t>(std::ranges::count_if(impl_->slots, &Slot::live)) : 0;
}

uint32_t Audio::sampleRate() const { return impl_ ? ma_engine_get_sample_rate(&impl_->engine) : 0; }
uint32_t Audio::channels() const { return impl_ ? ma_engine_get_channels(&impl_->engine) : 0; }

void Audio::render(float* out, uint32_t frames)
{
    if (!impl_ || output_ != Output::Offline)
        throw std::logic_error("Audio::render needs a started Offline audio");
    ma_engine_read_pcm_frames(&impl_->engine, out, frames, nullptr);
    update();
}

} // namespace kast
