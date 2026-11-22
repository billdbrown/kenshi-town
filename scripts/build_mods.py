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


def base(db, sid_and_name):
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

    start = mod.new(base(db, WANDERER_START), "Kenshi Town: The Hub")
    start.set("str", "description",
              "The Hub is a ruin and nobody wants it. Claim it, clear it and rebuild it into a town "
              "worth living in. (Kenshi Town: open the Town Ledger with F8.)")
    start.set("str", "style", "Town")
    # Starts at the Hub with the Wanderer's squad and money, inherited from
    # the clone. Settlers and stock are a design decision still to make.

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
    return mod


def install(path):
    name = os.path.splitext(os.path.basename(path))[0]
    dest = os.path.join(KENSHI_MODS, name)
    if not os.path.isdir(dest):
        os.makedirs(dest)
    shutil.copy2(path, os.path.join(dest, name + ".mod"))
    print("installed  %s" % os.path.normpath(dest))


def main(argv):
    db = kmod.Database()
    paths = [town_ledger(db).write(), dev(db).write()]
    if argv[1:] == ["install"]:
        for p in paths:
            install(p)


if __name__ == "__main__":
    main(sys.argv)
