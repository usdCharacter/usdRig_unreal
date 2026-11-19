"""
Describes a stage's layer stack and deformer stack for the Unreal Stack panel.

Runs under the Python that owns the rig's USD build (OpenUSD with OpenExec),
never inside Unreal:

    python export_stack.py <stage.usda> <out.json> [--rig /Biped/Rig]

What it writes, so the panel needs no USD and no rig compiler:

  layers   the root layer's sublayer tree, strongest first, flattened with a
           depth; each with its file's size and time (so the editor can tell
           a layer changed after the bake), and how many movers it defines
           or overrides.
  movers   the compiled execution order (rigexec.Rig.mover_order: first runs
           first), each with its type, targets, the layer that defines it,
           the layers that override it, and whether it is enabled.
  meshes   each deformed mesh with the movers that write its points, in
           that order: the mesh's deformer stack.
  solvers  the pose-phase solvers and how many joints each writes.
"""

import argparse
import datetime
import json
import os
import sys

from pxr import Sdf, Usd
import rigexec

rigexec.load_schema_plugin()

SOLVER_TYPES = ("RigExecFkChain", "RigExecTwoBoneIk", "RigExecBlendPointFrames",
                "RigExecTwistDistribution", "RigExecRibbon", "RigExecSplineIk")


def _FileInfo(identifier):
    path = identifier
    if not os.path.isfile(path):
        layer = Sdf.Layer.Find(identifier)
        path = layer.realPath if layer is not None else ""
    if path and os.path.isfile(path):
        st = os.stat(path)
        return os.path.abspath(path), int(st.st_mtime), st.st_size
    return path, 0, 0


def _Layers(stage):
    """The sublayer tree from the root down, strongest first, as
    (identifier, layer, depth) in pre-order."""
    out = []

    def walk(identifier, layer, depth, seen):
        out.append((identifier, layer, depth))
        if layer is None:
            return
        for sub in layer.subLayerPaths:
            child = Sdf.ComputeAssetPathRelativeToLayer(layer, sub)
            if child in seen:
                continue
            walk(child, Sdf.Layer.FindOrOpen(child), depth + 1, seen | {child})

    root = stage.GetRootLayer()
    walk(root.identifier, root, 0, {root.identifier})
    return out


def _Root(stage, wanted):
    if wanted:
        return wanted
    roots = [str(p.GetPath()) for p in stage.Traverse() if p.GetTypeName() == "RigExecRoot"]
    if not roots:
        raise RuntimeError("no RigExecRoot on the stage")
    return roots[0]


def Export(stagePath, outPath, rigRoot=None):
    stage = Usd.Stage.Open(stagePath)
    root = _Root(stage, rigRoot)
    rig = rigexec.Rig(stage, root)
    rig.compile()
    order = rig.mover_order
    if callable(order):
        order = order()
    skipped = rig.skipped_operations() if hasattr(rig, "skipped_operations") else {}

    layers = _Layers(stage)
    index = {layer.identifier: i for i, (_, layer, _) in enumerate(layers) if layer is not None}
    layerRows = []
    for identifier, layer, depth in layers:
        file, mtime, size = _FileInfo(identifier)
        specs = 0
        if layer is not None:
            def count(path):
                nonlocal specs
                if path.IsPrimPath():
                    specs += 1
            layer.Traverse(Sdf.Path.absoluteRootPath, count)
        layerRows.append({
            "name": os.path.basename(identifier.replace("\\", "/")),
            "file": file.replace("\\", "/"),
            "depth": depth,
            "missing": layer is None,
            "mtime": mtime,
            "size": size,
            "primSpecs": specs,
            "defines": 0,
            "overrides": 0,
        })

    movers = []
    for e in order:
        path = e["path"]
        prim = stage.GetPrimAtPath(path)
        defined, over = -1, []
        enabled = True
        if prim:
            for spec in prim.GetPrimStack():
                i = index.get(spec.layer.identifier)
                if i is None:
                    continue
                if spec.specifier == Sdf.SpecifierDef and defined < 0:
                    defined = i
                elif i not in over:
                    over.append(i)
            attr = prim.GetAttribute("inputs:enabled")
            if attr and attr.HasAuthoredValue():
                enabled = bool(attr.Get())
        over = [i for i in over if i != defined]
        if defined >= 0:
            layerRows[defined]["defines"] += 1
        for i in over:
            layerRows[i]["overrides"] += 1
        movers.append({
            "o": e["ordinal"],
            "path": path,
            "type": e["type"].replace("RigExec", ""),
            "targets": list(e.get("targets") or []),
            "layer": defined,
            "over": over,
            "enabled": enabled,
        })

    # Each mesh's deformer stack: the movers writing <mesh>.points, in order.
    meshes = {}
    for m in movers:
        for t in m["targets"]:
            if t.endswith(".points"):
                meshes.setdefault(t[:-len(".points")], []).append(m["o"])
    meshRows = [{"path": p, "name": p.rsplit("/", 1)[-1], "movers": o}
                for p, o in sorted(meshes.items(), key=lambda kv: (-len(kv[1]), kv[0]))]

    solvers = []
    for prim in stage.Traverse():
        if prim.GetTypeName() in SOLVER_TYPES and str(prim.GetPath()).startswith(root):
            rel = prim.GetRelationship("rigExec:joints")
            solvers.append({"type": prim.GetTypeName().replace("RigExec", ""),
                            "path": str(prim.GetPath()),
                            "joints": len(rel.GetTargets()) if rel else 0})
    solvers.sort(key=lambda s: (s["type"], s["path"]))

    out = {
        "version": 1,
        "stage": os.path.abspath(stagePath).replace("\\", "/"),
        "rigRoot": root,
        "exported": datetime.datetime.now().isoformat(timespec="seconds"),
        "layers": layerRows,
        "movers": movers,
        "meshes": meshRows,
        "solvers": solvers,
        "skipped": [{"path": str(k), "reason": str(v)} for k, v in dict(skipped).items()],
    }
    with open(outPath, "w") as f:
        json.dump(out, f, separators=(",", ":"))
    print("export_stack: %d layers, %d movers, %d meshes, %d solvers -> %s" % (
        len(layerRows), len(movers), len(meshRows), len(solvers), outPath))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("stage")
    parser.add_argument("out")
    parser.add_argument("--rig", default=None, help="the RigExecRoot (default: the first)")
    args = parser.parse_args()
    Export(args.stage, args.out, args.rig)
