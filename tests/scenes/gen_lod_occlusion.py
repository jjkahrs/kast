"""Writes the LOD / occlusion benchmark (docs/engine/lod-and-occlusion.html#benchmark) into tests/scenes:

  models/lod_torus.obj        a 96 x 48 segment torus (9216 triangles; the importer smooths its normals)
  lod_occlusion.scene         far-field view: a 24 x 24 field of tori, two walls, seen from high and far away
  lod_occlusion_wall.scene    behind-wall view: the same scene, the camera on the ground behind the near wall

The two scenes differ only in the Main Camera's transform and the editor camera. Run: python gen_lod_occlusion.py
"""
import json
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
FIELD = 24       # tori per side
SPACING = 5.0    # metres between them
MAJOR, MINOR = 1.0, 0.4
RINGS, SIDES = 96, 48


def write_torus(path):
    lines = ["# Kast LOD benchmark model, written by tests/scenes/gen_lod_occlusion.py: a torus, %d x %d quads"
             % (RINGS, SIDES)]
    for i in range(RINGS):
        a = 2.0 * math.pi * i / RINGS
        for j in range(SIDES):
            b = 2.0 * math.pi * j / SIDES
            r = MAJOR + MINOR * math.cos(b)
            lines.append("v %.4f %.4f %.4f" % (r * math.cos(a), MINOR * math.sin(b) + MINOR, r * math.sin(a)))
    for i in range(RINGS):
        for j in range(SIDES):
            def v(ii, jj):
                return (ii % RINGS) * SIDES + (jj % SIDES) + 1
            lines.append("f %d %d %d %d" % (v(i, j), v(i, j + 1), v(i + 1, j + 1), v(i + 1, j)))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def quat_pitch(degrees):
    h = math.radians(degrees) / 2.0
    return [round(math.sin(h), 6), 0, 0, round(math.cos(h), 6)]


def entity(eid, name, components):
    return {"id": eid, "name": name, "parent": None, "components": components}


def transform(position, rotation=(0, 0, 0, 1), scale=(1, 1, 1)):
    return {"position": list(position), "rotation": list(rotation), "scale": list(scale)}


def scene(eye, pitch):
    entities = [
        entity(1, "Directional Light", {
            "Transform": transform((0, 10, 0), (-0.408218, -0.23457, -0.109382, 0.875426)),
            "DirectionalLight": {"color": [1, 0.95, 0.9], "intensity": 1.1}}),
        entity(2, "Ground", {"Transform": transform((0, 0, 0), scale=(400, 1, 400)),
                             "MeshRenderer": {"primitive": "plane", "material": ""}}),
        # The near wall hides most of the field from the behind-wall camera; the middle one hides some from the far one.
        entity(3, "Near Wall", {"Transform": transform((0, 7.5, 65), scale=(80, 15, 1)),
                                "MeshRenderer": {"primitive": "cube", "material": ""}}),
        entity(4, "Middle Wall", {"Transform": transform((0, 5, 0), scale=(40, 10, 1)),
                                  "MeshRenderer": {"primitive": "cube", "material": ""}}),
        entity(5, "Main Camera", {"Transform": transform(eye, quat_pitch(pitch)),
                                  "Camera": {"verticalFovDegrees": 60, "nearPlane": 0.1, "farPlane": 1000}}),
    ]
    eid = 6
    half = (FIELD - 1) * SPACING / 2.0
    for row in range(FIELD):
        for col in range(FIELD):
            x, z = col * SPACING - half, row * SPACING - half
            entities.append(entity(eid, "Torus %d_%d" % (row, col), {
                "Transform": transform((x, 0, z)),
                "ModelRenderer": {"model": "models/lod_torus.obj", "materials": []}}))
            eid += 1
    # Editor camera = the Main Camera: yaw 0 looks down -Z, the pivot 10 m ahead.
    forward = (0.0, math.sin(math.radians(pitch)), -math.cos(math.radians(pitch)))
    pivot = [round(eye[i] + forward[i] * 10.0, 4) for i in range(3)]
    return {"version": 5, "entities": entities,
            "editor": {"camera": {"pivot": pivot, "yaw": 0, "pitch": pitch, "distance": 10}}}


def write_scene(name, eye, pitch):
    data = scene(eye, pitch)
    with open(os.path.join(HERE, name), "w", newline="\n") as f:  # an entity per line
        f.write('{\n  "version": 5,\n  "entities": [\n    ')
        f.write(",\n    ".join(json.dumps(e) for e in data["entities"]))
        f.write('\n  ],\n  "editor": %s\n}\n' % json.dumps(data["editor"]))


write_torus(os.path.join(HERE, "models", "lod_torus.obj"))
write_scene("lod_occlusion.scene", (0, 60, 160), -20.56)      # far field: every torus small on screen
write_scene("lod_occlusion_wall.scene", (0, 2, 110), 0)       # behind the near wall, 45 m from it
