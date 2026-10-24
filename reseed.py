import json,os,re,bisect,sys
PROJ=os.path.dirname(os.path.abspath(__file__))
TK=os.path.join(PROJ,'xboxrecomp')
fl=json.load(open(os.path.join(TK,'tools','disasm','output','functions.json')))
v=lambda x:int(x,16) if isinstance(x,str) else x
fl=sorted((v(f['start']),v(f['end'])) for f in fl); starts=[a for a,b in fl]
stubs=[int(m,16) for m in re.findall(r'void sub_([0-9A-F]{8})\(void\)',open(os.path.join(PROJ,'src','recomp','gen','recomp_stubs_unresolved.c')).read())]
seedp=os.path.join(PROJ,'config','seed_functions.json')
seeds={v(s['start']) for s in json.load(open(seedp))}
gap=[a for a in stubs if not (lambda i:i>=0 and fl[i][0]<=a<fl[i][1])(bisect.bisect_right(starts,a)-1)]
new=set(stubs)-seeds
print('stubs',len(stubs),'gap',len(gap),'new seeds',len(new))
json.dump([{"start":"0x%08X"%a} for a in sorted(seeds|set(stubs))],open(seedp,'w'))
open(os.path.join(PROJ,'newseeds.txt'),'w').write(str(len(new)))
