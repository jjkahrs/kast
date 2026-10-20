#pragma once

#include "frustum.h"
#include "math3d.h"
#include "scene.h"

#include <bgfx/bgfx.h>

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

// GPU particles (docs/engine/particles.html): a ParticleEmitter's particles are spawned, moved and killed by a compute
// shader (cs_particle_sim.sc) and drawn as camera-facing, unlit, fogged billboards (vs/fs_particle.sc).
// The CPU side is ImGui- and GPU-free: particleSystem advances each emitter's clock (ParticleEmitterState) and decides
// how many particles to spawn; ParticleRenderer (one per Renderer) turns that into GPU work.
namespace kast {

enum class ParticleShape { Cone, Sphere, Box };
enum class ParticleSpace { World, Local }; // World: spawned particles stay put; Local: they move with the entity
enum class ParticleBlend { Alpha, Additive }; // Additive: src * alpha + dst

constexpr int kMaxParticles = 65536;          // per emitter
constexpr float kMaxParticleColor = 16.0f;    // HDR: channels above 1 glow with bloom
constexpr float kMaxParticleDt = 0.1f;        // seconds: a longer frame (a hitch) simulates only this much

// Ranges are min/max pairs; either order works (each particle picks uniformly between them).
// Cone emits along the entity's local +Y (Kast is Y-up) within coneAngle degrees of it, from a disc of radius.
// Sphere emits outward from its center; Box emits along local +Y from inside boxSize (full extents).
// emitFromSurface: Cone from the disc's rim, Sphere / Box from their shell, instead of the whole volume.
// endSize multiplies each particle's start size at the end of its life (1 = constant size).
struct ParticleEmitter {
    bool playOnStart = true;
    bool looping = true;
    float duration = 5.0f;  // s, one cycle, > 0
    float rate = 10.0f;     // particles per second, >= 0
    int burstCount = 0;     // emitted at the start of each cycle, 0..kMaxParticles
    int maxParticles = 1000; // 1..kMaxParticles: live particles; the oldest are replaced when full
    ParticleShape shape = ParticleShape::Cone;
    float coneAngle = 25.0f; // degrees, 0..180; 0 = a straight beam
    float radius = 0.25f;    // sphere radius and cone base radius, >= 0
    bx::Vec3 boxSize{1.0f, 1.0f, 1.0f}; // >= 0
    bool emitFromSurface = false;
    float lifetimeMin = 2.0f, lifetimeMax = 3.0f;   // s, > 0
    float speedMin = 0.5f, speedMax = 1.0f;         // m/s
    float startSizeMin = 0.3f, startSizeMax = 0.5f; // m, >= 0
    float endSize = 2.0f;                           // multiplier of the start size, >= 0
    bx::Vec3 startColor{1.0f, 1.0f, 1.0f}; // linear RGB, 0..kMaxParticleColor
    bx::Vec3 endColor{1.0f, 1.0f, 1.0f};
    float startAlpha = 0.8f, endAlpha = 0.0f; // 0..1
    bx::Vec3 gravity{0.0f, 0.0f, 0.0f}; // m/s^2, world space
    ParticleSpace space = ParticleSpace::World;
    std::string texture; // project-relative image; empty = a procedural soft round falloff
    ParticleBlend blend = ParticleBlend::Alpha;
};

// Runtime only (never saved or copied): an emitter's clock, made by particleSystem. The GPU side catches up to it:
// it steps simTime - its own and spawns spawned - its own, so any number of renderers can follow one emitter.
struct ParticleEmitterState {
    uint64_t id = 0;     // unique per state: renderers key their GPU buffers by it
    uint32_t seed = 0;   // per entity, so emitters don't look identical
    bool playing = false; // emitting; stopped emitters' live particles still finish their life
    bool paused = false;  // nothing moves
    bool started = false; // the first cycle's burst went out
    float time = 0.0f;    // s into the current cycle
    int cycle = 0;
    float accumulator = 0.0f; // fractional particles carried to the next frame
    uint64_t spawned = 0;     // particles requested since the state was made
    double simTime = 0.0;     // seconds simulated since the state was made
    uint32_t clears = 0;      // bumped to kill every live particle
    uint64_t lastSpawn = 0;   // the last tick's share of spawned (with the bursts since the tick before)
    float lastDt = 0.0f;      // and of simTime: a renderer seeing the state first catches up from there
    uint64_t pending = 0;     // burstEmitter's particles since the last tick
    int capacity = 0;         // maxParticles at the last tick: a change empties the GPU ring
    std::deque<std::pair<double, uint32_t>> births; // (simTime, count) of the spawns still possibly alive
};

// dt clamped to 0..kMaxParticleDt.
float clampParticleDt(float dt);
// Advances the emitter's clock by dt (clamped) and returns how many particles to spawn: the cycle's burst at its start
// plus rate * dt with the fraction carried over. A non-looping emitter stops playing at the end of its cycle.
int advanceEmitter(const ParticleEmitter& emitter, ParticleEmitterState& state, float dt);
// entity's state (it must have a ParticleEmitter), made on first sight: playing if playOnStart.
ParticleEmitterState& emitterState(entt::registry& registry, entt::entity entity);
// Runtime controls (kast.particles, the editor's Inspector); none is saved or undoable.
// Starts or resumes; a stopped emitter starts its cycle over (with its burst).
void playEmitter(ParticleEmitterState& state);
// Stops emitting; clear also kills the live particles, else they finish their life.
void stopEmitter(ParticleEmitterState& state, bool clear);
// Kills the live particles and plays from the start of the cycle.
void restartEmitter(ParticleEmitterState& state);
// Spawns count particles (clamped to 0..maxParticles) at the next simulate, even when stopped; returns how many.
int burstEmitter(const ParticleEmitter& emitter, ParticleEmitterState& state, int count);
// Live particles, estimated on the CPU (no GPU readback): those spawned within the last lifetimeMax seconds of
// simulated time, at most maxParticles. An upper bound: 0 means every particle is dead.
// ponytail: births are kept per tick (about lifetimeMax * fps entries); shortening lifetimeMax at runtime under-counts
// the particles already flying. A delayed GPU readback of an alive counter if exact counts matter.
int aliveParticles(const ParticleEmitter& emitter, const ParticleEmitterState& state);
// Ticks every ParticleEmitter (making its state on first sight: playing if playOnStart). Play and the editor's edit
// mode preview call it; GPU-free.
void particleSystem(Scene& scene, float dt);

// Farthest a particle gets from the emitter's position: the shape's reach (scaled), the flight at the top speed
// under gravity for the longest lifetime, and half the largest size. A box around worldPos +- that is the culling
// bound. World space particles left behind by a moving emitter can fall outside it (culled a little early).
float particleReach(const ParticleEmitter& emitter, float maxScale);
Aabb particleBounds(const ParticleEmitter& emitter, const float* world);
// The emission shape as line segments in the entity's local space (the editor's gizmo): a sphere's three circles, the
// box's twelve edges, or the cone's base disc and its spread over the first metre (a cylinder at coneAngle 0).
std::vector<std::pair<bx::Vec3, bx::Vec3>> particleShapeLines(const ParticleEmitter& emitter);

// The GPU side of the emitters drawn by one Renderer. Buffers are made on first sight of a state and destroyed when
// it is gone (entity, component, scene or project), its maxParticles changes, or on shutdown.
// ponytail: a ring buffer of maxParticles slots per emitter; new particles overwrite the oldest slots and every slot
// is drawn (dead ones collapse to nothing in the vertex shader). Ceiling: a full ring recycles particles before they
// die, and draw cost follows maxParticles, not the live count; a dead list + indirect draw if that matters.
class ParticleRenderer {
public:
    struct Draw {
        uint64_t id = 0; // the state's
        const ParticleEmitter* emitter = nullptr;
        std::array<float, 16> world{};
        bx::Vec3 center{0.0f, 0.0f, 0.0f}; // for blend sorting
    };

    // supportedCaps: bgfx::getCaps()->supported; without BGFX_CAPS_COMPUTE no emitter is ever simulated or drawn.
    void init(uint64_t supportedCaps);
    void shutdown();
    // Runs this frame's compute work for the scene's ticked emitters in view (bgfx runs a view's compute before its
    // draws) and returns the ones to draw: all of them, or with frustum those whose bounds intersect it.
    std::vector<Draw> simulate(const Scene& scene, bgfx::ViewId view, const Frustum* frustum);
    // Draws one emitter's particles (blended, depth tested, no depth write): times texture if textured, else a soft
    // circle (texture is still bound). fog / cameraPos: the scene's, as fs_mesh.sc takes them.
    void draw(const Draw& item, bgfx::ViewId view, bgfx::TextureHandle texture, bool textured, const float* fogColor,
              const float* fogParams, const float* cameraPos);
    size_t emitters() const { return gpu_.size(); } // with GPU buffers

private:
    struct Gpu {
        bgfx::DynamicVertexBufferHandle particles = BGFX_INVALID_HANDLE;
        int capacity = 0;
        uint32_t head = 0;      // next ring slot
        uint64_t spawned = 0;   // the state's spawned this has caught up to
        double simTime = 0.0;   // and its simTime
        uint32_t clears = 0;
        bool used = false;      // by this frame's simulate
    };
    void destroy(Gpu& gpu);

    bool supported_ = false;
    bgfx::VertexLayout particleLayout_; // 3 vec4 per particle (cs_particle_sim.sc)
    bgfx::VertexLayout quadLayout_;
    bgfx::VertexBufferHandle quad_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle simProgram_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle drawProgram_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle sim_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle simWorld_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle startColor_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle endColor_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle params_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle cameraPos_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle fogColor_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle fogParams_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle sampler_ = BGFX_INVALID_HANDLE;
    std::map<uint64_t, Gpu> gpu_; // by state id
};

} // namespace kast
