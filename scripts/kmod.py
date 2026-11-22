"""Read and write Kenshi data files (.base / .mod), format versions 16 and 17.

Layout, after OpenConstructionSet's OcsReader/OcsWriter (MIT), which is the
reference this follows:

    int  fileType                       16 = mod, 17 = mod with merge header
    header                              v17 wraps it in an int byte length
        int version, str author, str description, str dependencies, str references
        v17 only: uint saveCount, uint lastMerge, byte n + n*(str, uint, uint),
                  byte n + n*(str, uint, str)
    int  lastId
    int  recordCount
    record*
        int  lead                       see Record.lead
        int  type, int id, str name, str stringId, uint changeFlags
        7 x (int n + n*(str key, value))   bool(byte) float int vec3 vec4 str file
        int n + n*(str category, int m + m*(str sid, int v0, int v1, int v2))
        int n + n*(str id, str target, vec3 pos, vec4 rot, int m + m*str)

Strings are int32 length + UTF-8. The test is exactness: read then write must
reproduce every vanilla file byte for byte (see `python kmod.py selftest`).
"""

import os
import struct
import sys
from collections import OrderedDict

KENSHI_DATA = os.environ.get(
    "KENSHI_DATA", r"C:\Program Files (x86)\Steam\steamapps\common\Kenshi\data")
VANILLA = ["gamedata.base", "Newwworld.mod", "rebirth.mod", "Dialogue.mod"]

# Record type ids, in the order of OCS's ItemType enum.
TYPES = """Building Character Weapon Armour Item AnimalAnimation Attachment Race Location
WarSavestate Faction NullItem ZoneMap Town WorldmapCharacter CharacterAppearanceOld
LocationalDamage CombatTechnique Dialogue DialogueLine Techtree Research AiTask AiState
Animation Stats Personality Constants Biomes BuildingPart InstanceCollection DialogAction
TemporaryInfo ModFilename Platoon GamestateBuilding GamestateCharacter GamestateFaction
GamestateTownInstanceList State SavedState InventoryState InventoryItemState
RepeatableBuildingPartSlot MaterialSpec MaterialSpecsCollection Container
MaterialSpecsClothing GamestateBuildingInterior VendorList MaterialSpecsWeapon
WeaponManufacturer SquadTemplate Road LocationNode ColorData Camera MedicalState
MedicalPartState FoliageLayer FoliageMesh Grass BuildingFunctionality DaySchedule
NewGameStartoff GamestateCrafting CharacterAppearance GamestateAi WildlifeBirds
MapFeatures DiplomaticAssaults SingleDiplomaticAssault AiPackage DialoguePackage GunData
HumanCharacter AnimalCharacter UniqueSquadTemplate FactionTemplate AiSchedule Weather
Season Effect ItemPlacementGroup WordSwaps Nest NestItem CharacterPhysicsAttachment Light
Head Blueprint ShopTraderClass FoliageBuilding FactionCampaign GamestateTown BiomeGroup
EffectFogVolume FarmData FarmPart EnvironmentResources RaceGroup Artifacts MapItem
BuildingsSwap ItemsCulture AnimationEvent Tutorial Crossbow TerrainDecals AmbientSound
WorldEventState LimbReplacement AnimationFile Boat GamestateBoat BuildGrid
BuildingShell""".split()
TYPE_ID = {name: i for i, name in enumerate(TYPES)}

VALUE_KINDS = ["bool", "float", "int", "vec3", "vec4", "str", "file"]

# A reference whose three values are all int.MaxValue deletes that reference
# from the record being overridden. This is how FCS writes reference removal.
DELETED = 2147483647
CHANGE_NEW = 0
CHANGE_CHANGED = 1


class Reader(object):
    def __init__(self, data):
        self.b = data
        self.p = 0

    def int(self):
        v = struct.unpack_from("<i", self.b, self.p)[0]
        self.p += 4
        return v

    def uint(self):
        v = struct.unpack_from("<I", self.b, self.p)[0]
        self.p += 4
        return v

    def float(self):
        v = struct.unpack_from("<f", self.b, self.p)[0]
        self.p += 4
        return v

    def byte(self):
        v = self.b[self.p]
        self.p += 1
        return v

    def raw(self, n):
        v = self.b[self.p:self.p + n]
        self.p += n
        return v

    def str(self):
        n = self.int()
        # Kept as bytes: some vanilla strings aren't valid UTF-8, and exact
        # round-tripping matters more than pretty text.
        return self.raw(n)

    def floats(self, n):
        v = struct.unpack_from("<%df" % n, self.b, self.p)
        self.p += 4 * n
        return v


class Writer(object):
    def __init__(self):
        self.parts = []

    def int(self, v):
        self.parts.append(struct.pack("<i", v))

    def uint(self, v):
        self.parts.append(struct.pack("<I", v))

    def float(self, v):
        self.parts.append(struct.pack("<f", v))

    def byte(self, v):
        self.parts.append(struct.pack("<B", v))

    def raw(self, v):
        self.parts.append(v)

    def str(self, v):
        if not isinstance(v, bytes):
            v = v.encode("utf-8")
        self.int(len(v))
        self.parts.append(v)

    def floats(self, vs):
        self.parts.append(struct.pack("<%df" % len(vs), *vs))

    def bytes(self):
        return b"".join(self.parts)


def _b(s):
    return s if isinstance(s, bytes) else s.encode("utf-8")


def text(s):
    return s.decode("utf-8", "replace") if isinstance(s, bytes) else s


class Record(object):
    """One record. Strings are stored as bytes; helpers accept str too.

    `lead` is the int before each record. FCS-written mods store the record's
    byte length including the lead itself (KenshiCoop.mod: 1119 for a
    1115-byte body); vanilla files store 0. The writer computes the FCS form
    unless `keep_lead` is set.
    """

    def __init__(self, type_, id_, name, sid, change=CHANGE_NEW):
        self.lead = None
        self.type = type_ if isinstance(type_, int) else TYPE_ID[type_]
        self.id = id_
        self.name = _b(name)
        self.sid = _b(sid)
        self.change = change
        self.values = OrderedDict((k, OrderedDict()) for k in VALUE_KINDS)
        self.refs = OrderedDict()
        self.instances = []

    @property
    def type_name(self):
        return TYPES[self.type] if self.type < len(TYPES) else str(self.type)

    def get(self, key, default=None):
        key = _b(key)
        for kind in VALUE_KINDS:
            if key in self.values[kind]:
                return self.values[kind][key]
        return default

    def kind_of(self, key):
        key = _b(key)
        for kind in VALUE_KINDS:
            if key in self.values[kind]:
                return kind
        return None

    def set(self, kind, key, value):
        self.values[kind][_b(key)] = _b(value) if kind in ("str", "file") else value

    def add_ref(self, category, sid, v0=0, v1=0, v2=0):
        self.refs.setdefault(_b(category), []).append((_b(sid), v0, v1, v2))

    def delete_ref(self, category, sid):
        self.add_ref(category, sid, DELETED, DELETED, DELETED)

    def __repr__(self):
        return "<%s %s %r>" % (self.type_name, text(self.sid), text(self.name))


def _read_record(r):
    lead = r.int()
    start = r.p
    rec = Record(r.int(), r.int(), b"", b"")
    rec.name = r.str()
    rec.sid = r.str()
    rec.change = r.uint()
    rec.lead = lead
    readers = {
        "bool": lambda: r.byte(),
        "float": r.float,
        "int": r.int,
        "vec3": lambda: r.floats(3),
        "vec4": lambda: r.floats(4),
        "str": r.str,
        "file": r.str,
    }
    for kind in VALUE_KINDS:
        for _ in range(r.int()):
            k = r.str()
            rec.values[kind][k] = readers[kind]()
    for _ in range(r.int()):
        cat = r.str()
        rec.refs[cat] = [(r.str(), r.int(), r.int(), r.int()) for _ in range(r.int())]
    for _ in range(r.int()):
        iid = r.str()
        target = r.str()
        pos = r.floats(3)
        rot = r.floats(4)
        states = [r.str() for _ in range(r.int())]
        rec.instances.append((iid, target, pos, rot, states))
    rec._body_len = r.p - start
    return rec


def _write_record_body(rec):
    w = Writer()
    w.int(rec.type)
    w.int(rec.id)
    w.str(rec.name)
    w.str(rec.sid)
    w.uint(rec.change)
    for kind in VALUE_KINDS:
        vals = rec.values[kind]
        w.int(len(vals))
        for k, v in vals.items():
            w.str(k)
            if kind == "bool":
                w.byte(1 if v else 0)
            elif kind == "float":
                w.float(v)
            elif kind == "int":
                w.int(v)
            elif kind in ("vec3", "vec4"):
                w.floats(v)
            else:
                w.str(v)
    # Never write an empty category: FCS doesn't, and a count that doesn't
    # match what follows corrupts the file.
    cats = [(c, refs) for c, refs in rec.refs.items() if refs]
    w.int(len(cats))
    for cat, refs in cats:
        w.str(cat)
        w.int(len(refs))
        for sid, v0, v1, v2 in refs:
            w.str(sid)
            w.int(v0)
            w.int(v1)
            w.int(v2)
    w.int(len(rec.instances))
    for iid, target, pos, rot, states in rec.instances:
        w.str(iid)
        w.str(target)
        w.floats(pos)
        w.floats(rot)
        w.int(len(states))
        for s in states:
            w.str(s)
    return w.bytes()


class ModFile(object):
    def __init__(self, file_type=16, author="", description="", dependencies=(), references=()):
        self.file_type = file_type
        self.version = 1
        self.author = _b(author)
        self.description = _b(description)
        self.dependencies = _b(",".join(dependencies))
        self.references = _b(",".join(references))
        self.v17 = None  # (saveCount, lastMerge, merges, deletes) for v17 files
        self.last_id = 0
        self.records = []

    def add(self, rec):
        self.records.append(rec)
        return rec

    def new_id(self, mod_name):
        """Next stringId for a record that originates in this mod."""
        self.last_id += 1
        return "%d-%s" % (self.last_id, mod_name)


def read(path):
    with open(path, "rb") as f:
        r = Reader(f.read())
    m = ModFile()
    m.file_type = r.int()
    if m.file_type not in (16, 17):
        raise ValueError("%s: unsupported file type %d" % (path, m.file_type))
    end = None
    if m.file_type == 17:
        n = r.int()
        end = r.p + n
    m.version = r.int()
    m.author = r.str()
    m.description = r.str()
    m.dependencies = r.str()
    m.references = r.str()
    if m.file_type == 17:
        # Each trailing part is optional: Dialogue.mod stops after the merges.
        save_count = last_merge = merges = deletes = None
        if r.p < end:
            save_count = r.uint()
            last_merge = r.uint()
        if r.p < end:
            merges = [(r.str(), r.uint(), r.uint()) for _ in range(r.byte())]
        if r.p < end:
            deletes = [(r.str(), r.uint(), r.str()) for _ in range(r.byte())]
        m.v17 = (save_count, last_merge, merges, deletes)
        if r.p != end:
            raise ValueError("%s: v17 header length mismatch" % path)
    m.last_id = r.int()
    for _ in range(r.int()):
        m.records.append(_read_record(r))
    if r.p != len(r.b):
        raise ValueError("%s: %d trailing bytes" % (path, len(r.b) - r.p))
    return m


def dumps(m, keep_lead=False):
    w = Writer()
    w.int(m.file_type)
    h = Writer()
    h.int(m.version)
    h.str(m.author)
    h.str(m.description)
    h.str(m.dependencies)
    h.str(m.references)
    if m.file_type == 17:
        save_count, last_merge, merges, deletes = m.v17 or (0, 0, [], [])
        if save_count is not None:
            h.uint(save_count)
            h.uint(last_merge)
        if merges is not None:
            h.byte(len(merges))
            for s, a, b in merges:
                h.str(s)
                h.uint(a)
                h.uint(b)
        if deletes is not None:
            h.byte(len(deletes))
            for s, a, t in deletes:
                h.str(s)
                h.uint(a)
                h.str(t)
        header = h.bytes()
        w.int(len(header))
        w.raw(header)
    else:
        w.raw(h.bytes())
    w.int(m.last_id)
    w.int(len(m.records))
    for rec in m.records:
        body = _write_record_body(rec)
        w.int(rec.lead if keep_lead and rec.lead is not None else len(body) + 4)
        w.raw(body)
    return w.bytes()


def write(m, path):
    data = dumps(m)
    d = os.path.dirname(path)
    if d and not os.path.isdir(d):
        os.makedirs(d)
    with open(path, "wb") as f:
        f.write(data)
    return len(data)


# ---- The merged vanilla world, for looking things up and cloning from.

class Database(object):
    """Vanilla records merged in load order, keyed by stringId.

    Merging follows the engine: later files' values replace earlier ones,
    references are keyed by target sid within a category, and a reference with
    all values DELETED removes it. Records marked REMOVED are dropped.
    """

    def __init__(self, files=None, data_dir=KENSHI_DATA):
        self.records = OrderedDict()
        for name in files or VANILLA:
            for rec in read(os.path.join(data_dir, name)).records:
                self._merge(rec)
        for sid in [s for s, r in self.records.items() if r.get("REMOVED")]:
            del self.records[sid]

    def _merge(self, rec):
        cur = self.records.get(rec.sid)
        if cur is None:
            copy = Record(rec.type, rec.id, rec.name, rec.sid, rec.change)
            for kind in VALUE_KINDS:
                copy.values[kind].update(rec.values[kind])
            for cat, refs in rec.refs.items():
                copy.refs[cat] = [r for r in refs if r[1:] != (DELETED,) * 3]
            copy.instances = list(rec.instances)
            self.records[rec.sid] = copy
            return
        cur.name = rec.name
        for kind in VALUE_KINDS:
            cur.values[kind].update(rec.values[kind])
        for cat, refs in rec.refs.items():
            by_sid = OrderedDict((r[0], r) for r in cur.refs.get(cat, []))
            for r in refs:
                if r[1:] == (DELETED,) * 3:
                    by_sid.pop(r[0], None)
                else:
                    by_sid[r[0]] = r
            cur.refs[cat] = list(by_sid.values())
        cur.instances.extend(rec.instances)

    def __getitem__(self, sid):
        return self.records[_b(sid)]

    def of_type(self, type_name):
        t = TYPE_ID[type_name]
        return [r for r in self.records.values() if r.type == t]

    def named(self, name, type_name=None):
        name = _b(name)
        hits = [r for r in self.records.values()
                if r.name == name and (type_name is None or r.type_name == type_name)]
        if len(hits) != 1:
            raise KeyError("%d records named %r (type %s)" % (len(hits), text(name), type_name))
        return hits[0]


def override(base):
    """An empty override of `base`: same stringId, change type 'changed'.

    Fill in only what changes. FCS writes 0x21 here (save count 2, changed).
    """
    return Record(base.type, base.id, base.name, base.sid, 0x20 | CHANGE_CHANGED)


def clone(base, sid, name):
    """A new record copying `base`'s values and references (not instances)."""
    # 0x10 = save count 1, new: what FCS writes for a fresh record.
    rec = Record(base.type, 0, name, sid, 0x10 | CHANGE_NEW)
    for kind in VALUE_KINDS:
        rec.values[kind].update(base.values[kind])
    for cat, refs in base.refs.items():
        rec.refs[cat] = list(refs)
    return rec


def selftest(data_dir=KENSHI_DATA):
    """Round-trip every vanilla file byte for byte, plus KenshiCoop.mod (an
    FCS-written mod) with recomputed leads, which tests the writer's own
    length calculation. Returns True when all pass."""
    ok = True
    paths = [os.path.join(data_dir, n) for n in VANILLA]
    coop = os.path.join(data_dir, os.pardir, "mods", "KenshiCoop", "KenshiCoop.mod")
    if os.path.exists(coop):
        paths.append(coop)
    for path in paths:
        name = os.path.basename(path)
        with open(path, "rb") as f:
            original = f.read()
        m = read(path)
        exact = dumps(m, keep_lead=True) == original
        recomputed = dumps(m) == original
        leads_are_lengths = all(r.lead == r._body_len + 4 for r in m.records)
        print("%-14s v%d %6d records  exact=%s  recomputed-leads=%s  leads-are-lengths=%s"
              % (name, m.file_type, len(m.records), exact, recomputed, leads_are_lengths))
        ok = ok and exact and (recomputed or not leads_are_lengths)
    return ok


if __name__ == "__main__":
    if sys.argv[1:] == ["selftest"]:
        sys.exit(0 if selftest() else 1)
    for p in sys.argv[1:]:
        m = read(p)
        print("%s: v%d, %d records, deps %s" % (p, m.file_type, len(m.records), text(m.dependencies)))
        for rec in m.records:
            print("  ", rec, "change=0x%x" % rec.change)
