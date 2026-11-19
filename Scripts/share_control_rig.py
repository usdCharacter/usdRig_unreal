"""Give a character's controls file the Control Rig fields of another
character with the same rig (the same control paths and channels), so it
can be baked without building a Control Rig of its own.

    share_control_rig.py <with_rig_controls.json> <controls.json>

Copies each control's and channel's Control Rig element name and kind, and
the description's "space" and "root". Fails if the rigs differ.
"""
import json
import sys

src_path, dst_path = [a for a in sys.argv[1:] if not a.startswith('--')][:2]
src = json.load(open(src_path))
dst = json.load(open(dst_path))
controls = {c["path"]: c for c in src["controls"]}
channels = {(c["control"], c["attr"]): c for c in src["channels"]}
missing = [c["path"] for c in dst["controls"] if c["path"] not in controls]
missing += ["%s.%s" % (c["control"], c["attr"]) for c in dst["channels"] if (c["control"], c["attr"]) not in channels]
if missing and "--allow-extra" not in sys.argv:
    sys.exit("the rigs differ: %d controls or channels have no match, e.g. %s" % (len(missing), missing[:5]))
# --allow-extra: controls and channels the other rig lacks (a character's own
# hidden drivers and extra dials) keep their exported names; the shared
# Control Rig has no element for them, so Unreal does not animate them.
for c in dst["controls"]:
    for key in ("element", "kind"):
        if c["path"] in controls and key in controls[c["path"]]:
            c[key] = controls[c["path"]][key]
for c in dst["channels"]:
    other = channels.get((c["control"], c["attr"]))
    if other and "element" in other:
        c["element"] = other["element"]
if missing:
    print("kept %d controls or channels the other rig lacks" % len(missing))
for key in ("space", "root"):
    if key in src:
        dst[key] = src[key]
json.dump(dst, open(dst_path, "w"), indent=1)
print("shared %d controls and %d channels from %s" % (len(dst["controls"]), len(dst["channels"]), src_path))
