import numpy as np, warnings, pickle, sys
warnings.filterwarnings('ignore')
from nowcast import *
net,mu,sd=pickle.load(open(sys.argv[1] if len(sys.argv)>1 else 'net.pkl','rb'))
def ml_step(prev,now,vy,vx):
    F,adv=features(prev,now,vy,vx)
    ok=np.all(np.isfinite(F),-1)
    out=adv.copy()
    Fs=(F[ok]-mu)/sd
    out[ok]=adv[ok]+net.predict(Fs)
    return out
def roll(prev,now,vy,vx,n,kind):
    for _ in range(n):
        if kind=='persist': nxt=now
        elif kind=='adv': nxt=advect(now,vy,vx,1)
        elif kind=='trend':
            F,adv=features(prev,now,vy,vx); nxt=adv+np.nan_to_num(F[...,3]*0.5)   # half the Lagrangian tendency
        else: nxt=ml_step(prev,now,vy,vx)
        prev,now=now,nxt
    return now
def score(pred,obs,ref):
    m=np.isfinite(obs)&np.isfinite(ref)&(RR<0.9)
    for p in pred.values(): m&=np.isfinite(p)
    out={}
    for k,p in pred.items():
        r=np.sqrt(np.mean((p[m]-obs[m])**2)); A=p[m]<240; O=obs[m]<240
        out[k]=(r,(A&O).sum()/max(1,(A|O).sum()))
    return out,m.sum()
cases=[('test +10 min (05:20,05:30 -> 05:40)','20261007_052006','20261007_053006','20261007_054006',1,1),
       ('+30 min (03:30,03:50 -> 04:20)','20261007_033006','20261007_035006','20261007_042006',3,2),
       ('+40 min (04:30,04:40 -> 05:20)','20261007_043006','20261007_044006','20261007_052006',4,1)]
for name,p,n,o,steps,gap in cases:
    a,b,c=load(p),load(n),load(o)
    vy,vx,_=motion(a,b); vy/=gap; vx/=gap     # per 10 minutes
    prev=a if gap==1 else advect(b,-vy,-vx,1) # the 10-min-earlier field when only a 20-min one exists
    preds={k:roll(prev,b,vy,vx,steps,k) for k in ['persist','adv','trend','ml']}
    s,npx=score(preds,c,b)
    print(name,'pixels',npx); [print('   %-8s RMSE %.3f K   CSI(<240K) %.3f'%(k,v[0],v[1])) for k,v in s.items()]
