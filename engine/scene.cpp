#include "scene.h"

#include "components.h"

#include <algorithm>

namespace kast {

Aabb primitiveBounds(Primitive primitive)
{
    if (primitive == Primitive::Plane)
        return {{-0.5f, 0.0f, -0.5f}, {0.5f, 0.0f, 0.5f}};
    return {{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
}

entt::entity Scene::create(const std::string& name, entt::entity parent)
{
    return createWithId(nextId_, name, parent);
}

entt::entity Scene::createWithId(EntityId id, const std::string& name, entt::entity parent)
{
    if (parent != entt::null && depth(parent) >= kMaxHierarchyDepth)
        return entt::null;
    const entt::entity entity = registry_.create();
    registry_.emplace<PersistentId>(entity, id);
    registry_.emplace<Name>(entity, name);
    registry_.emplace<Transform>(entity);
    registry_.emplace<Hierarchy>(entity, parent);
    siblings(parent).push_back(entity);
    nextId_ = std::max(nextId_, id + 1);
    ++creations_;
    return entity;
}

void Scene::destroy(entt::entity entity)
{
    if (!valid(entity))
        return;
    std::vector<entt::entity>& list = siblings(parent(entity));
    list.erase(std::find(list.begin(), list.end(), entity));

    std::vector<entt::entity> subtree{entity};
    for (size_t i = 0; i < subtree.size(); ++i) {
        const auto& kids = registry_.get<Hierarchy>(subtree[i]).children;
        subtree.insert(subtree.end(), kids.begin(), kids.end());
    }
    registry_.destroy(subtree.begin(), subtree.end());
}

entt::entity Scene::duplicate(entt::entity entity)
{
    const entt::entity parentEntity = parent(entity);
    const entt::entity copy = copySubtree(entity, parentEntity);
    // copySubtree appended it last; move it right after the original.
    std::vector<entt::entity>& list = siblings(parentEntity);
    list.pop_back();
    list.insert(std::find(list.begin(), list.end(), entity) + 1, copy);
    return copy;
}

entt::entity Scene::copySubtree(entt::entity source, entt::entity parentEntity)
{
    const std::string name = registry_.get<Name>(source).value; // copy: create() may grow the Name pool
    const entt::entity copy = create(name, parentEntity);
    copyComponents(registry_, source, copy);
    // Copy the child list first: create() appends to vectors that may reallocate.
    const std::vector<entt::entity> kids = registry_.get<Hierarchy>(source).children;
    for (entt::entity child : kids)
        copySubtree(child, copy);
    return copy;
}

bool Scene::reparent(entt::entity entity, entt::entity newParent)
{
    if (!valid(entity) || (newParent != entt::null && (!valid(newParent) || isSelfOrAncestor(entity, newParent))))
        return false;
    if (newParent != entt::null && depth(newParent) + height(entity) > kMaxHierarchyDepth)
        return false;
    const entt::entity oldParent = parent(entity);
    if (oldParent == newParent)
        return true;

    float world[16];
    worldMatrix(entity, world);
    float local[16];
    if (newParent == entt::null) {
        std::copy_n(world, 16, local);
    } else {
        float parentWorld[16];
        float parentInverse[16];
        worldMatrix(newParent, parentWorld);
        bx::mtxInverse(parentInverse, parentWorld);
        bx::mtxMul(local, world, parentInverse); // world = local * parentWorld
    }
    Transform& transform = registry_.get<Transform>(entity);
    decomposeTrs(local, transform.position, transform.rotation, transform.scale);

    std::vector<entt::entity>& oldList = siblings(oldParent);
    oldList.erase(std::find(oldList.begin(), oldList.end(), entity));
    siblings(newParent).push_back(entity);
    registry_.get<Hierarchy>(entity).parent = newParent;
    return true;
}

void Scene::setSiblingIndex(entt::entity entity, size_t index)
{
    if (!valid(entity))
        return;
    std::vector<entt::entity>& list = siblings(parent(entity));
    list.erase(std::find(list.begin(), list.end(), entity));
    list.insert(list.begin() + static_cast<ptrdiff_t>(std::min(index, list.size())), entity);
}

size_t Scene::siblingIndex(entt::entity entity) const
{
    const std::vector<entt::entity>& list = children(parent(entity));
    return static_cast<size_t>(std::find(list.begin(), list.end(), entity) - list.begin());
}

bool Scene::isSelfOrAncestor(entt::entity ancestor, entt::entity entity) const
{
    for (entt::entity e = entity; e != entt::null; e = parent(e))
        if (e == ancestor)
            return true;
    return false;
}

int Scene::depth(entt::entity entity) const
{
    int levels = 0;
    for (entt::entity e = entity; e != entt::null; e = parent(e))
        ++levels;
    return levels;
}

int Scene::height(entt::entity entity) const
{
    int levels = 0;
    for (std::vector<entt::entity> level{entity}; !level.empty(); ++levels) {
        std::vector<entt::entity> next;
        for (entt::entity e : level)
            next.insert(next.end(), children(e).begin(), children(e).end());
        level = std::move(next);
    }
    return levels;
}

const std::vector<entt::entity>& Scene::children(entt::entity entity) const
{
    return entity == entt::null ? roots_ : registry_.get<Hierarchy>(entity).children;
}

std::vector<entt::entity>& Scene::siblings(entt::entity parentEntity)
{
    return parentEntity == entt::null ? roots_ : registry_.get<Hierarchy>(parentEntity).children;
}

void Scene::localMatrix(entt::entity entity, float* out) const
{
    const Transform& t = registry_.get<Transform>(entity);
    mtxFromTrs(out, t.position, t.rotation, t.scale);
}

entt::entity activeCamera(const Scene& scene)
{
    return firstDepthFirst<Camera>(scene);
}

void cameraMatrices(const Scene& scene, entt::entity camera, float aspect, bool homogeneousDepth, float* view, float* proj)
{
    float world[16];
    scene.worldMatrix(camera, world);
    const bx::Vec3 eye{world[12], world[13], world[14]};
    const bx::Vec3 forward = bx::normalize(transformDir({0.0f, 0.0f, -1.0f}, world));
    const bx::Vec3 up = bx::normalize(transformDir({0.0f, 1.0f, 0.0f}, world));
    bx::mtxLookAt(view, eye, bx::add(eye, forward), up, bx::Handedness::Right);
    const Camera& c = scene.registry().get<Camera>(camera);
    if (c.projection == CameraProjection::Orthographic) {
        const float h = c.orthographicSize, w = h * aspect;
        bx::mtxOrtho(proj, -w, w, -h, h, c.nearPlane, c.farPlane, 0.0f, homogeneousDepth, bx::Handedness::Right);
    } else {
        bx::mtxProj(proj, c.verticalFovDegrees, aspect, c.nearPlane, c.farPlane, homogeneousDepth, bx::Handedness::Right);
    }
}

void Scene::worldMatrix(entt::entity entity, float* out) const
{
    localMatrix(entity, out);
    const entt::entity parentEntity = parent(entity);
    if (parentEntity == entt::null)
        return;
    float parentWorld[16];
    worldMatrix(parentEntity, parentWorld);
    float local[16];
    std::copy_n(out, 16, local);
    bx::mtxMul(out, local, parentWorld);
}

} // namespace kast
