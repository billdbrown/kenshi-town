# Reverse-engineering helpers

For finding game functions KenshiLib doesn't name (or names at the wrong address).
`dis.py` and `find90.py` need capstone 5.0.1 (`pip install capstone==5.0.1`, in a venv).

    python xref.py <kenshi_x64.exe> <KenshiLib Include/kenshi> "<string>"...
        which functions reference a string, named from KenshiLib's RVA comments
    python dis.py  <kenshi_x64.exe> <KenshiLib Include/kenshi> <start-rva> <end-rva>
        disassemble a range; calls and strings annotated
    python find90.py <kenshi_x64.exe> <KenshiLib Include/kenshi>
        writes to [reg+0x90] in the building-preview code (how inTown's setter was found;
        edit the offset and range for other fields)

Found this way: "Can't build too close to another town." comes from the click handler at
RVA 0x4E27E0, which refuses when PreviewBuilding::inTown (+0x90) is **set**: inTown is the
town a placement would intrude on, null means free. (Read it backwards once; it cost a day.)

These RVAs are for the exe on disk. In memory the same code sits at other addresses (the
function at disk 0x4D6D90 was at 0x4D6300 at runtime), so the plugin finds functions by
byte signature rather than by these numbers. KenshiLib's header RVAs match neither directly;
use `KenshiLib::GetRealAddress` for anything KenshiLib names.
