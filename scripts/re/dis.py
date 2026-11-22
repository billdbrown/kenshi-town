import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
exec(open('xref.py').read().split('tdata = d[')[0].replace("needles = sys.argv[3:]","needles=[]"))
lo, hi = int(sys.argv[3],16), int(sys.argv[4],16)
md = Cs(CS_ARCH_X86, CS_MODE_64)
code = d[rva2off(lo):rva2off(lo) + (hi - lo)]
strs = {}
for ins in md.disasm(code, lo):
    extra = ''
    if ins.mnemonic == 'call' and ins.op_str.startswith('0x'):
        t = int(ins.op_str, 16); extra = names.get(t, '')
        if not extra:
            f = func_of(t); extra = ('in ' + names.get(f[0], '?') + ' @0x%x' % f[0]) if f else ''
    if 'rip' in ins.op_str:
        import re
        m = re.search(r'rip \+ (0x[0-9a-f]+)', ins.op_str)
        if m:
            tgt = ins.address + ins.size + int(m.group(1), 16); o = rva2off(tgt)
            if o:
                s = d[o:o+60].split(b'\0')[0]
                if len(s) > 4 and all(32 <= c < 127 for c in s): extra = repr(s.decode())
                else: extra = names.get(tgt, '')
    print('%x  %-6s %-40s %s' % (ins.address, ins.mnemonic, ins.op_str, extra))
