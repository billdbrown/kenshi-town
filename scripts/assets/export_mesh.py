"""Run inside Blender on ankh_low.blend: write the low-poly ankh as an Ogre
binary mesh in Kenshi's layout (MeshSerializer_v1.100, one 'StaticObject'
submesh, float3 position/normal/uv/tangent at 44 bytes a vertex)."""
import bpy, struct, os, math
OUT = os.environ["ASSET_OUT"]
HEIGHT = float(os.environ.get('HEIGHT', '18'))  # Kenshi units, ~10 cm each

ob = bpy.data.objects['ankh_lo']
me = ob.data
me.calc_tangents()
uv = me.uv_layers.active.data

def ogre(v):  # Blender Z-up -> Ogre Y-up
    return (v[0], v[2], -v[1])

pts = [ogre(v.co) for v in me.vertices]
lo = [min(p[i] for p in pts) for i in range(3)]
hi = [max(p[i] for p in pts) for i in range(3)]
s = HEIGHT / (hi[1] - lo[1])
off = (-(lo[0] + hi[0]) / 2, -lo[1], -(lo[2] + hi[2]) / 2)
print('blender size', [round(hi[i] - lo[i], 4) for i in range(3)], 'scale', s)

verts = bytearray()
count = 0
for poly in me.polygons:
    assert len(poly.loop_indices) == 3
    for li in poly.loop_indices:
        l = me.loops[li]
        p = ogre(me.vertices[l.vertex_index].co)
        p = tuple((p[i] + off[i]) * s for i in range(3))
        n = ogre(l.normal)
        t = ogre(l.tangent)
        u, v = uv[li].uv
        verts += struct.pack('<3f3f2f3f', *p, *n, u, 1.0 - v, *t)
        count += 1
assert count < 65536
idx = struct.pack('<%dH' % count, *range(count))

def chunk(cid, body):
    return struct.pack('<HI', cid, len(body) + 6) + body

decl = b''.join(chunk(0x5110, struct.pack('<5H', 0, typ, sem, offs, 0))
                for typ, sem, offs in ((2, 1, 0), (2, 4, 12), (1, 7, 24), (2, 9, 32)))
geom = chunk(0x5000, struct.pack('<I', count)
             + chunk(0x5100, decl)
             + chunk(0x5200, struct.pack('<HH', 0, 44) + chunk(0x5210, bytes(verts))))
sub = chunk(0x4000, b'StaticObject\n' + struct.pack('<BIB', 0, count, 0) + idx
            + geom + chunk(0x4010, struct.pack('<H', 4)))
bmin = [(lo[i] + off[i]) * s for i in range(3)]
bmax = [(hi[i] + off[i]) * s for i in range(3)]
radius = math.sqrt(sum(max(abs(bmin[i]), abs(bmax[i])) ** 2 for i in range(3)))
bounds = chunk(0x9000, struct.pack('<7f', *bmin, *bmax, radius))
names = chunk(0xA000, chunk(0xA100, struct.pack('<H', 0) + b'submesh0\n'))
mesh = chunk(0x3000, struct.pack('<B', 0) + sub + bounds + names)
data = struct.pack('<H', 0x1000) + b'[MeshSerializer_v1.100]\n' + mesh
open(os.path.join(OUT, 'ankh.mesh'), 'wb').write(data)
print('wrote ankh.mesh', len(data), 'bytes', count, 'verts', 'bounds', [round(x, 2) for x in bmin + bmax])
