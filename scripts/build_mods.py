"""Build kenshi-town's data mods from the vanilla game data.

    python scripts/build_mods.py            build into build/mods/
    python scripts/build_mods.py install    build, then copy into Kenshi/mods/

Two mods:

  TownLedger     shipped alongside the plugin (same folder, same name):
                   - a "Kenshi Town" new-game start at the Hub
                   - the Hub's Shinobi Thieves Guild removed from its residents
  KenshiTownDev  development only, never shipped:
                   - every research free, instant, at the first bench tier,
                     and with no prerequisites
                   - "Faience Ankh", a custom-model building (asset pipeline
                     test; scripts/assets/build_ankh.cmd makes its files)

Every base record is looked up by stringId and checked by name, so a typo or a
game update that moves a record fails the build instead of shipping a mod that
silently does nothing.
"""

import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kmod  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(REPO, "build", "mods")
KENSHI_MODS = os.path.join(kmod.KENSHI_DATA, os.pardir, "mods")
DEPENDENCIES = ["gamedata.base", "Newwworld.mod", "rebirth.mod", "Dialogue.mod"]

# Vanilla records we build on: stringId -> expected name.
WANDERER_START = ("1980-gamedata.base", "Wanderer")
THE_HUB = ("18919-Newwworld.mod", "The Hub")
THIEVES_GUILD = ("51709-rebirth.mod", "Thieves Guild")
WANDERER_SQUAD = ("45550-gamedata.base", "startoff- Wanderer squad")
TORCH_POST = ("5437-new_furniture_otto.mod", "Torch Post")
SIGNS_MATERIAL = ("973-gamedata.base", "signs")
DEFAULT_START = ("5359-gamedata.base", "_Default Start")
THRONE_USE = ("57289-rebirth.mod", "throne chair")       # BuildingFunctionality, BF_THRONE
BOWING = ("44934-rebirth.mod", "bowing")                  # the fogmen's worship animation
NOTE_ITEM = None  # "Memo From Big Grim", resolved by name in town_ledger()

# Must match DEED_NAME and FOUNDER_NAME in src/TownLedger.cpp.
DEED_NAME = "Deed to the Hub"
FOUNDER_NAME = "Kenshi Town founder"


def base(db, sid_and_name):
    if sid_and_name is None:  # NOTE_ITEM: found by name, then checked like the rest
        rec = db.named("Memo From Big Grim", "Item")
        return rec
    sid, name = sid_and_name
    rec = db[sid]
    if kmod.text(rec.name) != name:
        raise SystemExit("%s is %r, expected %r: the game data changed" % (sid, kmod.text(rec.name), name))
    return rec


class Mod(object):
    """A mod being built: allocates stringIds in FCS's `<n>-<file>` form."""

    def __init__(self, name, description):
        self.name = name
        self.file = kmod.ModFile(16, description=description, dependencies=DEPENDENCIES)

    def new(self, base_rec, name):
        sid = self.file.new_id(self.name + ".mod")
        rec = kmod.clone(base_rec, sid, name)
        rec.id = self.file.last_id
        return self.file.add(rec)

    def override(self, base_rec):
        return self.file.add(kmod.override(base_rec))

    def write(self):
        path = os.path.join(OUT, self.name, self.name + ".mod")
        size = kmod.write(self.file, path)
        # Prove the file reads back to what we meant to write.
        again = kmod.read(path)
        assert kmod.dumps(again) == kmod.dumps(self.file), path
        print("%-14s %3d records  %6d bytes  %s" % (self.name, len(self.file.records), size, path))
        return path


def town_ledger(db):
    mod = Mod("TownLedger", "Kenshi Town: rebuild and run the Hub.")

    # The deed: the founder arrives holding it, and the plugin claims the Hub
    # for whoever holds it in a game where the Hub was never claimed. Cloned
    # from a vanilla note item so it looks and handles like one.
    deed = mod.new(base(db, NOTE_ITEM), DEED_NAME)
    deed.set("str", "description",
             "A deed to the Hub, signed by the last Holy Nation official who still cared. The walls, "
             "the ruins and the trouble are yours now. The Outlaws who guard the place have agreed to "
             "stay on for ten days.")

    # Player starts don't hand out a squad's "special items" (tried 2026-11-22:
    # no deed). Starting items come from the leader's own inventory list, as
    # (item, count, chance %, 0), so the founder is a character of our own.
    # The plugin also recognises the founder by this name (FOUNDER_NAME).
    wanderer_squad = base(db, WANDERER_SQUAD)
    leader_sid = wanderer_squad.refs[b"leader"][0][0]
    founder_char = mod.new(db[leader_sid], FOUNDER_NAME)
    founder_char.refs[b"inventory"] = [(kmod._b(deed.sid), 1, 100, 0)]

    founder = mod.new(wanderer_squad, "startoff- Kenshi Town founder")
    founder.refs[b"leader"] = [(kmod._b(founder_char.sid), 1, 0, 0)]

    start = mod.new(base(db, WANDERER_START), "Kenshi Town: The Hub")
    start.set("str", "description",
              "The Hub is a ruin and you hold the deed to it. Clear it, rebuild it and run it as a "
              "town worth living in. (Kenshi Town: the Town button opens the ledger and roster.)")
    start.set("str", "style", "Town")
    start.refs[b"squad"] = [(kmod._b(founder.sid), 0, 0, 0)]
    # Starts at the Hub with the Wanderer's money, inherited from the clone.

    hub = mod.override(base(db, THE_HUB))
    thieves = base(db, THIEVES_GUILD)
    if not any(r[0] == thieves.sid for r in db[THE_HUB[0]].refs.get(b"residents", [])):
        raise SystemExit("Thieves Guild is no longer a resident of The Hub")
    hub.delete_ref("residents", thieves.sid)
    return mod


def dev(db):
    mod = Mod("KenshiTownDev", "Kenshi Town development: all research free, instant, no prerequisites.")
    count = 0
    for research in db.of_type("Research"):
        rec = mod.override(research)
        for key, value in (("time", 1), ("money", 0), ("level", 0)):
            if research.kind_of(key) == "int":
                rec.set("int", key, value)
        if research.kind_of("blueprint only") == "bool":
            rec.set("bool", "blueprint only", 0)
        for category in (b"cost", b"requirements"):
            for sid, _, _, _ in research.refs.get(category, []):
                rec.delete_ref(category, sid)
        count += 1
    if count < 100:
        raise SystemExit("only %d research records found; expected ~185" % count)
    ankh(db, mod)
    return mod


ANKH_DIR = os.path.join(REPO, "build", "assets", "ankh")
ANKH_FILES = ["ankh.mesh", "ankh.xml", "ankh.dds", "ankh_n.dds"]


def ankh(db, mod):
    """A custom model as a placeable building: proves the asset pipeline.

    A Building's look is a list of BuildingParts; each part is a mesh (Ogre
    .mesh, whose submesh material is just "StaticObject") plus a MaterialSpec
    naming the textures. Built on the Torch Post (a plain exterior prop on
    the default build list), without its light. Without collision it could be
    placed but nobody would build it.
    """
    where = ".\mods\%s\\" % mod.name
    mat = mod.new(base(db, SIGNS_MATERIAL), "Kenshi Town ankh")
    mat.set("file", "texture map", where + "ankh.dds")
    mat.set("file", "normal map", where + "ankh_n.dds")
    mat.set("float", "specular mult", 0.3)

    torch = base(db, TORCH_POST)
    part = mod.new(db[torch.refs[b"parts"][0][0]], "Kenshi Town ankh part")
    part.set("file", "phs or mesh", where + "ankh.mesh")
    part.set("file", "xml collision", where + "ankh.xml")  # a box; see assets/collision.py
    part.refs[b"material"] = [(kmod._b(mat.sid), 0, 100, 0)]

    # Worship: a throne's "use" (sit here, play this animation) with the
    # fogmen's bowing in place of sitting. A user goes to the building's
    # origin and faces +Z (the vanilla Boss Chair's back is at -Z), so the
    # mesh stands 7 units in front of its origin (export_mesh OFFSET_Z).
    # (Created after the building: the building's id, 3-KenshiTownDev.mod,
    # is in saves that have one placed.)
    building = mod.new(torch, "Faience Ankh")
    use = mod.new(base(db, THRONE_USE), "Kenshi Town ankh worship")
    use.refs[b"animation"] = [(kmod._b(base(db, BOWING).sid), 0, 0, 0)]
    building.refs = kmod.OrderedDict([(b"parts", [(kmod._b(part.sid), 0, 100, 0)]),
                                      (b"functionality", [(kmod._b(use.sid), 0, 0, 0)])])
    building.set("str", "building category", "EXTERIOR")
    building.set("str", "Description", "A faience ankh, taller than a man. Kenshi Town's first custom model.")
    building.set("int", "build materials", 2)

    # The research loop above already overrides _Default Start; one record per id.
    start = [r for r in mod.file.records if r.sid == kmod._b(DEFAULT_START[0])]
    start = start[0] if start else mod.override(base(db, DEFAULT_START))
    start.add_ref("enable buildings", building.sid)


def install(path):
    name = os.path.splitext(os.path.basename(path))[0]
    dest = os.path.join(KENSHI_MODS, name)
    if not os.path.isdir(dest):
        os.makedirs(dest)
    shutil.copy2(path, os.path.join(dest, name + ".mod"))
    if name == "TownLedger":
        import assets.chime as chime  # noqa: E402  the task-complete sound
        chime.write(os.path.join(dest, "task_complete.wav"))
    if name == "KenshiTownDev":
        for f in ANKH_FILES:
            src = os.path.join(ANKH_DIR, f)
            if not os.path.isfile(src):
                raise SystemExit("%s missing: run scripts\assets\build_ankh.cmd first" % src)
            shutil.copy2(src, os.path.join(dest, f))
    print("installed  %s" % os.path.normpath(dest))


def main(argv):
    db = kmod.Database()
    paths = [town_ledger(db).write(), dev(db).write()]
    if argv[1:] == ["install"]:
        for p in paths:
            install(p)


if __name__ == "__main__":
    main(sys.argv)
