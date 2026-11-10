"""Generates the ClothLab test project: mannequins that wear a generated cape with character cloth on it.

Usage: python gen_clothlab.py <sdk root> <project dir> <mannequin dir> <package dir> [--configuration Development]
                              [--platform Windows|Mac|Linux] [--abi-tag <tag>]

  sdk root       the engine checkout the project runs with
  project dir    where the project is written; every file is written fresh
  mannequin dir  a folder with the CC0 Quaternius mannequin: UAL1_Standard.glb, its skeleton asset (with shapes),
                 its material assets and the UAL1_Jog / UAL1_Idle clip assets
  package dir    the plasma.jolt checkout whose build is installed into the project
  --platform     the platform folder of the package build, Bin/<platform>_x64_<configuration>; the host's by default
  --abi-tag      the ABI tag of the engine build the package was built against, written into the installed manifest.
                 Needed when the compiler here is not the one the manifest names; the engine prints its tag when it
                 refuses the package.

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
FLOOR_MATERIAL_GUID = uuid.UUID("c107c000-434c-4f54-8000-000000000004")
MIRROR_MATERIAL_GUID = uuid.UUID("c107c000-434c-4f54-8000-000000000005")
SCENE_GUID = uuid.UUID("c107c000-434c-4f54-8000-000000000020")

TEMPLATE_SCENE_ID = "u4{14841776606954812814,5457590759842316574}"

# cape grid
CAPE_COLUMNS = 15
CAPE_ROWS = 26

# The camera rig looks along +X. The mannequins stand around the origin.
CAMERA_POSITION = (-3.4, 0.0, 1.35)
CAMERA_PITCH_DEGREES = 6.0

# The template's sun is in physical units and stands low; this exposes the lab's characters well in its light.
EXPOSURE_EV100 = 11.9


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


def add_cape(source: Path, target: Path, name: str = "Cape", hang: float = 1.02, train: float = 0.0, rows: int = CAPE_ROWS) -> dict:
    """Writes a copy of the mannequin glb with a cape. glTF space: Y up, the mannequin looks along +Z.

    hang: metres the cape hangs down from the collar. train: metres it then lies on the floor behind the character.
    """
    CAPE_ROWS = rows
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
        # how far down the cape the row is, in metres, and as a share of the part that hangs
        along = r / (CAPE_ROWS - 1) * (hang + train)
        v = min(along / hang, 1.0)
        y = 1.47 - v * hang
        half_width = 0.19 + 0.13 * v

        for c in range(CAPE_COLUMNS):
            index = r * CAPE_COLUMNS + c
            u = c / (CAPE_COLUMNS - 1)
            side = (u - 0.5) * 2.0

            x = side * half_width

            # behind the back, drifting away from it towards the hem; the collar's ends wrap forward over the shoulders
            z = -0.15 - 0.05 * v + 0.10 * side * side * (1.0 - v) ** 2

            # a train goes on along the floor, away from the heels
            z -= max(along - hang, 0.0)

            body_back = back_of_body(x, y)
            if body_back is not None:
                z = min(z, body_back - 0.015)

            positions[index] = (x, y, z)
            uvs[index] = (u, r / (CAPE_ROWS - 1))

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
            down = r / (CAPE_ROWS - 1)
            free = 0.0 if down < 0.05 else ((down - 0.05) / 0.95) ** 0.7
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

    gltf["materials"].append({"name": f"M_{name}", "doubleSided": True, "pbrMetallicRoughness": {"baseColorFactor": [0.55, 0.08, 0.08, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.85}})
    gltf["meshes"].append({"name": f"CLOTH_{name}", "primitives": [{"attributes": attributes, "indices": index_accessor, "material": len(gltf["materials"]) - 1}]})

    # next to the mannequin's own mesh node
    body_node = next(i for i, node in enumerate(gltf["nodes"]) if "mesh" in node)
    gltf["nodes"].append({"name": f"CLOTH_{name}", "mesh": len(gltf["meshes"]) - 1, "skin": 0})

    parent = next((node for node in gltf["nodes"] if body_node in node.get("children", [])), None)
    if parent is not None:
        parent["children"].append(len(gltf["nodes"]) - 1)
    else:
        gltf["scenes"][0]["nodes"].append(len(gltf["nodes"]) - 1)

    glb.save(target)

    return {"vertices": len(positions), "triangles": len(triangles), "materials": [m["name"] for m in gltf["materials"]]}


# Coat, skirt and scarf -----------------------------------------------------------------------------------------------

def build_grid(rows: int, columns: int, sample, joint: dict) -> dict:
    """A sheet of rows x columns vertices. sample(u, v) returns (position, [(joint name, weight)], red).

    A closed garment puts its last column on its first: the seam a UV layout needs, which the cloth cook welds.
    """
    count = rows * columns
    grid = {
        "positions": np.zeros((count, 3), dtype="<f4"),
        "uvs": np.zeros((count, 2), dtype="<f4"),
        "joints": np.zeros((count, 4), dtype="uint8"),
        "weights": np.zeros((count, 4), dtype="<f4"),
        "colors": np.zeros((count, 4), dtype="<f4"),
    }

    for r in range(rows):
        v = r / (rows - 1)

        for c in range(columns):
            index = r * columns + c
            u = c / (columns - 1)

            position, influences, red = sample(u, v)
            grid["positions"][index] = position
            grid["uvs"][index] = (u, v)
            grid["colors"][index] = (red, 0.0, 0.0, 1.0)

            influences = sorted((i for i in influences if i[1] > 0.0), key=lambda i: -i[1])[:4]
            total = sum(weight for _, weight in influences)
            for slot, (name, weight) in enumerate(influences):
                grid["joints"][index, slot] = joint[name]
                grid["weights"][index, slot] = weight / total

    indices = []
    for r in range(rows - 1):
        for c in range(columns - 1):
            i0 = r * columns + c
            i1, i2, i3 = i0 + 1, i0 + columns, i0 + columns + 1
            indices += [i0, i1, i2, i1, i3, i2]

    grid["indices"] = np.array(indices, dtype="<u2")
    return grid


def merge_grids(first: dict, second: dict) -> dict:
    merged = {key: np.concatenate([first[key], second[key]]) for key in ("positions", "uvs", "joints", "weights", "colors")}
    merged["indices"] = np.concatenate([first["indices"], second["indices"] + len(first["positions"])]).astype("<u2")
    return merged


def add_grid_mesh(glb: Glb, name: str, grid: dict, material: int) -> None:
    """Adds a skinned mesh and its node to the glb. Triangles are wound so that their front faces away from the body."""
    gltf = glb.json
    positions = grid["positions"]
    triangles = grid["indices"].reshape(-1, 3)

    face_normals = np.cross(positions[triangles[:, 1]] - positions[triangles[:, 0]], positions[triangles[:, 2]] - positions[triangles[:, 0]])
    centres = positions[triangles].mean(axis=1)
    away = centres * (1.0, 0.0, 1.0)

    if float((face_normals * away).sum()) < 0.0:
        triangles = triangles[:, [0, 2, 1]]
        face_normals = -face_normals

    normals = np.zeros_like(positions, dtype=np.float64)
    for corner in range(3):
        np.add.at(normals, triangles[:, corner], face_normals)
    normals = (normals / np.linalg.norm(normals, axis=1, keepdims=True)).astype("<f4")

    attributes = {
        "POSITION": glb.add_accessor(positions, "VEC3", 34962, with_bounds=True),
        "NORMAL": glb.add_accessor(normals, "VEC3", 34962),
        "TEXCOORD_0": glb.add_accessor(grid["uvs"], "VEC2", 34962),
        "JOINTS_0": glb.add_accessor(grid["joints"], "VEC4", 34962),
        "WEIGHTS_0": glb.add_accessor(grid["weights"], "VEC4", 34962),
        "COLOR_0": glb.add_accessor(grid["colors"], "VEC4", 34962),
    }
    index_accessor = glb.add_accessor(np.ascontiguousarray(triangles).astype("<u2").reshape(-1, 1), "SCALAR", 34963)

    gltf["meshes"].append({"name": name, "primitives": [{"attributes": attributes, "indices": index_accessor, "material": material}]})

    body_node = next(i for i, node in enumerate(gltf["nodes"]) if "mesh" in node)
    gltf["nodes"].append({"name": name, "mesh": len(gltf["meshes"]) - 1, "skin": 0})

    parent = next((node for node in gltf["nodes"] if body_node in node.get("children", [])), None)
    if parent is not None:
        parent["children"].append(len(gltf["nodes"]) - 1)
    else:
        gltf["scenes"][0]["nodes"].append(len(gltf["nodes"]) - 1)


def add_garment(source: Path, target: Path, garment: str, colour) -> dict:
    """Writes a copy of the mannequin glb with a coat, a skirt or a scarf. glTF space: Y up, the mannequin looks along +Z."""
    glb = Glb(source)
    gltf = glb.json

    skin = gltf["skins"][0]
    joint_names = [gltf["nodes"][n].get("name") for n in skin["joints"]]
    joint = {name: joint_names.index(name) for name in ("pelvis", "spine_01", "spine_02", "spine_03", "neck_01", "thigh_l", "thigh_r")}

    inverse_bind = glb.accessor(skin["inverseBindMatrices"]).reshape(-1, 4, 4).transpose(0, 2, 1)
    joint_height = {name: float(np.linalg.inv(inverse_bind[index].astype(np.float64))[1, 3]) for name, index in joint.items()}

    body = np.concatenate([glb.accessor(p["attributes"]["POSITION"]) for mesh in gltf["meshes"] for p in mesh["primitives"]])

    def front_of_body(x: float, y: float):
        # the mannequin has few vertices: a narrow search finds only the ones on its back
        near = body[(np.abs(body[:, 0] - x) < 0.09) & (np.abs(body[:, 1] - y) < 0.09) & (body[:, 2] > 0.0)]
        return float(near[:, 2].max()) if len(near) else None

    chain = ["neck_01", "spine_03", "spine_02", "spine_01", "pelvis"]

    def torso(y: float):
        """Skinned to the torso joint at this height, blended with the next one down."""
        for upper, lower in zip(chain, chain[1:]):
            top, bottom = joint_height[upper], joint_height[lower]
            if y >= top:
                return [(upper, 1.0)]
            if y >= bottom:
                t = (top - y) / (top - bottom)
                return [(upper, 1.0 - t), (lower, t)]

        return [("pelvis", 1.0)]

    def hips(x: float, y: float, share: float):
        """Below the hip joints a garment follows the pelvis and, by the given share at most, the thigh on its side."""
        legs = share * min(max((joint_height["thigh_l"] - y) / 0.35, 0.0), 1.0)
        left = min(max(0.5 + x / 0.12, 0.0), 1.0)
        return [("pelvis", 1.0 - legs), ("thigh_l", legs * left), ("thigh_r", legs * (1.0 - left))]

    def skirt(u: float, v: float):
        # wide enough at the waist to clear the skeleton's hip shapes, which are coarser than the body
        angle = 2.0 * math.pi * u
        y = 1.03 - 0.50 * v
        x = (0.205 + 0.10 * v) * math.sin(angle)
        z = -0.005 + (0.175 + 0.11 * v) * math.cos(angle)

        # the waistband is pinned, then the skirt may move further the lower it hangs
        free = 0.0 if v < 0.08 else ((v - 0.08) / 0.92) ** 0.7
        return (x, y, z), hips(x, y, 0.6), 1.0 - free

    def coat(u: float, v: float):
        # open at the front: from 20 degrees left of the front, around the back, to 20 degrees right of it
        angle = math.radians(20.0 + 320.0 * u)
        y = 1.36 - 0.86 * v
        flare = min(max((1.05 - y) / 0.55, 0.0), 1.0)
        x = (0.19 + 0.10 * flare) * math.sin(angle)
        z = -0.01 + (0.165 + 0.10 * flare) * math.cos(angle)

        # A coat hangs from the hips. Skinned to the thighs for more than a little, its two sides are pulled apart
        # with every stride, which the stress test reports as stretch: the cloth is what follows the legs.
        influences = torso(y) if y >= joint_height["thigh_l"] else hips(x, y, 0.15)

        # pinned on the chest and the back, free below the waist
        free = 0.0 if v < 0.33 else ((v - 0.33) / 0.67) ** 0.8
        return (x, y, z), influences, 1.0 - free

    def scarf_ring(u: float, v: float):
        angle = 2.0 * math.pi * u
        y = 1.53 - 0.07 * v
        return (0.085 * math.sin(angle), y, -0.01 + 0.085 * math.cos(angle)), [("neck_01", 1.0)], 1.0

    def scarf_tail(u: float, v: float):
        x = 0.01 + 0.12 * u
        y = 1.50 - 0.48 * v
        # off the chest far enough to clear the skeleton's chest shape, which is coarser than the body
        front = front_of_body(x, y)
        z = (front if front is not None else 0.09) + 0.03

        free = 0.0 if v < 0.1 else ((v - 0.1) / 0.9) ** 0.7
        return (x, y, z), torso(y), 1.0 - free

    gltf["materials"].append({"name": f"M_{garment}", "doubleSided": True, "pbrMetallicRoughness": {"baseColorFactor": [*colour, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.85}})
    material = len(gltf["materials"]) - 1

    if garment == "Skirt":
        grid = build_grid(16, 33, skirt, joint)
        add_grid_mesh(glb, "CLOTH_Skirt", grid, material)
    elif garment == "Coat":
        # the drawn coat is finer than what is simulated: a proxy with a fifth of its vertices carries it
        grid = build_grid(31, 41, coat, joint)
        add_grid_mesh(glb, "CLOTH_Coat", grid, material)
        add_grid_mesh(glb, "CLOTHPROXY_Coat", build_grid(14, 17, coat, joint), material)
    elif garment == "Scarf":
        grid = merge_grids(build_grid(3, 13, scarf_ring, joint), build_grid(17, 5, scarf_tail, joint))
        add_grid_mesh(glb, "CLOTH_Scarf", grid, material)
    else:
        raise ValueError(garment)

    glb.save(target)

    return {"vertices": len(grid["positions"]), "triangles": len(grid["indices"]) // 3, "materials": [m["name"] for m in gltf["materials"]]}


# Bone chains --------------------------------------------------------------------------------------------------------

# name, the joint it hangs from, where its joints are (glTF space: Y up, the mannequin looks along +Z), tube radius
CHAINS = (
    ("pony", "Head", ((0.0, 1.70, -0.10), (0.0, 1.64, -0.17), (0.0, 1.55, -0.21), (0.0, 1.45, -0.23), (0.0, 1.35, -0.24)), 0.035),
    ("pouch", "pelvis", ((0.20, 1.00, 0.03), (0.21, 0.90, 0.03), (0.21, 0.80, 0.03)), 0.055),
    ("tail", "pelvis", ((0.0, 0.96, -0.12), (0.0, 0.86, -0.22), (0.0, 0.73, -0.30), (0.0, 0.59, -0.36), (0.0, 0.45, -0.40), (0.0, 0.31, -0.42)), 0.03),
)


def add_chains(source: Path, target: Path) -> dict:
    """Writes a copy of the mannequin glb with a ponytail, a pouch and a tail: joints of their own with a tube skinned
    to them. They are what the Bone Chains component swings."""
    glb = Glb(source)
    gltf = glb.json

    skin = gltf["skins"][0]
    joint_names = [gltf["nodes"][n].get("name") for n in skin["joints"]]

    # glTF stores matrices by column; numpy reads them by row
    inverse_bind = glb.accessor(skin["inverseBindMatrices"]).reshape(-1, 4, 4).transpose(0, 2, 1).astype(np.float64)
    inverse_bind = [m for m in inverse_bind]

    positions, normals, uvs, joints, weights, indices = [], [], [], [], [], []
    sides = 8

    for name, parent, points, radius in CHAINS:
        parent_index = joint_names.index(parent)
        parent_node = skin["joints"][parent_index]
        parent_world = np.linalg.inv(inverse_bind[parent_index])

        first_vertex = len(positions)

        for number, point in enumerate(points):
            # a joint that looks the way the character stands; only where it is matters to the chain
            world = np.eye(4)
            world[:3, 3] = point
            local = np.linalg.inv(parent_world) @ world

            gltf["nodes"].append({"name": f"{name}_{number + 1:02d}", "matrix": [float(v) for v in local.T.reshape(-1)]})
            node = len(gltf["nodes"]) - 1
            gltf["nodes"][parent_node].setdefault("children", []).append(node)

            skin["joints"].append(node)
            joint_names.append(f"{name}_{number + 1:02d}")
            inverse_bind.append(np.linalg.inv(world))
            joint = len(skin["joints"]) - 1

            parent_node, parent_world = node, world

            # a ring of the tube around the joint, thinner towards the tip
            along = np.array(points[min(number + 1, len(points) - 1)]) - np.array(points[max(number - 1, 0)])
            along /= np.linalg.norm(along)
            side = np.cross(along, (1.0, 0.0, 0.0))
            side /= np.linalg.norm(side)
            up = np.cross(side, along)

            ring_radius = radius * (1.0 - 0.5 * number / (len(points) - 1))
            for s in range(sides):
                angle = 2.0 * math.pi * s / sides
                outward = math.cos(angle) * side + math.sin(angle) * up

                positions.append(np.array(point) + outward * ring_radius)
                normals.append(outward)
                uvs.append((s / sides, number / (len(points) - 1)))
                joints.append((joint, 0, 0, 0))
                weights.append((1.0, 0.0, 0.0, 0.0))

        for ring in range(len(points) - 1):
            for s in range(sides):
                a = first_vertex + ring * sides + s
                b = first_vertex + ring * sides + (s + 1) % sides
                indices += [a, b, a + sides, b, b + sides, a + sides]

    # the inverse bind matrices of all joints, old and new
    matrices = np.array([m.T.reshape(-1) for m in inverse_bind], dtype="<f4")
    skin["inverseBindMatrices"] = glb.add_accessor(matrices, "MAT4", 34962)
    del gltf["bufferViews"][gltf["accessors"][skin["inverseBindMatrices"]]["bufferView"]]["target"]

    attributes = {
        "POSITION": glb.add_accessor(np.array(positions, dtype="<f4"), "VEC3", 34962, with_bounds=True),
        "NORMAL": glb.add_accessor(np.array(normals, dtype="<f4"), "VEC3", 34962),
        "TEXCOORD_0": glb.add_accessor(np.array(uvs, dtype="<f4"), "VEC2", 34962),
        "JOINTS_0": glb.add_accessor(np.array(joints, dtype="uint8"), "VEC4", 34962),
        "WEIGHTS_0": glb.add_accessor(np.array(weights, dtype="<f4"), "VEC4", 34962),
    }
    index_accessor = glb.add_accessor(np.array(indices, dtype="<u2").reshape(-1, 1), "SCALAR", 34963)

    gltf["materials"].append({"name": "M_Chains", "doubleSided": True, "pbrMetallicRoughness": {"baseColorFactor": [0.12, 0.10, 0.09, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.8}})
    gltf["meshes"].append({"name": "Chains", "primitives": [{"attributes": attributes, "indices": index_accessor, "material": len(gltf["materials"]) - 1}]})

    body_node = next(i for i, node in enumerate(gltf["nodes"]) if "mesh" in node)
    gltf["nodes"].append({"name": "Chains", "mesh": len(gltf["meshes"]) - 1, "skin": 0})

    parent = next((node for node in gltf["nodes"] if body_node in node.get("children", [])), None)
    if parent is not None:
        parent["children"].append(len(gltf["nodes"]) - 1)
    else:
        gltf["scenes"][0]["nodes"].append(len(gltf["nodes"]) - 1)

    glb.save(target)

    return {"vertices": len(positions), "joints": len(skin["joints"]), "materials": [m["name"] for m in gltf["materials"]]}


def skeleton_asset(template: Path, guid: uuid.UUID, mesh_file: str) -> str:
    """The mannequin's skeleton asset for another source file. A transform adds the joints the file has on top."""
    import re

    text = template.read_text(encoding="utf-8")

    old_id = re.search(r"Uuid %DocumentID\{(u4\{\d+,\d+\})\}", text).group(1)
    old_file = re.search(r's %File\{"([^"]+)"\}', text).group(1)

    text = text.replace(old_id, u4(guid)).replace(old_file, mesh_file)
    return re.sub(r"u4 %Hash\{\d+\}", f"u4 %Hash{{{content_hash(text)}}}", text, count=1)


def bone_chain_component(comp_id: str, chains) -> str:
    chain_list = "".join(f"\t\t\ts{{\"{c}\"}}\n" for c in chains)
    return component(comp_id, "plJoltBoneChainComponent", 1, f"""		VarArray %Chains
		{{
{chain_list}		}}
		b %CollideWithBody{{1}}
		f %Damping{{{f32(0.15)}}}
		f %GravityFactor{{{f32(1.0)}}}
		f %LinearInertia{{{f32(0.8)}}}
		Angle %MaxAngle{{f{{{f32(math.radians(60.0))}}}}}
		f %Radius{{{f32(0.03)}}}
		f %Stiffness{{{f32(0.2)}}}
		f %TeleportDistance{{{f32(2.0)}}}
		f %WindInfluence{{{f32(1.0)}}}
""")


# Assets -------------------------------------------------------------------------------------------------------------

def animated_mesh_asset(skeleton: uuid.UUID, materials, mesh_guid: uuid.UUID = MESH_GUID, garment: str = "Cape") -> str:
    """materials: list of (label, guid) in the order of the glb's materials."""
    seed = "mesh" if garment == "Cape" else "mesh/" + garment
    mesh_file = f"Characters/Mannequin/Mannequin_{garment}.glb"

    slots = ""
    slot_ids = []
    for label, guid in materials:
        slot_id = new_id(seed + "/slot/" + label)
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

    props_id = new_id(seed + "/props")
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
		s %MeshFile{{"{mesh_file}"}}
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
    return asset_header("Animated Mesh", mesh_guid, body, references=references, dependencies=[mesh_file]) + body


def cloth_asset(preview_clip: uuid.UUID, cloth_guid: uuid.UUID = CLOTH_GUID, mesh_guid: uuid.UUID = MESH_GUID, garment: str = "Cape", max_distance: float = 0.45, preset: str = "Cotton", world_collision: str = "None", joints=(), collider_inflate: float = 0.01, iterations: int = 8, stress_clips=()) -> str:
    """joints: the joints whose skeleton shapes the garment collides with; every shape in reach without it.
    collider_inflate: metres the body shapes grow by for this garment. The stress test says when it is too little."""
    joint_list = "{}" if not joints else "\n\t\t{\n" + "".join(f"\t\t\ts{{\"{j}\"}}\n" for j in joints) + "\t\t}"
    stress_list = "{}" if not stress_clips else "\n\t\t{\n" + "".join(f"\t\t\ts{{\"{guid_ref(c)}\"}}\n" for c in stress_clips) + "\t\t}"
    props_id = new_id("cloth/props" if garment == "Cape" else f"cloth/{garment}/props")

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
		f %ColliderInflate{{{f32(collider_inflate)}}}
		s %ColliderSource{{"plJoltClothColliderSource::SkeletonShapes"}}
		VarArray %Colliders{{}}
		f %Damping{{{f32(0.2)}}}
		f %Density{{{f32(0.15)}}}
		f %Drag{{{f32(0.7)}}}
		s %FabricPreset{{"plJoltClothFabricPreset::{preset}"}}
		f %Friction{{{f32(0.2)}}}
		f %FullRateDistance{{{f32(14.0)}}}
		VarArray %Garments{{}}
		f %GravityFactor{{{f32(1.0)}}}
		u1 %Iterations{{{iterations}}}
		f %Lift{{{f32(0.2)}}}
		f %MaxDistance{{{f32(max_distance)}}}
		s %Mesh{{"{guid_ref(mesh_guid)}"}}
		s %PreviewClip{{"{guid_ref(preview_clip)}"}}
		f %Shear{{{f32(0.9)}}}
		f %SimulateDistance{{{f32(30.0)}}}
		VarArray %SkeletonShapeJoints{joint_list}
		f %StepRate{{{f32(60.0)}}}
		VarArray %StressClips{stress_list}
		f %Stretch{{{f32(0.95)}}}
		f %StretchLimit{{{f32(0.08)}}}
		f %TetherSlack{{{f32(1.03)}}}
		b %Tethers{{1}}
		f %Thickness{{{f32(0.01)}}}
		b %UseImportedMasks{{1}}
		f %WeldDistance{{{f32(0.0001)}}}
		s %WorldCollision{{"plJoltClothWorldCollision::{world_collision}"}}
	}}
}}
{DOCUMENT_ROOT.format(child=props_id)}}}
Types
{{
}}
"""
    return asset_header("Jolt_Cloth", cloth_guid, body, references=[guid_ref(mesh_guid)], dependencies=[guid_ref(mesh_guid)]) + body


def cape_material(template: Path, material_guid: uuid.UUID = CAPE_MATERIAL_GUID, colour=(0.45, 0.04, 0.05), roughness=None, metallic=None) -> str:
    """The mannequin's main material with another id and colour. It is two-sided already. roughness and metallic
    replace the template's values, for a floor that mirrors what stands on it."""
    import re

    text = template.read_text(encoding="utf-8")

    old_id = re.search(r"Uuid %DocumentID\{(u4\{\d+,\d+\})\}", text).group(1)
    text = text.replace(old_id, u4(material_guid))

    text, count = re.subn(r"Color %BaseColor\{f\{[^}]*\}\}", "Color %BaseColor{" + vec((*colour, 1.0)) + "}", text, count=1)
    assert count == 1, "the material template has no BaseColor"

    for name, value in (("RoughnessValue", roughness), ("MetallicValue", metallic)):
        if value is not None:
            text, count = re.subn(r"f %" + name + r"\{[^}]*\}", f"f %{name}{{{f32(value)}}}", text, count=1)
            assert count == 1, f"the material template has no {name}"

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


def character(name: str, position, yaw_degrees: float, clip: uuid.UUID, simulate=True, mesh_guid: uuid.UUID = MESH_GUID, cloth_guid: uuid.UUID = CLOTH_GUID, importance: str = "Always", teleport_mode: str = "Reset", world_collision: str = "Asset", extra_components=(), seed=None):
    """cloth_guid None leaves the Character Cloth component out. extra_components: (id, text) of further components.
    seed: what the ids are made from, for two objects of the same name."""
    half = math.radians(yaw_degrees) * 0.5
    rotation = (0.0, 0.0, math.sin(half), math.cos(half))

    seed = seed or name
    root_id = new_id(seed)
    mesh_id = new_id(seed + "/mesh")
    animation_id = new_id(seed + "/animation")
    cloth_id = new_id(seed + "/cloth")

    component_ids = [mesh_id, animation_id] + ([cloth_id] if cloth_guid is not None else []) + [c[0] for c in extra_components]
    text = game_object(root_id, name, position, rotation, components=component_ids)
    text += "".join(c[1] for c in extra_components)

    text += component(mesh_id, "plAnimatedMeshComponent", 13, f"""		Color %Color{{f{{0x0000803F,0x0000803F,0x0000803F,0x0000803F}}}}
		VarArray %Materials{{}}
		s %Mesh{{"{guid_ref(mesh_guid)}"}}
""")

    text += component(animation_id, "plSimpleAnimationComponent", 3, f"""		s %AnimationClip{{"{guid_ref(clip)}"}}
		s %AnimationMode{{"plPropertyAnimMode::Loop"}}
		b %EnableIK{{0}}
		s %InvisibleUpdateRate{{"plAnimationInvisibleUpdateRate::FullUpdate"}}
		s %RootMotionMode{{"plRootMotionMode::Ignore"}}
		f %Speed{{0x0000803F}}
""")

    if cloth_guid is None:
        return text, root_id

    text += component(cloth_id, "plJoltCharacterClothComponent", 1, f"""		f %AngularInertia{{{f32(0.7)}}}
		s %Cloth{{"{guid_ref(cloth_guid)}"}}
		u1 %CollisionLayer{{0}}
		f %GravityFactor{{{f32(1.0)}}}
		s %Importance{{"plJoltClothImportance::{importance}"}}
		f %LinearInertia{{{f32(0.7)}}}
		f %MaxDistanceScale{{{f32(1.0)}}}
		b %Simulate{{{1 if simulate else 0}}}
		f %TeleportDistance{{{f32(2.0)}}}
		s %TeleportMode{{"plJoltClothTeleportMode::{teleport_mode}"}}
		f %WindInfluence{{{f32(1.0)}}}
		s %WorldCollision{{"plJoltClothWorldCollisionMode::{world_collision}"}}
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


TICKS_PER_SECOND = 4800


def f64(value: float) -> str:
    return "0x" + struct.pack("<d", value).hex().upper()


def time_value(seconds: float) -> str:
    return "d{" + (f64(seconds) if seconds != 0.0 else "0") + "}"


def property_animation(guid: uuid.UUID, seed: str, duration: float, tracks) -> str:
    """A property animation asset: what scripts a scene without a script.

    tracks: (object path, component type, property, target, [(seconds, value), ...]). The object path names a child
    of the object with the Property Animation component, '' is that object itself. The component type is '' for the
    object's own position and rotation. Keys are joined by straight lines; two keys a tick apart make a jump.
    """
    objects = ""
    track_ids = []

    for number, (object_path, component_type, prop, target, keys) in enumerate(tracks):
        point_ids = []

        for key, (seconds, value) in enumerate(keys):
            point_id = new_id(f"{seed}/track{number}/key{key}")
            point_ids.append(point_id)
            objects += f"""o
{{
	Uuid %id{{{point_id}}}
	s %t{{"plCurveControlPointData"}}
	u3 %v{{5}}
	p
	{{
		Vec2 %LeftTangent{{{vec((-0.1, 0.0))}}}
		s %LeftTangentMode{{"plCurveTangentMode::Linear"}}
		b %Linked{{0}}
		Vec2 %RightTangent{{{vec((0.1, 0.0))}}}
		s %RightTangentMode{{"plCurveTangentMode::Linear"}}
		i4 %Tick{{{int(round(seconds * TICKS_PER_SECOND))}}}
		d %Value{{{f64(float(value)) if value != 0.0 else "0"}}}
	}}
}}
"""

        curve_id = new_id(f"{seed}/track{number}/curve")
        points = "".join(f"\t\t\tUuid{{{p}}}\n" for p in point_ids)
        objects += f"""o
{{
	Uuid %id{{{curve_id}}}
	s %t{{"plSingleCurveData"}}
	u3 %v{{3}}
	p
	{{
		VarArray %ControlPoints
		{{
{points}		}}
	}}
}}
"""

        track_id = new_id(f"{seed}/track{number}")
        track_ids.append(track_id)
        objects += f"""o
{{
	Uuid %id{{{track_id}}}
	s %t{{"plPropertyAnimationTrack"}}
	u3 %v{{1}}
	p
	{{
		s %ComponentType{{"{component_type}"}}
		Uuid %FloatCurve{{{curve_id}}}
		s %ObjectPath{{"{object_path}"}}
		s %Property{{"{prop}"}}
		s %Target{{"plPropertyAnimTarget::{target}"}}
	}}
}}
"""

    group_id = new_id(f"{seed}/group")
    track_list = "".join(f"\t\t\tUuid{{{t}}}\n" for t in track_ids)

    body = f"""Objects
{{
{objects}o
{{
	Uuid %id{{{group_id}}}
	s %t{{"plPropertyAnimationTrackGroup"}}
	u3 %v{{1}}
	p
	{{
		u4 %Duration{{{int(round(duration * TICKS_PER_SECOND))}}}
		u3 %FPS{{60}}
		VarArray %Tracks
		{{
{track_list}		}}
	}}
}}
{DOCUMENT_ROOT.format(child=group_id)}}}
Types
{{
}}
"""
    return asset_header("PropertyAnim", guid, body) + body


def property_animation_component(comp_id: str, animation: uuid.UUID, mode: str = "Once") -> str:
    return component(comp_id, "plPropertyAnimComponent", 3, f"""		s %Animation{{"{guid_ref(animation)}"}}
		s %Mode{{"plPropertyAnimMode::{mode}"}}
		b %Playing{{1}}
		Time %RandomOffset{{{time_value(0.0)}}}
		Time %RangeHigh{{{time_value(3600.0)}}}
		Time %RangeLow{{{time_value(0.0)}}}
		f %Speed{{{f32(1.0)}}}
""")


GREY_BOX_MATERIAL = "{ 6c1be2e8-0926-447e-ae8c-b5216f6ea9f5 }"


def grey_box(name: str, position, size, shape: str = "Box", detail: int = 16, yaw_degrees: float = 0.0, children=(), components=(), material=None):
    """A block of level geometry with collision. size: (negative x, positive x, negative y, positive y, negative z,
    positive z) from the object's position. Stairs climb along +X with 'detail' steps. Without a material it gets
    the floor's."""
    root_id = new_id(name)
    box_id = new_id(name + "/box")

    half = math.radians(yaw_degrees) * 0.5
    text = game_object(root_id, name, position, (0.0, 0.0, math.sin(half), math.cos(half)), components=(box_id, *components), children=children)
    text += component(box_id, "plGreyBoxComponent", 6, f"""		Color %Color{{f{{0x0000803F,0x0000803F,0x0000803F,0x0000803F}}}}
		Angle %Curvature{{f{{0}}}}
		Vec4 %CustomData{{f{{0,0x0000803F,0,0x0000803F}}}}
		u3 %Detail{{{detail}}}
		b %GenerateCollision{{1}}
		b %IncludeInNavmesh{{1}}
		s %Material{{"{guid_ref(material) if material is not None else GREY_BOX_MATERIAL}"}}
		s %Shape{{"plGreyBoxShape::{shape}"}}
		f %SizeNegX{{{f32(size[0])}}}
		f %SizeNegY{{{f32(size[2])}}}
		f %SizeNegZ{{{f32(size[4])}}}
		f %SizePosX{{{f32(size[1])}}}
		f %SizePosY{{{f32(size[3])}}}
		f %SizePosZ{{{f32(size[5])}}}
		b %SlopedBottom{{0}}
		b %SlopedTop{{0}}
		f %Thickness{{0x0000003F}}
		b %UseAsOccluder{{1}}
""")
    return text, root_id


def driven(name: str, position, animation: uuid.UUID, children, mode: str = "Once", yaw_degrees: float = 0.0):
    """An object that a property animation moves, and that carries the objects it is given."""
    root_id = new_id(name)
    comp_id = new_id(name + "/animation")

    half = math.radians(yaw_degrees) * 0.5
    text = game_object(root_id, name, position, (0.0, 0.0, math.sin(half), math.cos(half)), components=(comp_id,), children=children)
    text += property_animation_component(comp_id, animation, mode)
    return text, root_id


def build_scene(sdk: Path, jog: uuid.UUID, idle: uuid.UUID, scene_guid: uuid.UUID, wind_strength, simulate=True, cast=None, camera=None, extra=(), references=(), camera_animation=None, camera_yaw=0.0) -> str:
    """cast: (name, position, yaw, clip, mesh guid, cloth guid, importance) per character; the three caped mannequins
    without it. camera: (position, pitch in degrees); the lab's fixed view without it. extra: (object text, root id)
    pairs that are added as they are. references: asset guids the extra objects use. camera_animation: a property
    animation that moves the camera rig. camera_yaw: degrees the camera is turned to the left."""
    template = (sdk / "Data/Base/Editor/DocumentTemplates/Default.plScene").read_text(encoding="utf-8")
    text = template.replace(TEMPLATE_SCENE_ID, u4(scene_guid))

    # The template's sphere stands where the lab's characters do and throws its shadow over them: out of the way.
    old_sphere = "Vec3 %LocalPosition{f{0x54245B3F,0xD1DCACC0,0x00D27EBC}}"
    assert old_sphere in text, "the template's sphere was not found"
    text = text.replace(old_sphere, f"Vec3 %LocalPosition{{{vec((0.0, -60.0, -40.0))}}}")

    camera_position, camera_pitch = camera if camera is not None else (CAMERA_POSITION, CAMERA_PITCH_DEGREES)

    # turned to the left around the up axis, then tilted down: q = yaw * pitch
    pitch = math.radians(camera_pitch) * 0.5
    yaw = math.radians(camera_yaw) * 0.5
    rig_rotation = (-math.sin(yaw) * math.sin(pitch), math.cos(yaw) * math.sin(pitch), math.sin(yaw) * math.cos(pitch), math.cos(yaw) * math.cos(pitch))

    old_rig = "Vec3 %LocalPosition{f{0x2B90E6C0,0xB4998B3F,0xF4469B40}}\n\t\tQuat %LocalRotation{f{0x6E9DFA3C,0x0BD2B93D,0x36D5A2BE,0xA877713F}}"
    new_rig = f"Vec3 %LocalPosition{{{vec(camera_position)}}}\n\t\tQuat %LocalRotation{{{vec(rig_rotation)}}}"
    assert old_rig in text, "camera rig transform not found in the template"
    text = text.replace(old_rig, new_rig)

    rig_animation = ""
    if camera_animation is not None:
        # the rig has no component of its own in the template
        old_components = "\t\tVarArray %Components{}\n\t\ts %GlobalKey{\"\"}\n\t\t" + new_rig
        assert old_components in text, "the camera rig's component list was not found"

        comp_id = new_id(f"{scene_guid}/camera animation")
        text = text.replace(old_components, f"\t\tVarArray %Components\n\t\t{{\n\t\t\tUuid{{{comp_id}}}\n\t\t}}\n\t\ts %GlobalKey{{\"\"}}\n\t\t" + new_rig)
        rig_animation = property_animation_component(comp_id, camera_animation, "Once")

    # A fixed exposure: captures of the same scene then look the same, whatever the metering does.
    assert 's %ExposureMode{"plCameraExposureMode::Auto"}' in text and "f %ManualEV100{0x00004041}" in text
    text = text.replace('s %ExposureMode{"plCameraExposureMode::Auto"}', 's %ExposureMode{"plCameraExposureMode::Manual"}')
    text = text.replace("f %ManualEV100{0x00004041}", f"f %ManualEV100{{{f32(EXPOSURE_EV100)}}}")

    # Three mannequins turned three ways, so that one view shows the cape from behind, from the side and from the front.
    if cast is None:
        cast = (
            ("Jogger_Back", (0.0, -1.3, 0.0), 0.0, jog, MESH_GUID, CLOTH_GUID, "Always"),
            ("Jogger_Side", (0.0, 0.0, 0.0), 110.0, jog, MESH_GUID, CLOTH_GUID, "Always"),
            ("Idle_Front", (0.0, 1.3, 0.0), 215.0, idle, MESH_GUID, CLOTH_GUID, "Always"),
        )

    objects = rig_animation
    roots = []
    for name, position, yaw, clip, mesh_guid, cloth_guid, importance in cast:
        obj_text, root = character(name, position, yaw, clip, simulate, mesh_guid, cloth_guid, importance)
        objects += obj_text
        roots.append(root)

    if wind_strength is not None:
        obj_text, root = wind(wind_strength)
        objects += obj_text
        roots.append(root)

    # a root id of None marks objects that hang under another extra object
    for obj_text, root in extra:
        objects += obj_text
        if root is not None:
            roots.append(root)

    marker = "Objects\n{\n"
    assert marker in text
    text = text.replace(marker, marker + objects, 1)

    root_children = "\t\tVarArray %Children\n\t\t{\n\t\t\tUuid{u4{15628317284442516612,4804267580480477271}}"
    assert root_children in text, "scene root children not found in the template"
    text = text.replace(root_children, root_children + "".join(f"\n\t\t\tUuid{{{r}}}" for r in roots), 1)

    refs = [guid_ref(jog), guid_ref(idle)]
    for entry in cast:
        for guid in (entry[4], entry[5]):
            if guid is not None and guid_ref(guid) not in refs:
                refs.append(guid_ref(guid))

    for guid in (*references, camera_animation):
        if guid is not None and guid_ref(guid) not in refs:
            refs.append(guid_ref(guid))

    # The template's floor is dark earth. The lab's plain grey shows the characters and their shadows a lot better.
    # The level geometry of the lab uses the floor's material, so it changes with it.
    assert GREY_BOX_MATERIAL in text
    text = text.replace(GREY_BOX_MATERIAL, guid_ref(FLOOR_MATERIAL_GUID))
    refs.append(guid_ref(FLOOR_MATERIAL_GUID))
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


def host_platform() -> str:
    """The platform folder name of the machine the script runs on."""
    if sys.platform == "win32":
        return "Windows"

    if sys.platform == "darwin":
        return "Mac"

    return "Linux"


def install_package(package: Path, project: Path, platform: str, configuration: str, abi_tag: str | None) -> str:
    """Puts the package's build into the project's package store, the way an installed package lies there.

    With an ABI tag the installed manifest says the binaries were built for that engine build, on every platform it names.
    """
    import re

    manifest = (package / "Package.plPackage").read_text(encoding="utf-8")
    version = re.search(r'string %Version\{"([^"]+)"\}', manifest).group(1)

    store = project / "Plugins" / "plasma.jolt" / version
    if store.exists():
        shutil.rmtree(store)

    store.mkdir(parents=True)

    for name in ("Package.plPackage", "Jolt.plPluginBundle"):
        shutil.copyfile(package / name, store / name)

    if abi_tag is not None:
        stamped = re.sub(r'%AbiTag\{"[^"]*"\}', f'%AbiTag{{"{abi_tag}"}}', manifest)
        write(store / "Package.plPackage", stamped)

    for folder in ("Assets", "EditorIcons", "Branding"):
        if (package / folder).exists():
            shutil.copytree(package / folder, store / folder)

    binaries = package / "Bin" / f"{platform}_x64_{configuration}"
    target = store / "Bin" / f"{platform}_x64_{configuration}"
    target.mkdir(parents=True)

    for file in binaries.iterdir():
        if file.is_file() and file.name.startswith("pl") and file.suffix in (".dll", ".pdb", ".dylib", ".so"):
            shutil.copyfile(file, target / file.name)

    write(store / "PackageSource.txt", str(package).replace("\\", "/"))
    return version


def write_scripted_scenes(sdk: Path, project: Path, jog: uuid.UUID, idle: uuid.UUID, wardrobe: dict) -> None:
    """The scenes that move: property animations stand in for the game code that would drive a character."""
    tick = 1.0 / TICKS_PER_SECOND
    cape_mesh, cape_cloth = wardrobe["Cape"]

    def guid(number: int) -> uuid.UUID:
        return uuid.UUID(f"c107c000-434c-4f54-8000-{number:012d}")

    def asset(name: str, number: int, duration: float, tracks) -> uuid.UUID:
        write(project / f"Scenes/{name}.plPropertyAnimAsset", property_animation(guid(number), name, duration, tracks))
        return guid(number)

    # Motion ---------------------------------------------------------------------------------------------------------
    # One take, five sets in a row along X, the camera looking along +Y and cutting from set to set:
    #   0 s    a sprint at 6 m/s, a hard stop, a turn, the sprint back and another hard stop
    #   8 s    a teleport: 6 m in one frame
    #   11 s   a platform that slides and turns under a standing character
    #   15 s   a character that goes limp
    #   18.5 s a cutscene blend: Max Distance Scale fades to 0 and back
    length = 23.0

    runner = asset("ClothMotionRunner", 202, length, [
        ("", "", "LocalPosition", "VectorX", [(0.0, 0.0), (1.0, 0.0), (3.0, 12.0), (5.0, 12.0), (7.0, 0.0), (9.0, 0.0), (9.0 + tick, 6.0), (length, 6.0)]),
        ("", "", "LocalRotation", "RotationZ", [(0.0, 0.0), (4.0, 0.0), (4.6, 180.0), (length, 180.0)]),
    ])
    platform = asset("ClothMotionPlatform", 203, length, [
        ("", "", "LocalPosition", "VectorX", [(0.0, 28.0), (11.0, 28.0), (12.2, 32.0), (13.4, 28.0), (14.6, 32.0), (length, 32.0)]),
        ("", "", "LocalRotation", "RotationZ", [(0.0, 0.0), (11.0, 0.0), (15.0, 270.0), (length, 270.0)]),
    ])
    faller = asset("ClothMotionFaller", 204, length, [
        ("", "plJoltRagdollComponent", "Active", "Number", [(0.0, 0.0), (15.8, 0.0), (15.8 + tick, 1.0), (length, 1.0)]),
        ("", "plSimpleAnimationComponent", "Active", "Number", [(0.0, 1.0), (15.8, 1.0), (15.8 + tick, 0.0), (length, 0.0)]),
    ])
    blend = asset("ClothMotionBlend", 205, length, [
        ("", "plJoltCharacterClothComponent", "MaxDistanceScale", "Number", [(0.0, 1.0), (19.6, 1.0), (20.4, 0.0), (21.4, 0.0), (22.2, 1.0), (length, 1.0)]),
    ])
    camera = asset("ClothMotionCamera", 201, length, [
        ("", "", "LocalPosition", "VectorX", [(0.0, 0.0), (1.0, 0.0), (3.0, 12.0), (5.0, 12.0), (7.0, 0.0), (8.0, 0.0), (8.0 + tick, 3.0), (11.0, 3.0), (11.0 + tick, 30.0), (15.0, 30.0), (15.0 + tick, 50.0), (18.5, 50.0), (18.5 + tick, 70.0), (length, 70.0)]),
        ("", "", "LocalPosition", "VectorY", [(0.0, -5.5), (8.0, -5.5), (8.0 + tick, -8.0), (11.0, -8.0), (11.0 + tick, -7.0), (15.0, -7.0), (15.0 + tick, -4.6), (18.5, -4.6), (18.5 + tick, -3.6), (length, -3.6)]),
        ("", "", "LocalPosition", "VectorZ", [(0.0, 1.5), (11.0, 1.5), (11.0 + tick, 2.1), (15.0, 2.1), (15.0 + tick, 1.3), (length, 1.3)]),
    ])

    extra = []

    def add(pair, child=False):
        extra.append((pair[0], None if child else pair[1]))
        return pair[1]

    actor = add(character("Actor", (0.0, 0.0, 0.0), 0.0, jog, True, cape_mesh, cape_cloth, "Always", seed="motion/actor"), child=True)
    add(driven("Runner", (0.0, 0.0, 0.0), runner, children=(actor,)))

    deck = add(grey_box("Deck", (0.0, 0.0, 0.0), (1.2, 1.2, 1.2, 1.2, 0.3, 0.0)), child=True)
    rider = add(character("Rider", (0.0, 0.0, 0.0), 90.0, idle, True, cape_mesh, cape_cloth, "Always", seed="motion/rider"), child=True)
    add(driven("Platform", (28.0, 0.0, 0.3), platform, children=(deck, rider)))

    ragdoll_id = new_id("motion/faller/ragdoll")
    faller_animation_id = new_id("motion/faller/property animation")
    ragdoll = component(ragdoll_id, "plJoltRagdollComponent", 5, """		s %AnimMode{"plJoltRagdollAnimMode::Off"}
		s %StartMode{"plJoltRagdollStartMode::WithCurrentMeshPose"}
""").replace("b %Active{1}", "b %Active{0}")
    add(character("Faller", (50.0, 0.0, 0.0), 60.0, idle, True, cape_mesh, cape_cloth, "Always", seed="motion/faller",
                  extra_components=((ragdoll_id, ragdoll), (faller_animation_id, property_animation_component(faller_animation_id, faller)))))

    blend_animation_id = new_id("motion/blend/property animation")
    add(character("Blend", (70.0, 0.0, 0.0), 35.0, jog, True, cape_mesh, cape_cloth, "Always", seed="motion/blend",
                  extra_components=((blend_animation_id, property_animation_component(blend_animation_id, blend)),)))

    write(project / "Scenes/ClothMotion.plScene", build_scene(sdk, jog, idle, guid(26), wind_strength=None, cast=(), camera=((0.0, -5.5, 1.5), 4.0), camera_yaw=90.0,
                                                               camera_animation=camera, extra=extra, references=(cape_mesh, cape_cloth, runner, platform, faller, blend)))

    # Turn -----------------------------------------------------------------------------------------------------------
    # A caped jogger turning on the spot, close to the camera: what a temporal anti-aliasing ghost trail would show on.
    turn = asset("ClothTurnSpin", 207, 4.0, [
        ("", "", "LocalRotation", "RotationZ", [(0.0, 0.0), (4.0, 360.0)]),
    ])

    extra = []
    actor = add(character("Actor", (0.0, 0.0, 0.0), 0.0, jog, True, cape_mesh, cape_cloth, "Always", seed="turn/actor"), child=True)
    add(driven("Spinner", (0.0, 0.0, 0.0), turn, children=(actor,), mode="Loop"))

    # a polished plate under the character, for the cape's reflection
    add(grey_box("Mirror", (0.0, 0.0, 0.0), (1.6, 1.6, 1.6, 1.6, 0.0, 0.015), material=MIRROR_MATERIAL_GUID))

    write(project / "Scenes/ClothTurn.plScene", build_scene(sdk, jog, idle, guid(27), wind_strength=None, cast=(), camera=((-3.2, 0.0, 1.25), 9.0), extra=extra, references=(cape_mesh, cape_cloth, turn, MIRROR_MATERIAL_GUID)))

    # Stairs ---------------------------------------------------------------------------------------------------------
    # A character in a cape that reaches the floor climbs eight steps. The cape collides with the level, so it drags
    # over the steps behind it.
    if "LongCape" in wardrobe:
        long_mesh, long_cloth = wardrobe["LongCape"]

        climb = asset("ClothStairsClimb", 208, 12.0, [
            ("", "", "LocalPosition", "VectorX", [(0.0, -4.5), (1.0, -4.5), (3.0, -2.0), (8.0, 2.0), (10.0, 4.0), (12.0, 4.0)]),
            ("", "", "LocalPosition", "VectorZ", [(0.0, 0.0), (3.0, 0.0), (8.0, 1.6), (12.0, 1.6)]),
        ])

        extra = []
        actor = add(character("Actor", (0.0, 0.0, 0.0), 0.0, jog, True, long_mesh, long_cloth, "Always", world_collision="Static", seed="stairs/actor"), child=True)
        add(driven("Climber", (-4.5, 0.0, 0.0), climb, children=(actor,)))
        add(grey_box("Stairs", (0.0, 0.0, 0.0), (2.0, 2.0, 1.5, 1.5, 0.0, 1.6), shape="StairsX", detail=8))
        add(grey_box("Landing", (4.5, 0.0, 0.0), (2.5, 2.5, 1.5, 1.5, 0.0, 1.6)))

        write(project / "Scenes/ClothStairs.plScene", build_scene(sdk, jog, idle, guid(28), wind_strength=None, cast=(), camera=((0.3, -5.2, 1.9), 8.0), camera_yaw=90.0,
                                                                   extra=extra, references=(long_mesh, long_cloth, climb)))

    # Chains ---------------------------------------------------------------------------------------------------------
    # A ponytail, a pouch and a tail swinging on a jogger seen from behind, a jogger seen from the side and a
    # character that turns on the spot. Bone Chains moves their joints; no cloth is simulated here.
    if "Chains" in wardrobe:
        chains_mesh, _ = wardrobe["Chains"]
        chain_names = [f"{name}_01" for name, _, _, _ in CHAINS]

        def chained(name: str, position, yaw: float, clip: uuid.UUID, seed: str):
            comp_id = new_id(seed + "/chains")
            return character(name, position, yaw, clip, True, chains_mesh, None, seed=seed, extra_components=((comp_id, bone_chain_component(comp_id, chain_names)),))

        spin = asset("ClothChainsSpin", 209, 3.0, [
            ("", "", "LocalRotation", "RotationZ", [(0.0, 0.0), (3.0, 360.0)]),
        ])

        extra = []
        add(chained("Chains_Back", (0.0, -1.5, 0.0), 0.0, jog, "chains/back"))
        add(chained("Chains_Side", (0.0, 0.0, 0.0), 90.0, jog, "chains/side"))
        actor = add(chained("Actor", (0.0, 0.0, 0.0), 0.0, idle, "chains/turn"), child=True)
        add(driven("Chains_Turn", (0.0, 1.5, 0.0), spin, children=(actor,), mode="Loop"))

        write(project / "Scenes/ClothChains.plScene", build_scene(sdk, jog, idle, guid(30), wind_strength=None, cast=(), camera=((-3.6, 0.0, 1.35), 5.0), extra=extra, references=(chains_mesh, spin)))

    # Bare -----------------------------------------------------------------------------------------------------------
    # Two mannequins that draw the scarf mesh without a Character Cloth component: what saving the scarf's cloth asset
    # in the editor offers to fix.
    scarf_mesh, _ = wardrobe["Scarf"]
    bare = [("Bare_Jog", (0.0, -0.7, 0.0), 30.0, jog, scarf_mesh, None, "Always"), ("Bare_Idle", (0.0, 0.7, 0.0), 180.0, idle, scarf_mesh, None, "Always")]
    write(project / "Scenes/ClothBare.plScene", build_scene(sdk, jog, idle, guid(29), wind_strength=None, cast=bare))


def main() -> None:
    sdk = Path(sys.argv[1])
    project = Path(sys.argv[2])
    mannequin = Path(sys.argv[3])
    package = Path(sys.argv[4])

    configuration = "Development"
    if "--configuration" in sys.argv:
        configuration = sys.argv[sys.argv.index("--configuration") + 1]

    platform = host_platform()
    if "--platform" in sys.argv:
        platform = sys.argv[sys.argv.index("--platform") + 1]

    abi_tag = None
    if "--abi-tag" in sys.argv:
        abi_tag = sys.argv[sys.argv.index("--abi-tag") + 1]

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

    version = install_package(package, project, platform, configuration, abi_tag)

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
    # The skeleton's shapes are slimmer than the mannequin's shoulders and arms: they grow by a few centimetres for
    # the cape, so that it does not sink into the body where it wraps round a shoulder in a turn.
    write(project / "Characters/Mannequin/Mannequin_Cape.plJoltClothAsset", cloth_asset(jog, stress_clips=(idle,), collider_inflate=0.035))

    write(project / "Scenes/M_Floor.plMaterialAsset", cape_material(quaternius / "UAL1_Standard_data/M_Main.plMaterialAsset", FLOOR_MATERIAL_GUID, (0.30, 0.31, 0.33)))
    write(project / "Scenes/M_Mirror.plMaterialAsset", cape_material(quaternius / "UAL1_Standard_data/M_Main.plMaterialAsset", MIRROR_MATERIAL_GUID, (0.75, 0.77, 0.80), roughness=0.04, metallic=1.0))

    write(project / "Scenes/ClothTest.plScene", build_scene(sdk, jog, idle, SCENE_GUID, wind_strength=None))
    write(project / "Scenes/ClothWind.plScene", build_scene(sdk, jog, idle, uuid.UUID("c107c000-434c-4f54-8000-000000000021"), wind_strength="StrongBreeze"))
    write(project / "Scenes/ClothSkinned.plScene", build_scene(sdk, jog, idle, uuid.UUID("c107c000-434c-4f54-8000-000000000022"), wind_strength=None, simulate=False))

    # The sample garments: one mannequin file, animated mesh, material and cloth asset each.
    wardrobe = {"Cape": (MESH_GUID, CLOTH_GUID)}
    for number, (garment, colour, max_distance) in enumerate((("Coat", (0.10, 0.22, 0.14), 0.35), ("Skirt", (0.07, 0.16, 0.45), 0.30), ("Scarf", (0.80, 0.80, 0.74), 0.40)), start=1):
        mesh_guid = uuid.UUID(f"c107c000-434c-4f54-8000-0000000001{number}1")
        cloth_guid = uuid.UUID(f"c107c000-434c-4f54-8000-0000000001{number}2")
        material_guid = uuid.UUID(f"c107c000-434c-4f54-8000-0000000001{number}3")
        wardrobe[garment] = (mesh_guid, cloth_guid)

        made = add_garment(quaternius / "UAL1_Standard.glb", project / f"Characters/Mannequin/Mannequin_{garment}.glb", garment, colour)
        assert made["materials"] == ["M_Main", "M_Joints", f"M_{garment}"], made["materials"]

        write(project / f"Characters/Mannequin/M_{garment}.plMaterialAsset", cape_material(quaternius / "UAL1_Standard_data/M_Main.plMaterialAsset", material_guid, colour))
        write(project / f"Characters/Mannequin/Mannequin_{garment}.plAnimatedMeshAsset", animated_mesh_asset(skeleton, [("M_Main", main_material), ("M_Joints", joints_material), (f"M_{garment}", material_guid)], mesh_guid, garment))
        # the scarf's tail lies on the chest: its shapes grow a little more, so that a hard stop does not put it inside
        inflate = 0.03 if garment == "Scarf" else 0.01
        write(project / f"Characters/Mannequin/Mannequin_{garment}.plJoltClothAsset", cloth_asset(jog, cloth_guid, mesh_guid, garment, max_distance, collider_inflate=inflate))
        print(f"{garment}: {made['vertices']} vertices, {made['triangles']} triangles")

    # A cape that reaches the floor and trails behind, for the stairs: it collides with the level.
    long_mesh = uuid.UUID("c107c000-434c-4f54-8000-000000000141")
    long_cloth = uuid.UUID("c107c000-434c-4f54-8000-000000000142")
    long_material = uuid.UUID("c107c000-434c-4f54-8000-000000000143")

    made = add_cape(quaternius / "UAL1_Standard.glb", project / "Characters/Mannequin/Mannequin_LongCape.glb", name="LongCape", hang=1.43, train=0.45, rows=40)
    assert made["materials"] == ["M_Main", "M_Joints", "M_LongCape"], made["materials"]

    write(project / "Characters/Mannequin/M_LongCape.plMaterialAsset", cape_material(quaternius / "UAL1_Standard_data/M_Main.plMaterialAsset", long_material, (0.30, 0.05, 0.32)))
    write(project / "Characters/Mannequin/Mannequin_LongCape.plAnimatedMeshAsset", animated_mesh_asset(skeleton, [("M_Main", main_material), ("M_Joints", joints_material), ("M_LongCape", long_material)], long_mesh, "LongCape"))
    write(project / "Characters/Mannequin/Mannequin_LongCape.plJoltClothAsset", cloth_asset(jog, long_cloth, long_mesh, "LongCape", 1.2, world_collision="Static", collider_inflate=0.03, iterations=12))
    wardrobe["LongCape"] = (long_mesh, long_cloth)
    print(f"LongCape: {made['vertices']} vertices, {made['triangles']} triangles")

    # A ponytail, a pouch and a tail on joints of their own, for the Bone Chains component. The mannequin with more
    # joints needs a skeleton of its own; the clips find their joints in it by name.
    chains_mesh = uuid.UUID("c107c000-434c-4f54-8000-000000000151")
    chains_skeleton = uuid.UUID("c107c000-434c-4f54-8000-000000000152")
    chains_material = uuid.UUID("c107c000-434c-4f54-8000-000000000153")

    made = add_chains(quaternius / "UAL1_Standard.glb", project / "Characters/Mannequin/Mannequin_Chains.glb")
    assert made["materials"] == ["M_Main", "M_Joints", "M_Chains"], made["materials"]

    write(project / "Characters/Mannequin/Mannequin_Chains.plSkeletonAsset", skeleton_asset(quaternius / "UAL1_Standard.plSkeletonAsset", chains_skeleton, "Characters/Mannequin/Mannequin_Chains.glb"))
    write(project / "Characters/Mannequin/M_Chains.plMaterialAsset", cape_material(quaternius / "UAL1_Standard_data/M_Main.plMaterialAsset", chains_material, (0.10, 0.08, 0.07)))
    write(project / "Characters/Mannequin/Mannequin_Chains.plAnimatedMeshAsset", animated_mesh_asset(chains_skeleton, [("M_Main", main_material), ("M_Joints", joints_material), ("M_Chains", chains_material)], chains_mesh, "Chains"))
    wardrobe["Chains"] = (chains_mesh, None)
    print(f"Chains: {made['joints']} joints, {made['vertices']} vertices")

    # Samples: one row, every garment jogging seen from behind and standing seen from the front.
    samples = []
    for column, garment in enumerate(("Cape", "Coat", "Skirt", "Scarf")):
        y = (column - 1.5) * 2.1
        samples.append((f"{garment}_Jog", (0.0, y - 0.5, 0.0), 30.0, jog, *wardrobe[garment], "Always"))
        samples.append((f"{garment}_Idle", (0.0, y + 0.5, 0.0), 180.0, idle, *wardrobe[garment], "Always"))

    write(project / "Scenes/ClothSamples.plScene", build_scene(sdk, jog, idle, uuid.UUID("c107c000-434c-4f54-8000-000000000023"), wind_strength=None, cast=samples, camera=((-5.2, 0.0, 1.3), 4.0)))
    write(project / "Scenes/ClothSamplesWind.plScene", build_scene(sdk, jog, idle, uuid.UUID("c107c000-434c-4f54-8000-000000000024"), wind_strength="StrongBreeze", cast=samples, camera=((-5.2, 0.0, 1.3), 4.0)))

    # Budget: 40 characters with ordinary importance, so that the budget and the distance rules decide who simulates.
    crowd = []
    for index in range(40):
        row, column = divmod(index, 8)
        garment = ("Cape", "Coat", "Skirt", "Scarf")[(row + column) % 4]
        crowd.append((f"Crowd_{index:02d}", (row * 3.0, (column - 3.5) * 1.6, 0.0), 20.0 + 35.0 * ((index * 7) % 5), jog if (row + column) % 3 else idle, *wardrobe[garment], "Normal"))

    # the camera flies in over the crowd, so that garments come into the budget and drop out of it while it is watched
    budget_camera = uuid.UUID("c107c000-434c-4f54-8000-000000000206")
    write(project / "Scenes/ClothBudgetCamera.plPropertyAnimAsset", property_animation(budget_camera, "budget camera", 14.0, [
        ("", "", "LocalPosition", "VectorX", [(0.0, -14.0), (1.0, -14.0), (13.0, 7.0), (14.0, 7.0)]),
    ]))
    write(project / "Scenes/ClothBudget.plScene", build_scene(sdk, jog, idle, uuid.UUID("c107c000-434c-4f54-8000-000000000025"), wind_strength=None, cast=crowd, camera=((-14.0, 0.0, 3.2), 14.0), camera_animation=budget_camera))

    write_scripted_scenes(sdk, project, jog, idle, wardrobe)

    print(f"ClothLab written to {project}: cape with {cape['vertices']} vertices and {cape['triangles']} triangles, plasma.jolt {version} installed")


if __name__ == "__main__":
    main()
