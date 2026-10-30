"""Post-generation patches for mm3recomp. Run after tools.recomp, before the build.

Each patch is (file-glob, anchor text, replacement). The anchor must be found in exactly
one place; a missing anchor is reported (the generated code changed under us).
"""
import glob,os,sys,re
GEN=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),'src','recomp','gen','').replace('\\','/')
PATCHES=[
 # XDK DSOUND (sub_0027CC4F): divides by byte [ecx+0x64] before checking it for zero.
 # The real hardware path never sees zero here; our DSOUND state machine can leave it
 # at 0 for some buffers (see NOTES.md). Treat 0 as 1 so the divide cannot fault.
 ("loc_0027CCDA: ;\n    esi = ZX8(MEM8(ecx + 0x64));\n",
  "loc_0027CCDA: ;\n    esi = ZX8(MEM8(ecx + 0x64));\n    if (esi == 0) esi = 1; /* mm3recomp patch: avoid #DE, see patch_gen.py */\n"),
 # Pedestrian LOD (sub_000DB5BB): level 4+ (beyond 130 m) takes a rigid draw that lays the
 # body flat; draw it skinned like levels 1-3 instead. See mm3_ped_rigid_lod in
 # recomp_manual.c and NOTES.md 33; RECOMP_PED_RIGID=1 restores the original.
 ("/* cmp LO8(eax), 4 (8-bit) */\n    if (CMP_AE(_fa, _fb)) goto loc_000DB6F6; /* jae: above or equal (unsigned >=) */\n",
  "/* cmp LO8(eax), 4 (8-bit) */\n    { extern int mm3_ped_rigid_lod(void);\n      if (CMP_AE(_fa, _fb) && mm3_ped_rigid_lod()) goto loc_000DB6F6; } /* mm3recomp patch: LOD 4+ draws skinned (see patch_gen.py) */\n"),
 # D3D SetTextureStageState (sub_00341880): 'cmp edx, 0xC; jge 0x3418AF' lands on a jne
 # that reuses those flags, but 0x3418AF is split off as its own function whose flag
 # variables start at zero, so the jne never fired and every stage state >= 0xC was stored
 # as COLOROP. Swap's back-to-front blit then ran with COLOROP = DISABLE (diffuse, white)
 # instead of SELECTARG1(TEXTURE): the loading-screen image never reached the screen.
 # edx is unchanged since the cmp. See NOTES.md 37.
 ("loc_003418AF: ;\n    if (_flags /* jne: not equal / not zero */) { g_seh_ebp = ebp; sub_003418DC(); return; }\n",
  "loc_003418AF: ;\n    if (edx != 0xC) { g_seh_ebp = ebp; sub_003418DC(); return; } /* mm3recomp patch: flags of the cmp in sub_00341880 (see patch_gen.py) */\n"),
]
files=sorted(glob.glob(GEN+'recomp_*.c'))
ok=True
for anchor,repl in PATCHES:
    hits=[]
    for f in files:
        t=open(f,encoding='utf-8',errors='surrogateescape',newline='').read()
        a=anchor.replace('\n','\r\n') if '\r\n' in t[:2000] else anchor
        r=repl.replace('\n','\r\n') if '\r\n' in t[:2000] else repl
        if a in t: hits.append((f,t,a,r))
    if len(hits)!=1:
        print('PATCH ANCHOR FOUND IN',len(hits),'FILES:',anchor.splitlines()[0]); ok=False; continue
    f,t,a,r=hits[0]
    open(f,'w',encoding='utf-8',errors='surrogateescape',newline='').write(t.replace(a,r,1))
    print('patched',f.split('/')[-1])
sys.exit(0 if ok else 1)
