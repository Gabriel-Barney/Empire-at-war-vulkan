# Per-executable inventory of a build tree (size, ninja link step time, ctest runtime). bo2 t02.
# Usage: ctest --test-dir <tree> --show-only=json-v1 > <dir>/ctest.json; python tools/analysis/exe_inventory.py <dir> [tree]
# Writes <dir>/inv.json and prints one line per exe. Link times are wall time of the ninja step under load.
import json,os,re,sys,collections
sp=sys.argv[1]; b=sys.argv[2] if len(sys.argv)>2 else 'build-wt-msvc'
j=json.load(open(sp+'/ctest.json',encoding='utf-8'))
cost={}
for l in open(b+'/Testing/Temporary/CTestCostData.txt'):
    p=l.split()
    if len(p)==3 and p[1].isdigit(): cost[p[0]]=float(p[2])
link={}
for l in open(b+'/.ninja_log'):
    p=l.rstrip('\n').split('\t')
    if len(p)>=4 and p[3].startswith('bin/') and p[3].endswith('.exe'):
        link[os.path.basename(p[3])]=(int(p[1])-int(p[0]))/1000
exes={}
for f in os.listdir(b+'/bin'):
    if f.endswith('.exe'): exes[f]=dict(size=os.path.getsize(b+'/bin/'+f)/1e6,tests=[],rt=0.0,slow=False,link=link.get(f))
for t in j['tests']:
    cmd=t.get('command',[''])
    e=os.path.basename(cmd[0]) if cmd else ''
    if e in exes:
        exes[e]['tests'].append(t['name']); exes[e]['rt']+=cost.get(t['name'],0)
        if any('slow' in (p.get('value') or []) for p in t.get('properties',[]) if p['name']=='LABELS'): exes[e]['slow']=True
def mod(n):
    return n
rows=sorted(exes.items(), key=lambda kv:kv[0])
json.dump(exes,open(sp+'/inv.json','w'),indent=1)
print(len(rows),'exes',sum(1 for _,v in rows if v['tests']),'with tests')
print('total size MB',round(sum(v['size'] for _,v in rows)),' link s (known)',round(sum(v['link'] or 0 for _,v in rows),1), sum(1 for _,v in rows if v['link'] is None),'no link time')
for n,v in rows: print(f"{n[:-4]:42s} {v['size']:6.1f} MB link {v['link'] if v['link'] is not None else -1:5.1f}s tests {len(v['tests']):2d} rt {v['rt']:6.1f}s {'slow' if v['slow'] else ''}")
