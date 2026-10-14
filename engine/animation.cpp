#include "animation.h"

#include "material.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

namespace kast {
namespace {

bx::Vec3 lerpVec3(const bx::Vec3& a, const bx::Vec3& b, float t) { return bx::add(a, bx::mul(bx::sub(b, a), t)); }

// The value of keys at time: the end keys hold outside their range; between keys, interpolate.
template <typename T, typename Interpolate>
T sample(const std::vector<Key<T>>& keys, float time, Interpolate interpolate)
{
    if (time <= keys.front().time)
        return keys.front().value;
    if (time >= keys.back().time)
        return keys.back().value;
    const auto next = std::ranges::upper_bound(keys, time, {}, &Key<T>::time);
    const auto prev = next - 1;
    const float span = next->time - prev->time;
    return interpolate(prev->value, next->value, span > 0.0f ? (time - prev->time) / span : 0.0f);
}

std::string lower(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace

Pose bindPose(const Model& model)
{
    Pose pose;
    pose.reserve(model.nodes.size());
    for (const ModelNode& node : model.nodes)
        pose.push_back(node.bind);
    return pose;
}

float clipTime(float time, float duration, bool loop)
{
    if (duration <= 0.0f)
        return 0.0f;
    if (!loop)
        return bx::clamp(time, 0.0f, duration);
    const float wrapped = std::fmod(time, duration);
    return wrapped < 0.0f ? wrapped + duration : wrapped;
}

bx::Quaternion slerp(const bx::Quaternion& a, const bx::Quaternion& b, float t)
{
    float cosTheta = bx::dot(a, b);
    bx::Quaternion to = b;
    if (cosTheta < 0.0f) { // q and -q are the same rotation: take the short way
        cosTheta = -cosTheta;
        to = {-b.x, -b.y, -b.z, -b.w};
    }
    float wa = 1.0f - t, wb = t;
    if (cosTheta < 0.9995f) { // nearly parallel: the linear weights are exact enough and avoid dividing by ~0
        const float theta = std::acos(cosTheta);
        const float sinTheta = std::sin(theta);
        wa = std::sin(wa * theta) / sinTheta;
        wb = std::sin(wb * theta) / sinTheta;
    }
    return bx::normalize(bx::Quaternion{a.x * wa + to.x * wb, a.y * wa + to.y * wb, a.z * wa + to.z * wb,
                                        a.w * wa + to.w * wb});
}

void sampleClip(const Clip& clip, float time, Pose& pose)
{
    for (const Channel& channel : clip.channels) {
        Trs& trs = pose[static_cast<size_t>(channel.node)];
        if (!channel.translation.empty())
            trs.translation = sample(channel.translation, time, lerpVec3);
        if (!channel.rotation.empty())
            trs.rotation = sample(channel.rotation, time, slerp);
        if (!channel.scale.empty())
            trs.scale = sample(channel.scale, time, lerpVec3);
    }
}

void blendPoses(const Pose& from, const Pose& to, float weight, Pose& out)
{
    out.resize(to.size());
    for (size_t i = 0; i < to.size(); ++i) {
        const Trs& a = i < from.size() ? from[i] : to[i];
        out[i] = {lerpVec3(a.translation, to[i].translation, weight), slerp(a.rotation, to[i].rotation, weight),
                  lerpVec3(a.scale, to[i].scale, weight)};
    }
}

float fadeWeight(float elapsed, float duration)
{
    return duration <= 0.0f ? 1.0f : bx::clamp(elapsed / duration, 0.0f, 1.0f);
}

void nodeGlobals(const Model& model, const Pose& pose, std::vector<Mat4>& out)
{
    out.resize(model.nodes.size());
    for (size_t i = 0; i < model.nodes.size(); ++i) {
        const Trs& trs = pose[i];
        Mat4 local;
        mtxFromTrs(local.data(), trs.translation, trs.rotation, trs.scale);
        const int parent = model.nodes[i].parent;
        if (parent < 0)
            out[i] = local;
        else // parents come first, so theirs is ready
            bx::mtxMul(out[i].data(), local.data(), out[static_cast<size_t>(parent)].data());
    }
}

void skinMatrices(const ModelMesh& mesh, const std::vector<Mat4>& globals, std::vector<Mat4>& out)
{
    out.resize(mesh.joints.size());
    for (size_t i = 0; i < mesh.joints.size(); ++i)
        bx::mtxMul(out[i].data(), mesh.inverseBinds[i].data(), globals[static_cast<size_t>(mesh.joints[i])].data());
}

int findClip(const Model& model, std::string_view name)
{
    for (size_t i = 0; i < model.clips.size(); ++i)
        if (model.clips[i].name == name)
            return static_cast<int>(i);
    return -1;
}

std::string defaultClip(const Model& model)
{
    for (const char* preferred : {"Idle", "Idle_A"})
        if (findClip(model, preferred) >= 0)
            return preferred;
    for (const Clip& clip : model.clips)
        if (lower(clip.name).find("idle") != std::string::npos)
            return clip.name;
    return model.clips.empty() ? std::string() : model.clips.front().name;
}

void playClip(AnimatorState& state, int clip, float fade)
{
    state.fadeFrom = state.pose;
    state.fade = state.pose.empty() ? 0.0f : fade; // nothing posed yet: nothing to fade from
    state.fadeElapsed = 0.0f;
    state.clip = clip;
    state.time = 0.0f;
}

void advanceAnimator(AnimatorState& state, const Model& model, bool loop, float speed, float dt)
{
    const Clip* clip = state.clip >= 0 && static_cast<size_t>(state.clip) < model.clips.size()
                           ? &model.clips[static_cast<size_t>(state.clip)]
                           : nullptr;
    if (state.playing) {
        state.time += dt * speed;
        state.fadeElapsed += dt;
        if (clip && !loop && state.time >= clip->duration) {
            state.time = clip->duration;
            state.playing = false;
        }
    }
    Pose target = bindPose(model);
    if (clip)
        sampleClip(*clip, clipTime(state.time, clip->duration, loop), target);
    const float weight = fadeWeight(state.fadeElapsed, state.fade);
    if (weight < 1.0f && state.fadeFrom.size() == target.size())
        blendPoses(state.fadeFrom, target, weight, state.pose);
    else
        state.pose = std::move(target);
    nodeGlobals(model, state.pose, state.globals);
}

void animatorSystem(Scene& scene, float dt)
{
    entt::registry& registry = scene.registry();
    for (auto [entity, animator, renderer] : registry.view<Animator, ModelRenderer>().each()) {
        const std::shared_ptr<const Model> model = models().get(renderer.model);
        if (!model) {
            registry.remove<AnimatorState>(entity);
            continue;
        }
        AnimatorState& state = registry.get_or_emplace<AnimatorState>(entity);
        if (state.model != model.get()) { // first tick or a reload: indices and pose sizes may have changed
            state.model = model.get();
            state.clip = findClip(*model, state.clipName);
            state.pose.clear();
            state.fadeFrom.clear();
        }
        if (!state.started || state.clipName != animator.clip) {
            state.playing = state.started ? state.playing : animator.playOnStart;
            state.started = true;
            state.clipName = animator.clip;
            playClip(state, findClip(*model, animator.clip), 0.0f);
            if (state.clip < 0 && !animator.clip.empty())
                warnOnce(std::format("Animator on '{}': model '{}' has no clip '{}'; showing the bind pose",
                                     registry.get<Name>(entity).value, renderer.model, animator.clip));
        }
        advanceAnimator(state, *model, animator.loop, animator.speed, dt);
    }
}

} // namespace kast
