"""Write a PhysX (NXUSTREAM2) collision file for a mesh: one box fitted to
the mesh's bounds, using the vanilla torch's file as the template.

    python collision.py path\to\thing.mesh      writes path\to\thing.xml

Kenshi seems to need collision before anyone will build a building (the
ankh without one could be placed but not built, 2026-11-22). In these files
the box `dimensions` are half-extents and the frame is Z-up: the torch's box
(centre z 4.42, half-height 7.97) spans exactly its mesh's Y bounds."""
import os, re, struct, sys

TEMPLATE = os.path.join(os.environ.get("KENSHI_DIR", r"C:\Program Files (x86)\Steam\steamapps\common\Kenshi"),
                        "data", "buildings", "lights", "torch1.xml")


def mesh_bounds(path):
    d = open(path, "rb").read()
    i = d.rfind(struct.pack("<HI", 0x9000, 34))
    assert i > 0, "no bounds chunk"
    return struct.unpack_from("<6f", d, i + 6)


def main(mesh):
    x0, y0, z0, x1, y1, z1 = mesh_bounds(mesh)
    name = os.path.splitext(os.path.basename(mesh))[0]
    s = open(TEMPLATE, encoding="utf-8").read()
    box = 'NxBoxShapeDesc dimensions="%.6f %.6f %.6f"' % ((x1 - x0) / 2, (z1 - z0) / 2, (y1 - y0) / 2)
    pose = "<localPose>1 0 0  0 1 0  0 0 1  %.6f %.6f %.6f</localPose>" % ((x0 + x1) / 2, -(z0 + z1) / 2, (y0 + y1) / 2)
    s, n = re.subn(r'NxBoxShapeDesc dimensions="[^"]*"', box, s)
    assert n == 1
    s, n = re.subn(r'(<NxShapeDesc userProperties="" name=")torch1(">\s*)<localPose>[^<]*</localPose>',
                   lambda m: m.group(1) + name + m.group(2) + pose, s)
    assert n == 1
    s = s.replace('name="torch1"', 'name="%s"' % name).replace("torch1.xml", name + ".xml")
    out = os.path.splitext(mesh)[0] + ".xml"
    open(out, "w", encoding="utf-8", newline="").write(s)
    print("wrote", out, box, pose)


if __name__ == "__main__":
    main(sys.argv[1])
