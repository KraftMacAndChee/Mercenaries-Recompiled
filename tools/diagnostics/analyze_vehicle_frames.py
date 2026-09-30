"""Summarize completed-frame vehicle/camera timing without inferring image content."""
import argparse,gzip,json,collections
import numpy as np
p=argparse.ArgumentParser();p.add_argument('trace');p.add_argument('--last-seconds',type=float,default=30);p.add_argument('--output');a=p.parse_args()
d=json.load(gzip.open(a.trace,'rt'));s=d['samples'];assert s
end=s[-1]['qpc']/s[-1]['frequency'];s=[x for x in s if x['qpc']/x['frequency']>=end-a.last_seconds]
t=np.array([x['qpc']/x['frequency'] for x in s]);ticks=np.array([x['ticks'] for x in s]);h=np.array([x['hull'][12:15] for x in s]);c=np.array([x['rendered'][12:15] for x in s]);dt=np.diff(t);dh=np.diff(h,axis=0);dc=np.diff(c,axis=0)
def pct(x):return np.percentile(x,[0,1,50,95,99,100]).tolist()
result={'trace':a.trace,'sha256':d['sha256'],'samples':len(s),'seconds':float(t[-1]-t[0]),'percentile_order':[0,1,50,95,99,100],'frame_intervals_ms':pct(dt*1000),'game_tick_intervals_ms':pct(np.diff(ticks)/3),'duplicate_game_ticks':int(sum(np.diff(ticks)==0)),'duplicate_hull':int(sum(np.linalg.norm(dh,axis=1)<1e-7)),'duplicate_camera':int(sum(np.linalg.norm(dc,axis=1)<1e-7)),'sources':dict(collections.Counter(x.get('source',0) for x in s)),'hull_range':np.ptp(h,axis=0).tolist(),'relative_camera_range':np.ptp(c-h,axis=0).tolist()}
print(json.dumps(result,indent=2))
if a.output:open(a.output,'w').write(json.dumps(result,indent=2))
