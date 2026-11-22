import bpy, bmesh, sys, os, time
OUT = os.environ["ASSET_OUT"]
TARGET_TRIS = int(os.environ.get('TRIS', '12000'))
RES = int(os.environ.get('RES', '1024'))
scene = bpy.context.scene
dg = bpy.context.evaluated_depsgraph_get()

def realise(name):
    o = bpy.data.objects[name]
    me = bpy.data.meshes.new_from_object(o.evaluated_get(dg), preserve_all_data_layers=True, depsgraph=dg)
    me.transform(o.matrix_world)
    ob = bpy.data.objects.new(name + '_hi', me)
    scene.collection.objects.link(ob)
    return ob

his = [realise('Ankh'), realise('Base')]
for o in bpy.data.objects:
    if not o.name.endswith('_hi'):
        o.hide_render = True
        o.hide_set(True)
print('hi faces', [len(o.data.polygons) for o in his])

# Low: copy, decimate, join
lows = []
for h in his:
    me = h.data.copy()
    lo = bpy.data.objects.new(h.name.replace('_hi', '_lo'), me)
    scene.collection.objects.link(lo)
    tris = sum(len(p.vertices) - 2 for p in me.polygons)
    share = 0.85 if h.name.startswith('Ankh') else 0.15
    ratio = min(1.0, TARGET_TRIS * share / tris)
    m = lo.modifiers.new('dec', 'DECIMATE'); m.ratio = ratio
    bpy.context.view_layer.objects.active = lo
    for o in scene.objects: o.select_set(False)
    lo.select_set(True)
    bpy.ops.object.modifier_apply(modifier='dec')
    lows.append(lo)
for o in scene.objects: o.select_set(False)
for lo in lows: lo.select_set(True)
bpy.context.view_layer.objects.active = lows[0]
bpy.ops.object.join()
low = bpy.context.view_layer.objects.active
low.name = 'ankh_lo'
low.data.materials.clear()
bm = bmesh.new(); bm.from_mesh(low.data)
bmesh.ops.triangulate(bm, faces=bm.faces[:])
bm.to_mesh(low.data); bm.free()
print('low tris', len(low.data.polygons))

# UVs
bpy.ops.object.mode_set(mode='EDIT')
bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.uv.smart_project(angle_limit=1.15, island_margin=0.004)
bpy.ops.object.mode_set(mode='OBJECT')

# Bake target material
img = bpy.data.images.new('ankh_diffuse', RES, RES, alpha=False)
mat = bpy.data.materials.new('ankh_bake')
mat.use_nodes = True
tn = mat.node_tree.nodes.new('ShaderNodeTexImage'); tn.image = img
mat.node_tree.nodes.active = tn
low.data.materials.append(mat)

scene.render.engine = 'CYCLES'
scene.cycles.samples = 8
scene.cycles.device = 'CPU'
for o in scene.objects: o.select_set(False)
for h in his: h.select_set(True)
low.select_set(True)
bpy.context.view_layer.objects.active = low
t = time.time()
bpy.ops.object.bake(type='DIFFUSE', pass_filter={'COLOR'}, use_selected_to_active=True,
                    cage_extrusion=0.002, max_ray_distance=0.006, margin=8)
print('baked diffuse in %.0fs' % (time.time() - t))
img.filepath_raw = os.path.join(OUT, 'ankh_diffuse.png'); img.file_format = 'PNG'; img.save()

# Normal map (tangent space, Blender's OpenGL convention), kept for later
nimg = bpy.data.images.new('ankh_normal', RES, RES, alpha=False)
nimg.colorspace_settings.name = 'Non-Color'
tn.image = nimg
t = time.time()
bpy.ops.object.bake(type='NORMAL', normal_space='TANGENT', use_selected_to_active=True,
                    cage_extrusion=0.002, max_ray_distance=0.006, margin=8)
print('baked normal in %.0fs' % (time.time() - t))
nimg.filepath_raw = os.path.join(OUT, 'ankh_normal.png'); nimg.file_format = 'PNG'; nimg.save()

bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT, 'ankh_low.blend'), copy=True)
print('done')
