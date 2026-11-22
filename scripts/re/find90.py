import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
exec(open('xref.py').read().split('tdata = d[')[0].replace("needles = sys.argv[3:]","needles=[]"))
md = Cs(CS_ARCH_X86, CS_MODE_64)
lo, hi = 0x4c8000, 0x4fa000
seen = set()
for f0, f1 in funcs:
    if not (lo <= f0 < hi): continue
    code = d[rva2off(f0):rva2off(f0) + (f1 - f0)]
    for ins in md.disasm(code, f0):
        if ins.mnemonic == 'mov' and ins.op_str.startswith('qword ptr [') and '+ 0x90]' in ins.op_str.split(',')[0] and 'rsp' not in ins.op_str and 'rbp' not in ins.op_str:
            print('%x in func %x-%x %s : %s %s' % (ins.address, f0, f1, names.get(f0, '?'), ins.mnemonic, ins.op_str))
