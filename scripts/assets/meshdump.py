import struct,sys
d=open(sys.argv[1],'rb').read()
p=0
def u16():
    global p; v=struct.unpack_from('<H',d,p)[0]; p+=2; return v
def u32():
    global p; v=struct.unpack_from('<I',d,p)[0]; p+=4; return v
def s():
    global p; e=d.index(b'\n',p); v=d[p:e]; p=e+1; return v
assert u16()==0x1000; print('header',s())
NAMES={0x3000:'MESH',0x4000:'SUBMESH',0x4010:'SUBMESH_OP',0x5000:'GEOMETRY',0x5100:'VDECL',0x5110:'VELEM',0x5200:'VBUF',0x5210:'VBUFDATA',0xD000:'BOUNDS',0xA000:'SUBNAMES',0xA100:'SUBNAME',0x8000:'MESH_LOD',0xE000:'EDGES',0xB000:'EXTREMES',0x6000:'SKEL'}
def walk(end,depth):
    global p
    while p<end:
        start=p; cid=u16(); ln=u32(); cend=start+ln
        n=NAMES.get(cid,hex(cid))
        info=''
        if cid==0x3000:
            info='skel=%d'%d[p]; p+=1; print('  '*depth+n,ln,info); walk(cend,depth+1); continue
        if cid==0x4000:
            mat=s(); shared=d[p]; p+=1; ic=u32(); i32=d[p]; p+=1
            p+=ic*(4 if i32 else 2)
            print('  '*depth+n,ln,mat,'shared',shared,'idx',ic,'32bit',i32); walk(cend,depth+1); continue
        if cid==0x5000:
            print('  '*depth+n,ln,'verts',u32()); walk(cend,depth+1); continue
        if cid==0x5100:
            print('  '*depth+n,ln); walk(cend,depth+1); continue
        if cid==0x5110:
            src,typ,sem,off,idx=struct.unpack_from('<HHHHH',d,p); print('  '*depth+n,'src',src,'type',typ,'sem',sem,'off',off,'idx',idx); p=cend; continue
        if cid==0x5200:
            bi,vs=struct.unpack_from('<HH',d,p); p+=4; print('  '*depth+n,ln,'bind',bi,'vsize',vs); walk(cend,depth+1); continue
        if cid==0x4010:
            print('  '*depth+n,ln,'op',u16()); p=cend; continue
        if cid==0xD000:
            print('  '*depth+n,ln,struct.unpack_from('<7f',d,p)); p=cend; continue
        if cid==0xA000:
            print('  '*depth+n,ln); walk(cend,depth+1); continue
        if cid==0xA100:
            print('  '*depth+n,u16(),s()); p=cend; continue
        print('  '*depth+n,ln, struct.unpack_from('<7f',d,p) if cid==0x9000 else ''); p=cend
walk(len(d),0)
