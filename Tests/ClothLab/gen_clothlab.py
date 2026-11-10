"""Generates the ClothLab test project: mannequins that wear a generated cape with character cloth on it.

Usage: python gen_clothlab.py <sdk root> <project dir> <mannequin dir> <package dir> [--configuration Development]

  sdk root       the engine checkout the project runs with
  project dir    where the project is written; every file is written fresh
  mannequin dir  a folder with the CC0 Quaternius mannequin: UAL1_Standard.glb, its skeleton asset (with shapes),
                 its material assets and the UAL1_Jog / UAL1_Idle clip assets
  package dir    the plasma.jolt checkout whose build is installed into the project

The cape is added to a copy of the mannequin's glb as a mesh named CLOTH_Cape, skinned to the spine, with its pin
mask in the red vertex colour. Asset GUIDs are fixed so that references stay valid across regenerations.
"""

import hashlib
import json
import math
import shutil
import struct
import sys
import uuid
from pathlib import Path

import numpy as np

MESH_GUID = uuid.UUID("c107c000-434c-4f54-8000-000000000001")
CLOTH_GUID = uuid.UUID("c107c000-434c-4f54-8000-000000000002")
CAPE_MATERIAL_GUID = uuid.UUID("c107c000-434c-4f54-8000-000000000003")
SCENE_GUID = uuid.UUID("c107c000-434c-4f54-8000-000000000020")

TEMPLATE_SCENE_ID = "u4{14841776606954812814,5457590759842316574}"

# cape grid
CAPE_COLUMNS = 15
CAPE_ROWS = 26

# The camera rig looks along +X. The mannequins stand around the origin.
CAMERA_POSITION = (-3.4, 0.0, 1.35)
CAMERA_PITCH_DEGREES = 6.0

# The template's sun is in physical units; this exposes a sunlit scene.
EXPOSURE_EV100 = 12.0


def u4(guid: uuid.UUID) -> str:
    d1, d2, d3 = guid.fields[0], guid.fields[1], guid.fields[2]
    b = d1 | (d2 << 32) | (d3 << 48)
    a = int.from_bytes(guid.bytes[8:], "little")
    return f"u4{{{a},{b}}}"


def guid_from_u4(a: int, b: int) -> uuid.UUID:
    tail = a.to_bytes(8, "little")
    return uuid.UUID(fields=(b & 0xFFFFFFFF, (b >> 32) & 0xFFFF, (b >> 48) & 0xFFFF, tail[0], tail[1], int.from_bytes(tail[2:], "big")))


def guid_ref(guid: uuid.UUID) -> str:
    return "{ " + str(guid) + " }"


def f32(value: float) -> str:
    if value == 0.0:
        return "0"
    return "0x" + struct.pack("<f", value).hex().upper()


def vec(values) -> str:
    return "f{" + ",".join(f32(v) for v in values) + "}"


def content_hash(text: str) -> int:
    digest = hashlib.sha256(text.encode("utf-8")).digest()
    return (int.from_bytes(digest[:8], "little") | 1) & 0xFFFFFFFFFFFFFFFF


def new_id(seed: str) -> str:
    return u4(uuid.uuid5(uuid.NAMESPACE_URL, "clothlab/" + seed))


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def document_guid(path: Path) -> uuid.UUID:
    """The GUID of an asset document, from its header."""
    import re

    match = re.search(r"Uuid %DocumentID\{u4\{(\d+),(\d+)\}\}", path.read_text(encoding="utf-8"))
    assert match, f"{path} has no document id"
    return guid_from_u4(int(match.group(1)), int(match.group(2)))


def asset_header(asset_type: str, guid: uuid.UUID, body: str, references=(), dependencies=()) -> str:
    refs = "".join(f"\t\t\ts{{\"{r}\"}}\n" for r in references)
    ref_block = "{}" if not references else "\n\t\t{\n" + refs + "\t\t}"
    deps = "".join(f"\t\t\ts{{\"{d}\"}}\n" for d in dependencies)
    dep_block = "{}" if not dependencies else "\n\t\t{\n" + deps + "\t\t}"
    return f"""HeaderV2
{{
o
{{
	Uuid %id{{{u4(guid)}}}
	s %t{{"plAssetDocumentInfo"}}
	u3 %v{{2}}
	s %n{{"Header"}}
	p
	{{
		s %AssetType{{"{asset_type}"}}
		VarArray %Dependencies{dep_block}
		Uuid %DocumentID{{{u4(guid)}}}
		u4 %Hash{{{content_hash(body)}}}
		VarArray %MetaInfo{{}}
		VarArray %Outputs{{}}
		VarArray %PackageDeps{ref_block}
		VarArray %References{ref_block}
	}}
}}
}}
"""


DOCUMENT_ROOT = """o
{{
	Uuid %id{{u4{{18096612296587978288,6449934965513159559}}}}
	s %t{{"plDocumentRoot"}}
	u3 %v{{1}}
	s %n{{"ObjectTree"}}
	p
	{{
		VarArray %Children
		{{
			Uuid{{{child}}}
		}}
	}}
}}
"""


# Cape ---------------------------------------------------------------------------------------------------------------

class Glb:
    def __init__(self, path: Path):
        data = path.read_bytes()
        magic, version, length = struct.unpack_from("<III", data, 0)
        assert magic == 0x46546C67 and version == 2, "not a glb 2 file"

        offset = 12
        chunks = []
        while offset < length:
            chunk_length, chunk_type = struct.unpack_from("<II", data, offset)
            chunks.append((chunk_type, data[offset + 8: offset + 8 + chunk_length]))
            offset += 8 + chunk_length

        self.json = json.loads(chunks[0][1])
        self.binary = bytearray(chunks[1][1])

    def accessor(self, index: int) -> np.ndarray:
        accessor = self.json["accessors"][index]
        view = self.json["bufferViews"][accessor["bufferView"]]
        component = {5120: "b", 5121: "B", 5122: "h", 5123: "H", 5125: "I", 5126: "f"}[accessor["componentType"]]
        count = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[accessor["type"]]
        start = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
        assert not view.get("byteStride"), "interleaved buffer views are not handled"
        return np.frombuffer(bytes(self.binary), dtype=np.dtype("<" + component), count=accessor["count"] * count, offset=start).reshape(accessor["count"], count)

    def add_accessor(self, array: np.ndarray, kind: str, target: int, with_bounds=False) -> int:
        while len(self.binary) % 4:
            self.binary.append(0)

        component = {np.dtype("<f4"): 5126, np.dtype("uint8"): 5121, np.dtype("<u2"): 5123}[array.dtype]
        raw = array.tobytes()

        self.json["bufferViews"].append({"buffer": 0, "byteOffset": len(self.binary), "byteLength": len(raw), "target": target})
        self.binary.extend(raw)

        accessor = {"bufferView": len(self.json["bufferViews"]) - 1, "componentType": component, "count": int(array.shape[0]), "type": kind}
        if with_bounds:
            accessor["min"] = [float(v) for v in array.min(axis=0)]
            accessor["max"] = [float(v) for v in array.max(axis=0)]

        self.json["accessors"].append(accessor)
        return len(self.json["accessors"]) - 1

    def save(self, path: Path) -> None:
        while len(self.binary) % 4:
            self.binary.append(0)

        self.json["buffers"][0]["byteLength"] = len(self.binary)

        text = json.dumps(self.json, separators=(",", ":")).encode("utf-8")
        text += b" " * (-len(text) % 4)

        total = 12 + 8 + len(text) + 8 + len(self.binary)

        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "wb") as out:
            out.write(struct.pack("<III", 0x46546C67, 2, total))
            out.write(struct.pack("<II", len(text), 0x4E4F534A))
            out.write(text)
            out.write(struct.pack("<II", len(self.binary), 0x004E4942))
            out.write(self.binary)


def add_cape(source: Path, target: Path) -> dict:
    """Writes a copy of the mannequin glb with a cape. glTF space: Y up, the mannequin looks along +Z."""
    glb = Glb(source)
    gltf = glb.json

    skin = gltf["skins"][0]
    joint_names = [gltf["nodes"][n].get("name") for n in skin["joints"]]
    joint = {name: joint_names.index(name) for name in ("pelvis", "spine_01", "spine_02", "spine_03")}

    inverse_bind = glb.accessor(skin["inverseBindMatrices"]).reshape(-1, 4, 4).transpose(0, 2, 1)
    joint_height = {name: float(np.linalg.inv(inverse_bind[index].astype(np.float64))[1, 3]) for name, index in joint.items()}

    # the body, to lay the cape on its back
    body = np.concatenate([glb.accessor(p["attributes"]["POSITION"]) for mesh in gltf["meshes"] for p in mesh["primitives"]])

    def back_of_body(x: float, y: float):
        near = body[(np.abs(body[:, 0] - x) < 0.03) & (np.abs(body[:, 1] - y) < 0.03)]
        return float(near[:, 2].min()) if len(near) else None

    positions = np.zeros((CAPE_ROWS * CAPE_COLUMNS, 3), dtype="<f4")
    uvs = np.zeros((CAPE_ROWS * CAPE_COLUMNS, 2), dtype="<f4")
    joints = np.zeros((CAPE_ROWS * CAPE_COLUMNS, 4), dtype="uint8")
    weights = np.zeros((CAPE_ROWS * CAPE_COLUMNS, 4), dtype="<f4")
    colors = np.zeros((CAPE_ROWS * CAPE_COLUMNS, 4), dtype="<f4")

    spine = ["spine_03", "spine_02", "spine_01", "pelvis"]

    for r in range(CAPE_ROWS):
        v = r / (CAPE_ROWS - 1)
        y = 1.47 - v * 1.02
        half_width = 0.19 + 0.13 * v

        for c in range(CAPE_COLUMNS):
            index = r * CAPE_COLUMNS + c
            u = c / (CAPE_COLUMNS - 1)
            side = (u - 0.5) * 2.0

            x = side * half_width

            # behind the back, drifting away from it towards the hem; the collar's ends wrap forward over the shoulders
            z = -0.15 - 0.05 * v + 0.10 * side * side * (1.0 - v) ** 2

            body_back = back_of_body(x, y)
            if body_back is not None:
                z = min(z, body_back - 0.015)

            positions[index] = (x, y, z)
            uvs[index] = (u, v)

            # skinned to the spine joint at its height, blended with the next one down
            for upper, lower in zip(spine, spine[1:]):
                top, bottom = joint_height[upper], joint_height[lower]
                if y >= top:
                    joints[index, 0], weights[index, 0] = joint[upper], 1.0
                    break
                if y >= bottom:
                    t = (top - y) / (top - bottom)
                    joints[index, 0], weights[index, 0] = joint[upper], 1.0 - t
                    joints[index, 1], weights[index, 1] = joint[lower], t
                    break
            else:
                joints[index, 0], weights[index, 0] = joint["pelvis"], 1.0

            # red pins: the collar is fully red, then the cape may move further the lower it hangs
            free = 0.0 if v < 0.05 else ((v - 0.05) / 0.95) ** 0.7
            colors[index] = (1.0 - free, 0.0, 0.0, 1.0)

    indices = []
    for r in range(CAPE_ROWS - 1):
        for c in range(CAPE_COLUMNS - 1):
            i0 = r * CAPE_COLUMNS + c
            i1, i2, i3 = i0 + 1, i0 + CAPE_COLUMNS, i0 + CAPE_COLUMNS + 1

            # wound so that the front of the cape faces away from the body
            indices += [i0, i1, i2, i1, i3, i2]

    indices = np.array(indices, dtype="<u2")

    normals = np.zeros_like(positions, dtype=np.float64)
    triangles = indices.reshape(-1, 3)
    face_normals = np.cross(positions[triangles[:, 1]] - positions[triangles[:, 0]], positions[triangles[:, 2]] - positions[triangles[:, 0]])
    for corner in range(3):
        np.add.at(normals, triangles[:, corner], face_normals)
    normals = (normals / np.linalg.norm(normals, axis=1, keepdims=True)).astype("<f4")

    attributes = {
        "POSITION": glb.add_accessor(positions, "VEC3", 34962, with_bounds=True),
        "NORMAL": glb.add_accessor(normals, "VEC3", 34962),
        "TEXCOORD_0": glb.add_accessor(uvs, "VEC2", 34962),
        "JOINTS_0": glb.add_accessor(joints, "VEC4", 34962),
        "WEIGHTS_0": glb.add_accessor(weights, "VEC4", 34962),
        "COLOR_0": glb.add_accessor(colors, "VEC4", 34962),
    }
    index_accessor = glb.add_accessor(indices.reshape(-1, 1), "SCALAR", 34963)

    gltf["materials"].append({"name": "M_Cape", "doubleSided": True, "pbrMetallicRoughness": {"baseColorFactor": [0.55, 0.08, 0.08, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.85}})
    gltf["meshes"].append({"name": "CLOTH_Cape", "primitives": [{"attributes": attributes, "indices": index_accessor, "material": len(gltf["materials"]) - 1}]})

    # next to the mannequin's own mesh node
    body_node = next(i for i, node in enumerate(gltf["nodes"]) if "mesh" in node)
    gltf["nodes"].append({"name": "CLOTH_Cape", "mesh": len(gltf["meshes"]) - 1, "skin": 0})

    parent = next((node for node in gltf["nodes"] if body_node in node.get("children", [])), None)
    if parent is not None:
        parent["children"].append(len(gltf["nodes"]) - 1)
    else:
        gltf["scenes"][0]["nodes"].append(len(gltf["nodes"]) - 1)

    glb.save(target)

    return {"vertices": len(positions), "triangles": len(triangles), "materials": [m["name"] for m in gltf["materials"]]}


# Assets -------------------------------------------------------------------------------------------------------------

def animated_mesh_asset(skeleton: uuid.UUID, materials) -> str:
    """materials: list of (label, guid) in the order of the glb's materials."""
    slots = ""
    slot_ids = []
    for label, guid in materials:
        slot_id = new_id("mesh/slot/" + label)
        slot_ids.append(slot_id)
        slots += f"""o
{{
	Uuid %id{{{slot_id}}}
	s %t{{"plMaterialResourceSlot"}}
	u3 %v{{1}}
	p
	{{
		s %Label{{"{label}"}}
		s %Resource{{"{guid_ref(guid)}"}}
	}}
}}
"""

    props_id = new_id("mesh/props")
    slot_list = "".join(f"\t\t\tUuid{{{s}}}\n" for s in slot_ids)

    body = f"""Objects
{{
{slots}o
{{
	Uuid %id{{{props_id}}}
	s %t{{"plAnimatedMeshAssetProperties"}}
	u3 %v{{4}}
	p
	{{
		b %AggressiveSimplification{{0}}
		s %DefaultSkeleton{{"{guid_ref(skeleton)}"}}
		b %HighPrecision{{0}}
		b %ImportMaterials{{0}}
		VarArray %Materials
		{{
{slot_list}		}}
		u1 %MaxSimplificationError{{5}}
		s %MeshExactIncludeNames{{""}}
		s %MeshExcludeTags{{"$;UCX_"}}
		s %MeshFile{{"Characters/Mannequin/Mannequin_Cape.glb"}}
		s %MeshIncludeTags{{""}}
		u1 %MeshSimplification{{50}}
		b %NormalizeWeights{{1}}
		b %RecalculateNormals{{0}}
		b %RecalculateTangents{{1}}
		b %SimplifyMesh{{0}}
		s %VertexColorConversion{{"plMeshVertexColorConversion::None"}}
	}}
}}
{DOCUMENT_ROOT.format(child=props_id)}}}
Types
{{
}}
"""
    references = [guid_ref(g) for _, g in materials] + [guid_ref(skeleton)]
    return asset_header("Animated Mesh", MESH_GUID, body, references=references, dependencies=["Characters/Mannequin/Mannequin_Cape.glb"]) + body


def cloth_asset(preview_clip: uuid.UUID) -> str:
    props_id = new_id("cloth/props")

    body = f"""Objects
{{
o
{{
	Uuid %id{{{props_id}}}
	s %t{{"plJoltClothAssetProperties"}}
	u3 %v{{1}}
	p
	{{
		f %BackstopDistance{{{f32(0.03)}}}
		f %BackstopRadius{{{f32(0.4)}}}
		f %Bend{{{f32(0.2)}}}
		f %ColliderInflate{{{f32(0.01)}}}
		s %ColliderSource{{"plJoltClothColliderSource::SkeletonShapes"}}
		VarArray %Colliders{{}}
		f %Damping{{{f32(0.2)}}}
		f %Density{{{f32(0.15)}}}
		f %Drag{{{f32(0.7)}}}
		s %FabricPreset{{"plJoltClothFabricPreset::Cotton"}}
		f %Friction{{{f32(0.2)}}}
		f %FullRateDistance{{{f32(14.0)}}}
		VarArray %Garments{{}}
		f %GravityFactor{{{f32(1.0)}}}
		u1 %Iterations{{8}}
		f %Lift{{{f32(0.2)}}}
		f %MaxDistance{{{f32(0.45)}}}
		s %Mesh{{"{guid_ref(MESH_GUID)}"}}
		s %PreviewClip{{"{guid_ref(preview_clip)}"}}
		f %Shear{{{f32(0.9)}}}
		f %SimulateDistance{{{f32(30.0)}}}
		VarArray %SkeletonShapeJoints{{}}
		f %StepRate{{{f32(60.0)}}}
		f %Stretch{{{f32(0.95)}}}
		f %TetherSlack{{{f32(1.03)}}}
		b %Tethers{{1}}
		f %Thickness{{{f32(0.01)}}}
		b %UseImportedMasks{{1}}
		f %WeldDistance{{{f32(0.0001)}}}
		s %WorldCollision{{"plJoltClothWorldCollision::None"}}
	}}
}}
{DOCUMENT_ROOT.format(child=props_id)}}}
Types
{{
}}
"""
    return asset_header("Jolt_Cloth", CLOTH_GUID, body, references=[guid_ref(MESH_GUID)], dependencies=[guid_ref(MESH_GUID)]) + body


def cape_material(template: Path) -> str:
    """The mannequin's main material with another id and colour. It is two-sided already."""
    import re

    text = template.read_text(encoding="utf-8")

    old_id = re.search(r"Uuid %DocumentID\{(u4\{\d+,\d+\})\}", text).group(1)
    text = text.replace(old_id, u4(CAPE_MATERIAL_GUID))

    text, count = re.subn(r"Color %BaseColor\{f\{[^}]*\}\}", "Color %BaseColor{" + vec((0.45, 0.04, 0.05, 1.0)) + "}", text, count=1)
    assert count == 1, "the material template has no BaseColor"

    return re.sub(r"u4 %Hash\{\d+\}", f"u4 %Hash{{{content_hash(text)}}}", text, count=1)


# Scene --------------------------------------------------------------------------------------------------------------

def game_object(obj_id: str, name: str, position, rotation=(0.0, 0.0, 0.0, 1.0), components=(), children=()) -> str:
    comps = "{}" if not components else "\n\t\t{\n" + "".join(f"\t\t\tUuid{{{c}}}\n" for c in components) + "\t\t}"
    kids = "{}" if not children else "\n\t\t{\n" + "".join(f"\t\t\tUuid{{{c}}}\n" for c in children) + "\t\t}"
    return f"""o
{{
	Uuid %id{{{obj_id}}}
	s %t{{"plGameObject"}}
	u3 %v{{1}}
	p
	{{
		b %Active{{1}}
		VarArray %Children{kids}
		VarArray %Components{comps}
		s %GlobalKey{{""}}
		Vec3 %LocalPosition{{{vec(position)}}}
		Quat %LocalRotation{{{vec(rotation)}}}
		Vec3 %LocalScaling{{f{{0x0000803F,0x0000803F,0x0000803F}}}}
		f %LocalUniformScaling{{0x0000803F}}
		s %Mode{{"plObjectMode::Automatic"}}
		s %Name{{"{name}"}}
		VarArray %Tags
		{{
			s{{"CastShadow"}}
		}}
	}}
}}
"""


def component(comp_id: str, type_name: str, version: int, properties: str) -> str:
    return f"""o
{{
	Uuid %id{{{comp_id}}}
	s %t{{"{type_name}"}}
	u3 %v{{{version}}}
	p
	{{
		b %Active{{1}}
{properties}	}}
}}
"""


def character(name: str, position, yaw_degrees: float, clip: uuid.UUID, simulate=True):
    half = math.radians(yaw_degrees) * 0.5
    rotation = (0.0, 0.0, math.sin(half), math.cos(half))

    root_id = new_id(name)
    mesh_id = new_id(name + "/mesh")
    animation_id = new_id(name + "/animation")
    cloth_id = new_id(name + "/cloth")

    text = game_object(root_id, name, position, rotation, components=(mesh_id, animation_id, cloth_id))

    text += component(mesh_id, "plAnimatedMeshComponent", 13, f"""		Color %Color{{f{{0x0000803F,0x0000803F,0x0000803F,0x0000803F}}}}
		VarArray %Materials{{}}
		s %Mesh{{"{guid_ref(MESH_GUID)}"}}
""")

    text += component(animation_id, "plSimpleAnimationComponent", 3, f"""		s %AnimationClip{{"{guid_ref(clip)}"}}
		s %AnimationMode{{"plPropertyAnimMode::Loop"}}
		b %EnableIK{{0}}
		s %InvisibleUpdateRate{{"plAnimationInvisibleUpdateRate::FullUpdate"}}
		s %RootMotionMode{{"plRootMotionMode::Ignore"}}
		f %Speed{{0x0000803F}}
""")

    text += component(cloth_id, "plJoltCharacterClothComponent", 1, f"""		f %AngularInertia{{{f32(0.7)}}}
		s %Cloth{{"{guid_ref(CLOTH_GUID)}"}}
		u1 %CollisionLayer{{0}}
		f %GravityFactor{{{f32(1.0)}}}
		s %Importance{{"plJoltClothImportance::Always"}}
		f %LinearInertia{{{f32(0.7)}}}
		f %MaxDistanceScale{{{f32(1.0)}}}
		b %Simulate{{{1 if simulate else 0}}}
		f %TeleportDistance{{{f32(2.0)}}}
		s %TeleportMode{{"plJoltClothTeleportMode::Reset"}}
		f %WindInfluence{{{f32(1.0)}}}
		s %WorldCollision{{"plJoltClothWorldCollisionMode::Asset"}}
""")

    return text, root_id


def wind(strength: str):
    """Wind that blows along +Y, across the view."""
    root_id = new_id("wind")
    comp_id = new_id("wind/comp")

    half = math.radians(90.0) * 0.5
    text = game_object(root_id, "Wind", (0.0, -4.0, 2.0), (0.0, 0.0, math.sin(half), math.cos(half)), components=(comp_id,))
    text += component(comp_id, "plSimpleWindComponent", 3, f"""		f %GustFrequency{{{f32(0.25)}}}
		f %GustStrength{{{f32(0.3)}}}
		f %Inertia{{{f32(0.6)}}}
		Angle %MaxDeviation{{f{{{f32(math.radians(20.0))}}}}}
		s %MaxWindStrength{{"plWindStrength::{strength}"}}
		s %MinWindStrength{{"plWindStrength::LightBreeze"}}
		f %Turbulence{{{f32(0.3)}}}
""")
    return text, root_id


def build_scene(sdk: Path, jog: uuid.UUID, idle: uuid.UUID, scene_guid: uuid.UUID, wind_strength, simulate=True) -> str:
    template = (sdk / "Data/Base/Editor/DocumentTemplates/Default.plScene").read_text(encoding="utf-8")
    text = template.replace(TEMPLATE_SCENE_ID, u4(scene_guid))

    pitch = math.radians(CAMERA_PITCH_DEGREES) * 0.5
    old_rig = "Vec3 %LocalPosition{f{0x2B90E6C0,0xB4998B3F,0xF4469B40}}\n\t\tQuat %LocalRotation{f{0x6E9DFA3C,0x0BD2B93D,0x36D5A2BE,0xA877713F}}"
    new_rig = f"Vec3 %LocalPosition{{{vec(CAMERA_POSITION)}}}\n\t\tQuat %LocalRotation{{{vec((0.0, math.sin(pitch), 0.0, math.cos(pitch)))}}}"
    assert old_rig in text, "camera rig transform not found in the template"
    text = text.replace(old_rig, new_rig)

    # A fixed exposure: captures of the same scene then look the same, whatever the metering does.
    assert 's %ExposureMode{"plCameraExposureMode::Auto"}' in text and "f %ManualEV100{0x00004041}" in text
    text = text.replace('s %ExposureMode{"plCameraExposureMode::Auto"}', 's %ExposureMode{"plCameraExposureMode::Manual"}')
    text = text.replace("f %ManualEV100{0x00004041}", f"f %ManualEV100{{{f32(EXPOSURE_EV100)}}}")

    # Three mannequins turned three ways, so that one view shows the cape from behind, from the side and from the front.
    objects = ""
    roots = []
    for name, position, yaw, clip in (
        ("Jogger_Back", (0.0, -1.3, 0.0), 0.0, jog),
        ("Jogger_Side", (0.0, 0.0, 0.0), 110.0, jog),
        ("Idle_Front", (0.0, 1.3, 0.0), 215.0, idle),
    ):
        obj_text, root = character(name, position, yaw, clip, simulate)
        objects += obj_text
        roots.append(root)

    if wind_strength is not None:
        obj_text, root = wind(wind_strength)
        objects += obj_text
        roots.append(root)

    marker = "Objects\n{\n"
    assert marker in text
    text = text.replace(marker, marker + objects, 1)

    root_children = "\t\tVarArray %Children\n\t\t{\n\t\t\tUuid{u4{15628317284442516612,4804267580480477271}}"
    assert root_children in text, "scene root children not found in the template"
    text = text.replace(root_children, root_children + "".join(f"\n\t\t\tUuid{{{r}}}" for r in roots), 1)

    refs = [guid_ref(MESH_GUID), guid_ref(CLOTH_GUID), guid_ref(jog), guid_ref(idle)]
    for block in ("VarArray %PackageDeps\n\t\t{\n", "VarArray %References\n\t\t{\n"):
        assert block in text
        text = text.replace(block, block + "".join(f"\t\t\ts{{\"{r}\"}}\n" for r in refs), 1)

    old_hash = "u4 %Hash{16167880805885724146}"
    assert old_hash in text
    return text.replace(old_hash, f"u4 %Hash{{{content_hash(text)}}}")


# Project ------------------------------------------------------------------------------------------------------------

def strip_surface(skeleton: Path) -> None:
    """The skeleton names a surface asset of the project it came from. This project has none, so the reference goes."""
    import re

    text = skeleton.read_text(encoding="utf-8")

    for surface in set(re.findall(r's %Surface\{"(\{ [0-9a-f-]+ \})"\}', text)):
        text = text.replace(f's %Surface{{"{surface}"}}', 's %Surface{""}')
        text = re.sub(r'[ \t]*s\{"' + re.escape(surface) + r'"\}\n', "", text)

    skeleton.write_text(text, encoding="utf-8", newline="\n")


def install_package(package: Path, project: Path, configuration: str) -> str:
    """Puts the package's build into the project's package store, the way an installed package lies there."""
    import re

    manifest = (package / "Package.plPackage").read_text(encoding="utf-8")
    version = re.search(r'string %Version\{"([^"]+)"\}', manifest).group(1)

    store = project / "Plugins" / "plasma.jolt" / version
    if store.exists():
        shutil.rmtree(store)

    store.mkdir(parents=True)

    for name in ("Package.plPackage", "Jolt.plPluginBundle"):
        shutil.copyfile(package / name, store / name)

    for folder in ("Assets", "EditorIcons", "Branding"):
        if (package / folder).exists():
            shutil.copytree(package / folder, store / folder)

    binaries = package / "Bin" / f"Windows_x64_{configuration}"
    target = store / "Bin" / f"Windows_x64_{configuration}"
    target.mkdir(parents=True)

    for file in binaries.iterdir():
        if file.is_file() and file.name.startswith("pl") and file.suffix in (".dll", ".pdb"):
            shutil.copyfile(file, target / file.name)

    write(store / "PackageSource.txt", str(package).replace("\\", "/"))
    return version


def main() -> None:
    sdk = Path(sys.argv[1])
    project = Path(sys.argv[2])
    mannequin = Path(sys.argv[3])
    package = Path(sys.argv[4])

    configuration = "Development"
    if "--configuration" in sys.argv:
        configuration = sys.argv[sys.argv.index("--configuration") + 1]

    basics = sdk / "Data/UnitTests/GameEngineTest/Basics"

    write(project / "plProject", "ezEditor Project File")
    write(project / "DataDir.plManifest", "")

    write(project / "RuntimeConfigs/DataDirectories.ddl", """DataDir
{
	string %Path{">sdk/Data/Base"}
	string %RootName{"base"}
	bool %Writable{false}
}
DataDir
{
	string %Path{">project/"}
	string %RootName{"project"}
	bool %Writable{true}
}
""")

    version = install_package(package, project, configuration)

    write(project / "RuntimeConfigs/Plugins.ddl", """Plugin
{
	string %Path{"plJoltPlugin"}
	bool %LoadCopy{false}
}
""")
    write(project / "RuntimeConfigs/Tags.ddl", "")
    write(project / "Editor/Packages.ddl", f"""Package
{{
	string %Id{{"plasma.jolt"}}
	string %Version{{"{version}"}}
}}
""")
    write(project / "Editor/PluginSelection.ddl", """PluginState
{
	string %ID{"Jolt"}
	bool %Selected{true}
	bool %LoadCopy{false}
}
""")

    (project / "Editor").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(basics / "Editor/AssetProfiles.ddl", project / "Editor/AssetProfiles.ddl")
    shutil.copyfile(basics / "RuntimeConfigs/Default.plProfile", project / "RuntimeConfigs/Default.plProfile")

    # The mannequin: its source file, skeleton with the body shapes, materials and clips, at the paths its assets name.
    quaternius = project / "Characters/Quaternius"
    quaternius.mkdir(parents=True, exist_ok=True)

    for name in ("UAL1_Standard.glb", "UAL1_Standard.plSkeletonAsset", "UAL1_Standard.plAnimatedMeshAsset", "UAL1_Jog.plAnimationClipAsset", "UAL1_Idle.plAnimationClipAsset", "UAL-License.txt", "PROVENANCE.txt"):
        if (mannequin / name).exists():
            shutil.copyfile(mannequin / name, quaternius / name)

    if (quaternius / "UAL1_Standard_data").exists():
        shutil.rmtree(quaternius / "UAL1_Standard_data")
    shutil.copytree(mannequin / "UAL1_Standard_data", quaternius / "UAL1_Standard_data")

    strip_surface(quaternius / "UAL1_Standard.plSkeletonAsset")

    skeleton = document_guid(quaternius / "UAL1_Standard.plSkeletonAsset")
    jog = document_guid(quaternius / "UAL1_Jog.plAnimationClipAsset")
    idle = document_guid(quaternius / "UAL1_Idle.plAnimationClipAsset")
    main_material = document_guid(quaternius / "UAL1_Standard_data/M_Main.plMaterialAsset")
    joints_material = document_guid(quaternius / "UAL1_Standard_data/M_Joints.plMaterialAsset")

    cape = add_cape(quaternius / "UAL1_Standard.glb", project / "Characters/Mannequin/Mannequin_Cape.glb")
    assert cape["materials"] == ["M_Main", "M_Joints", "M_Cape"], cape["materials"]

    write(project / "Characters/Mannequin/M_Cape.plMaterialAsset", cape_material(quaternius / "UAL1_Standard_data/M_Main.plMaterialAsset"))
    write(project / "Characters/Mannequin/Mannequin_Cape.plAnimatedMeshAsset", animated_mesh_asset(skeleton, [("M_Main", main_material), ("M_Joints", joints_material), ("M_Cape", CAPE_MATERIAL_GUID)]))
    write(project / "Characters/Mannequin/Mannequin_Cape.plJoltClothAsset", cloth_asset(jog))

    write(project / "Scenes/ClothTest.plScene", build_scene(sdk, jog, idle, SCENE_GUID, wind_strength=None))
    write(project / "Scenes/ClothWind.plScene", build_scene(sdk, jog, idle, uuid.UUID("c107c000-434c-4f54-8000-000000000021"), wind_strength="StrongBreeze"))
    write(project / "Scenes/ClothSkinned.plScene", build_scene(sdk, jog, idle, uuid.UUID("c107c000-434c-4f54-8000-000000000022"), wind_strength=None, simulate=False))

    print(f"ClothLab written to {project}: cape with {cape['vertices']} vertices and {cape['triangles']} triangles, plasma.jolt {version} installed")


if __name__ == "__main__":
    main()
