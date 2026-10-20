#include "particles.h"

#include "cs_particle_sim.bin.h"
#include "fs_particle.bin.h"
#include "vs_particle.bin.h"

#include "material.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>

namespace kast {
namespace {

constexpr float kMinParticleDuration = 0.01f; // s: also the validated minimum
constexpr uint16_t kSimThreads = 64;          // NUM_THREADS in cs_particle_sim.sc
constexpr uint32_t kFloatExact = 0xffffff;    // integers sent as floats keep 24 bits
constexpr uint64_t kDrawState = BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA | BGFX_STATE_PT_TRISTRIP;

std::atomic<uint64_t> g_nextStateId = 1;

uint32_t seedOf(uint64_t id)
{
    const uint64_t mixed = id * 0x9e3779b97f4a7c15ull;
    return static_cast<uint32_t>(mixed >> 32);
}

} // namespace

float clampParticleDt(float dt) { return std::isfinite(dt) ? std::clamp(dt, 0.0f, kMaxParticleDt) : 0.0f; }

int advanceEmitter(const ParticleEmitter& emitter, ParticleEmitterState& state, float dt)
{
    dt = state.paused ? 0.0f : clampParticleDt(dt);
    state.lastDt = dt;
    state.lastSpawn = 0;
    state.simTime += dt;
    if (!state.playing || state.paused)
        return 0;
    const float duration = std::max(emitter.duration, kMinParticleDuration);
    const int burst = std::clamp(emitter.burstCount, 0, kMaxParticles);
    int64_t count = 0;
    if (!std::exchange(state.started, true))
        count += burst;
    for (float left = dt;;) {
        if (state.time >= duration) { // a cycle ended
            if (!emitter.looping) {
                state.playing = false;
                break;
            }
            state.time = 0.0f;
            ++state.cycle;
            count += burst;
        }
        if (left <= 0.0f)
            break;
        const float step = std::min(left, duration - state.time);
        state.accumulator += std::max(emitter.rate, 0.0f) * step;
        state.time = step == duration - state.time ? duration : state.time + step; // exact, so the cycle ends
        left -= step;
    }
    const float whole = std::floor(state.accumulator);
    state.accumulator -= whole;
    count = std::min<int64_t>(count + static_cast<int64_t>(whole), kMaxParticles);
    state.spawned += static_cast<uint64_t>(count);
    state.lastSpawn = static_cast<uint64_t>(count);
    return static_cast<int>(count);
}

void particleSystem(Scene& scene, float dt)
{
    entt::registry& registry = scene.registry();
    // A state outlives nothing: re-adding the component starts a fresh emitter.
    const auto orphans = registry.view<ParticleEmitterState>(entt::exclude<ParticleEmitter>);
    registry.remove<ParticleEmitterState>(orphans.begin(), orphans.end());
    for (auto [entity, emitter] : registry.view<ParticleEmitter>().each()) {
        auto* state = registry.try_get<ParticleEmitterState>(entity);
        if (!state) {
            state = &registry.emplace<ParticleEmitterState>(entity);
            state->id = g_nextStateId++;
            const auto* persistent = registry.try_get<PersistentId>(entity);
            state->seed = seedOf(persistent ? persistent->value : static_cast<uint64_t>(entt::to_integral(entity)));
            state->playing = emitter.playOnStart;
        }
        advanceEmitter(emitter, *state, dt);
    }
}

float particleReach(const ParticleEmitter& emitter, float maxScale)
{
    const float shape = emitter.shape == ParticleShape::Box ? 0.5f * bx::length(emitter.boxSize) : emitter.radius;
    const float life = std::max(emitter.lifetimeMin, emitter.lifetimeMax);
    const float speed = std::max(std::abs(emitter.speedMin), std::abs(emitter.speedMax));
    const float flight = speed * life * (emitter.space == ParticleSpace::Local ? maxScale : 1.0f);
    const float fall = 0.5f * bx::length(emitter.gravity) * life * life;
    const float size = 0.5f * std::max(emitter.startSizeMin, emitter.startSizeMax) * std::max(1.0f, emitter.endSize);
    return shape * maxScale + flight + fall + size;
}

Aabb particleBounds(const ParticleEmitter& emitter, const float* world)
{
    float maxScale = 0.0f;
    for (int row = 0; row < 3; ++row)
        maxScale = std::max(maxScale, bx::length(bx::Vec3{world[row * 4], world[row * 4 + 1], world[row * 4 + 2]}));
    const float reach = particleReach(emitter, maxScale);
    const bx::Vec3 center{world[12], world[13], world[14]};
    return {bx::sub(center, reach), bx::add(center, reach)};
}

void ParticleRenderer::init(uint64_t supportedCaps)
{
    supported_ = (supportedCaps & BGFX_CAPS_COMPUTE) != 0;
    if (!supported_)
        return;
    particleLayout_.begin()
        .add(bgfx::Attrib::TexCoord0, 4, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord1, 4, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord2, 4, bgfx::AttribType::Float)
        .end();
    quadLayout_.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).end();
    static const float corners[4][3] = {{-1, -1, 0}, {1, -1, 0}, {-1, 1, 0}, {1, 1, 0}}; // a triangle strip
    quad_ = bgfx::createVertexBuffer(bgfx::makeRef(corners, sizeof(corners)), quadLayout_);
    simProgram_ = bgfx::createProgram(bgfx::createShader(bgfx::makeRef(cs_particle_sim, sizeof(cs_particle_sim))), true);
    drawProgram_ = bgfx::createProgram(bgfx::createShader(bgfx::makeRef(vs_particle, sizeof(vs_particle))),
                                       bgfx::createShader(bgfx::makeRef(fs_particle, sizeof(fs_particle))), true);
    sim_ = bgfx::createUniform("u_sim", bgfx::UniformType::Vec4, 7);
    simWorld_ = bgfx::createUniform("u_simWorld", bgfx::UniformType::Mat4);
    startColor_ = bgfx::createUniform("u_particleStart", bgfx::UniformType::Vec4);
    endColor_ = bgfx::createUniform("u_particleEnd", bgfx::UniformType::Vec4);
    params_ = bgfx::createUniform("u_particleParams", bgfx::UniformType::Vec4);
    // The same uniforms as the Renderer's (bgfx shares them by name).
    cameraPos_ = bgfx::createUniform("u_cameraPos", bgfx::UniformType::Vec4);
    fogColor_ = bgfx::createUniform("u_fogColor", bgfx::UniformType::Vec4);
    fogParams_ = bgfx::createUniform("u_fogParams", bgfx::UniformType::Vec4);
    sampler_ = bgfx::createUniform("s_particle", bgfx::UniformType::Sampler);
}

void ParticleRenderer::shutdown()
{
    for (auto& [id, gpu] : gpu_)
        destroy(gpu);
    gpu_.clear();
    if (!supported_)
        return;
    bgfx::destroy(quad_);
    bgfx::destroy(simProgram_);
    bgfx::destroy(drawProgram_);
    for (bgfx::UniformHandle uniform :
         {sim_, simWorld_, startColor_, endColor_, params_, cameraPos_, fogColor_, fogParams_, sampler_})
        bgfx::destroy(uniform);
    supported_ = false;
}

void ParticleRenderer::destroy(Gpu& gpu)
{
    if (bgfx::isValid(gpu.particles))
        bgfx::destroy(gpu.particles);
    gpu.particles = BGFX_INVALID_HANDLE;
    gpu.capacity = 0;
}

std::vector<ParticleRenderer::Draw> ParticleRenderer::simulate(const Scene& scene, bgfx::ViewId view,
                                                               const Frustum* frustum)
{
    std::vector<Draw> draws;
    const auto emitters = scene.registry().view<const ParticleEmitter, const ParticleEmitterState>();
    if (!supported_) {
        if (emitters.begin() != emitters.end())
            warnOnce("Particles: the GPU has no compute shader support; ParticleEmitters are not drawn");
        return draws;
    }
    for (auto [entity, emitter, state] : emitters.each()) {
        const auto [it, added] = gpu_.try_emplace(state.id);
        Gpu& gpu = it->second;
        gpu.used = true;
        // New, or this renderer missed many ticks (e.g. a hidden editor tab): catch up from the last tick only, not
        // from when the state was made, dropping what this renderer still had (rather than a burst of maxParticles).
        const bool stale = !added && state.simTime - gpu.simTime > state.lastDt + kMaxParticleDt;
        if (added || stale) {
            gpu.spawned = state.spawned - std::min(state.spawned, state.lastSpawn);
            gpu.simTime = state.simTime - state.lastDt;
            gpu.clears = state.clears - (stale ? 1 : 0); // stale: cleared below
        }
        const int capacity = std::clamp(emitter.maxParticles, 1, kMaxParticles);
        bool clear = state.clears != gpu.clears;
        if (gpu.capacity != capacity) { // new, or maxParticles changed: a ring the dispatch below clears (all dead)
            destroy(gpu); // bgfx can't upload zeros into a compute-written buffer
            gpu.particles = bgfx::createDynamicVertexBuffer(static_cast<uint32_t>(capacity), particleLayout_,
                                                            BGFX_BUFFER_COMPUTE_READ_WRITE);
            gpu.capacity = capacity;
            gpu.head = 0;
            clear = true;
        }
        Draw draw{state.id, &emitter};
        scene.worldMatrix(entity, draw.world.data());
        draw.center = {draw.world[12], draw.world[13], draw.world[14]};

        const float dt = clampParticleDt(static_cast<float>(state.simTime - gpu.simTime));
        const auto spawn = static_cast<uint32_t>(std::min<uint64_t>(state.spawned - gpu.spawned, capacity));
        const uint64_t serial = gpu.spawned;
        gpu.simTime = state.simTime;
        gpu.spawned = state.spawned;
        gpu.clears = state.clears;
        if (dt > 0.0f || spawn > 0 || clear) {
            const bool local = emitter.space == ParticleSpace::Local;
            float identity[16];
            bx::mtxIdentity(identity);
            bx::Vec3 gravity = emitter.gravity; // into the space particles live in
            if (local) {
                float inverse[16];
                bx::mtxInverse(inverse, draw.world.data());
                gravity = transformDir(gravity, inverse);
            }
            const float sim[7][4] = {
                {float(gpu.head), float(spawn), float(capacity), dt},
                {float(serial & kFloatExact), float(state.seed & kFloatExact), clear ? 1.0f : 0.0f, 0.0f},
                {float(emitter.shape), std::cos(bx::toRad(std::clamp(emitter.coneAngle, 0.0f, 180.0f))), emitter.radius,
                 emitter.emitFromSurface ? 1.0f : 0.0f},
                {emitter.boxSize.x, emitter.boxSize.y, emitter.boxSize.z, 0.0f},
                {emitter.lifetimeMin, emitter.lifetimeMax, emitter.speedMin, emitter.speedMax},
                {emitter.startSizeMin, emitter.startSizeMax, 0.0f, 0.0f},
                {gravity.x, gravity.y, gravity.z, 0.0f},
            };
            bgfx::setUniform(sim_, sim, 7);
            bgfx::setUniform(simWorld_, local ? identity : draw.world.data());
            bgfx::setBuffer(0, gpu.particles, bgfx::Access::ReadWrite);
            bgfx::dispatch(view, simProgram_, static_cast<uint32_t>((capacity + kSimThreads - 1) / kSimThreads));
            gpu.head = (gpu.head + spawn) % static_cast<uint32_t>(capacity);
        }
        if (!frustum || intersects(*frustum, particleBounds(emitter, draw.world.data())))
            draws.push_back(draw); // off screen: simulated above, not drawn
    }
    for (auto it = gpu_.begin(); it != gpu_.end();) { // the entity, component or scene is gone
        if (std::exchange(it->second.used, false)) {
            ++it;
            continue;
        }
        destroy(it->second);
        it = gpu_.erase(it);
    }
    return draws;
}

void ParticleRenderer::draw(const Draw& item, bgfx::ViewId view, bgfx::TextureHandle texture, bool textured,
                            const float* fogColor, const float* fogParams, const float* cameraPos)
{
    const auto it = gpu_.find(item.id);
    if (it == gpu_.end())
        return;
    const ParticleEmitter& emitter = *item.emitter;
    const bool additive = emitter.blend == ParticleBlend::Additive;
    const float start[4] = {emitter.startColor.x, emitter.startColor.y, emitter.startColor.z, emitter.startAlpha};
    const float end[4] = {emitter.endColor.x, emitter.endColor.y, emitter.endColor.z, emitter.endAlpha};
    const float params[4] = {emitter.endSize, textured ? 1.0f : 0.0f, additive ? 1.0f : 0.0f, 0.0f};
    bgfx::setUniform(startColor_, start);
    bgfx::setUniform(endColor_, end);
    bgfx::setUniform(params_, params);
    bgfx::setUniform(cameraPos_, cameraPos);
    bgfx::setUniform(fogColor_, fogColor);
    bgfx::setUniform(fogParams_, fogParams);
    float identity[16];
    bx::mtxIdentity(identity);
    bgfx::setTransform(emitter.space == ParticleSpace::Local ? item.world.data() : identity);
    bgfx::setTexture(0, sampler_, texture, BGFX_SAMPLER_UVW_CLAMP);
    bgfx::setVertexBuffer(0, quad_);
    bgfx::setInstanceDataBuffer(it->second.particles, 0, static_cast<uint32_t>(it->second.capacity));
    bgfx::setState(kDrawState | (additive ? BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_ONE)
                                          : BGFX_STATE_BLEND_ALPHA));
    bgfx::submit(view, drawProgram_);
}

} // namespace kast
