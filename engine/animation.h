#pragma once

#include "model.h"
#include "scene.h"

#include <string>
#include <string_view>
#include <vector>

// Skeletal animation: key sampling, crossfades and skinning matrices on Model data, plus the per-entity playback state
// the Animator system advances. GPU-free; the renderer reads AnimatorState::globals.
namespace kast {

using Pose = std::vector<Trs>; // a local transform per model node

Pose bindPose(const Model& model);
// Time within a clip: wrapped into [0, duration) when looping, clamped to [0, duration] otherwise.
float clipTime(float time, float duration, bool loop);
bx::Quaternion slerp(const bx::Quaternion& a, const bx::Quaternion& b, float t); // shortest path
// Overwrites the nodes clip animates with their keys at time (lerp translation/scale, slerp rotation; before the first
// key or after the last the end key holds). Other nodes keep their values.
void sampleClip(const Clip& clip, float time, Pose& pose);
// weight 0 = from, 1 = to.
void blendPoses(const Pose& from, const Pose& to, float weight, Pose& out);
// Crossfade weight elapsed seconds into a fade lasting duration: 0 at the start, 1 at the end; 1 if duration <= 0.
float fadeWeight(float elapsed, float duration);
// Model-space transform of every node.
void nodeGlobals(const Model& model, const Pose& pose, std::vector<Mat4>& out);
// Bone matrices of a skinned mesh (inverse bind, then the joint's global): mesh space to model space.
void skinMatrices(const ModelMesh& mesh, const std::vector<Mat4>& globals, std::vector<Mat4>& out);
int findClip(const Model& model, std::string_view name); // -1 if there is none
// The clip a newly placed model plays: "Idle", else "Idle_A", else the first containing "idle" (any case), else the
// first; empty if it has none.
std::string defaultClip(const Model& model);

// Playback state of an Animator, added by the Animator system. Not registered: never saved, never in the Inspector,
// gone when Stop restores the scene.
struct AnimatorState {
    const Model* model = nullptr; // what clip refers to; a reloaded model re-resolves it by name
    std::string clipName;         // the Animator::clip last applied: a different one starts that clip
    int clip = -1;                // -1: bind pose
    float time = 0.0f;            // seconds into the clip (before wrapping)
    bool playing = false;
    bool started = false; // playOnStart applied
    Pose pose;            // current local pose
    Pose fadeFrom;        // the pose a crossfade started from
    float fade = 0.0f, fadeElapsed = 0.0f;
    std::vector<Mat4> globals; // node globals of pose, for the renderer
};

// Switches state to clip (-1: bind pose) from time 0, crossfading from the current pose over fade seconds.
void playClip(AnimatorState& state, int clip, float fade);
// Advances time by dt * speed while playing (a clip that is not looping stops at its end), then poses the model.
void advanceAnimator(AnimatorState& state, const Model& model, bool loop, float speed, float dt);

// Plays every Animator on an entity with a ModelRenderer: starts playOnStart clips, follows Animator::clip changes,
// advances and poses. An Animator without a ModelRenderer (or with a model that fails to load) does nothing; an
// unknown clip shows the bind pose and warns once. Runs in Play mode and, in the editor, in Edit mode too.
void animatorSystem(Scene& scene, float dt);

} // namespace kast
