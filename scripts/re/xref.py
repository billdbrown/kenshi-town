import struct, sys, re, glob, os
exe = sys.argv[1]; hdrdir = sys.argv[2]; needles = sys.argv[3:]
d = open(exe, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
optsz = struct.unpack_from('<H', d, pe + 20)[0]
opt = pe + 24
imagebase = struct.unpack_from('<Q', d, opt + 24)[0]
datadir = opt + 112
exc_rva, exc_sz = struct.unpack_from('<II', d, datadir + 3 * 8)
secs = []
for i in range(nsec):
    o = opt + optsz + i * 40
    name = d[o:o+8].rstrip(b'\0'); vsz, va, rsz, raw = struct.unpack_from('<IIII', d, o + 8)
    secs.append((name, va, vsz, raw, rsz))
def off2rva(off):
    for n, va, vsz, raw, rsz in secs:
        if raw <= off < raw + rsz: return va + off - raw
def rva2off(rva):
    for n, va, vsz, raw, rsz in secs:
        if va <= rva < va + max(vsz, rsz): return raw + rva - va
text = [s for s in secs if s[0] == b'.text'][0]
# function table
funcs = []
eo = rva2off(exc_rva)
for i in range(exc_sz // 12):
    b, e, u = struct.unpack_from('<III', d, eo + i * 12); funcs.append((b, e))
funcs.sort()
def func_of(rva):
    lo, hi = 0, len(funcs) - 1; best = None
    while lo <= hi:
        m = (lo + hi) // 2
        if funcs[m][0] <= rva: best = funcs[m]; lo = m + 1
        else: hi = m - 1
    return best
# KenshiLib RVA names
names = {}
for h in glob.glob(os.path.join(hdrdir, '**', '*.h'), recursive=True):
    cls = None
    for line in open(h, encoding='utf-8', errors='replace'):
        mc = re.match(r'\s*(class|struct)\s+(\w+)', line)
        if mc and '(' not in line and ';' not in line: cls = mc.group(2)
        m = re.search(r'RVA = 0x([0-9A-Fa-f]+)', line)
        if m:
            sig = line.split('//')[0].strip()
            names.setdefault(int(m.group(1), 16), '%s :: %s' % (cls, sig))
tdata = d[text[3]:text[3] + text[4]]
for needle in needles:
    off = d.find(needle.encode())
    srva = off2rva(off)
    print('== %r at rva 0x%x' % (needle, srva))
    # scan for rip-relative disp32 pointing at srva (lea/mov with 7-byte forms: disp at insn+3, next insn at +7)
    for i in range(len(tdata) - 4):
        disp = struct.unpack_from('<i', tdata, i)[0]
        insn_next = text[1] + i + 4
        if insn_next + disp == srva:
            ref = text[1] + i
            f = func_of(ref)
            fn = names.get(f[0], '?') if f else '?'
            print('  ref at 0x%x in func 0x%x-0x%x  %s' % (ref, f[0], f[1], fn) if f else '  ref 0x%x' % ref)
