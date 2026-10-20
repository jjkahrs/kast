#include <bgfx_compute.sh>

// One thread per ring slot of a ParticleEmitter (see engine/particles.h). Each particle is 3 vec4: (position, age),
// (velocity, lifetime), (size, 0, 0, 0), in world space or the emitter's local space; dead when age >= lifetime.
BUFFER_RW(b_particles, vec4, 0);

uniform vec4 u_sim[7];
// [0] x: first ring slot to spawn into, y: particles to spawn, z: ring size, w: dt (s)
// [1] x: serial of the first new particle (random stream), y: emitter seed, z: 1 kills every particle first
// [2] x: shape (0 cone, 1 sphere, 2 box), y: cos(cone angle), z: radius, w: 1 emits from the surface
// [3] xyz: box size (full extents)
// [4] lifetime min, max; speed min, max
// [5] start size min, max
// [6] xyz: gravity in the particles' space
uniform mat4 u_simWorld; // spawn space to the particles' space: the emitter's world matrix, or identity (local space)

#define PI 3.14159265

// PCG hash (Jarzynski & Olano, "Hash Functions for GPU Rendering").
uint pcgHash(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float random(inout uint state)
{
    state = pcgHash(state);
    return float(state & 0xffffffu) / 16777216.0;
}

NUM_THREADS(64, 1, 1)
void main()
{
    uint i = gl_GlobalInvocationID.x;
    uint capacity = uint(u_sim[0].z);
    if (i >= capacity)
        return;
    float dt = u_sim[0].w;
    uint offset = (i + capacity - uint(u_sim[0].x)) % capacity;
    if (offset < uint(u_sim[0].y)) { // a new particle
        uint state = pcgHash(uint(u_sim[1].y)) + uint(u_sim[1].x) + offset;
        float radius = u_sim[2].z;
        bool surface = u_sim[2].w > 0.5;
        vec3 pos;
        vec3 dir;
        if (u_sim[2].x < 0.5) { // cone around +Y from a disc
            float c = mix(1.0, u_sim[2].y, random(state));
            float s = sqrt(max(1.0 - c * c, 0.0));
            float phi = 2.0 * PI * random(state);
            dir = vec3(s * cos(phi), c, s * sin(phi));
            float r = radius * (surface ? 1.0 : sqrt(random(state)));
            float a = 2.0 * PI * random(state);
            pos = vec3(r * cos(a), 0.0, r * sin(a));
        } else if (u_sim[2].x < 1.5) { // sphere: outward
            float z = 2.0 * random(state) - 1.0;
            float s = sqrt(max(1.0 - z * z, 0.0));
            float phi = 2.0 * PI * random(state);
            dir = vec3(s * cos(phi), z, s * sin(phi));
            pos = dir * radius * (surface ? 1.0 : pow(random(state), 1.0 / 3.0));
        } else { // box: along +Y
            pos = (vec3(random(state), random(state), random(state)) - 0.5) * u_sim[3].xyz;
            if (surface) { // onto a random face
                int axis = min(int(random(state) * 3.0), 2);
                float side = random(state) < 0.5 ? -0.5 : 0.5;
                if (axis == 0)
                    pos.x = side * u_sim[3].x;
                else if (axis == 1)
                    pos.y = side * u_sim[3].y;
                else
                    pos.z = side * u_sim[3].z;
            }
            dir = vec3(0.0, 1.0, 0.0);
        }
        float lifetime = max(mix(u_sim[4].x, u_sim[4].y, random(state)), 0.0001);
        float speed = mix(u_sim[4].z, u_sim[4].w, random(state));
        float size = mix(u_sim[5].x, u_sim[5].y, random(state));
        float age = dt * random(state); // spread over the frame, so a stream doesn't come out in clumps
        vec3 worldDir = mul(u_simWorld, vec4(dir, 0.0)).xyz;
        vec3 velocity = worldDir * (speed / max(length(worldDir), 0.000001));
        pos = mul(u_simWorld, vec4(pos, 1.0)).xyz + velocity * age;
        b_particles[i * 3u] = vec4(pos, age);
        b_particles[i * 3u + 1u] = vec4(velocity, lifetime);
        b_particles[i * 3u + 2u] = vec4(size, 0.0, 0.0, 0.0);
        return;
    }
    if (u_sim[1].z > 0.5) { // cleared
        b_particles[i * 3u] = vec4(0.0, 0.0, 0.0, 0.0);
        b_particles[i * 3u + 1u] = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    vec4 p = b_particles[i * 3u];
    vec4 v = b_particles[i * 3u + 1u];
    if (p.w >= v.w)
        return; // dead
    v.xyz += u_sim[6].xyz * dt;
    p.xyz += v.xyz * dt;
    p.w += dt;
    b_particles[i * 3u] = p;
    b_particles[i * 3u + 1u] = v;
}
