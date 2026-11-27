import numpy as np, warnings, pickle, time
warnings.filterwarnings('ignore')
from nowcast import *
from sklearn.neural_network import MLPRegressor
from export import export
rng=np.random.default_rng(0)
trip=[('20261007_042006','20261007_043006','20261007_044006'),('20261007_032006','20261007_033006','20261007_034006'),('20261007_033006','20261007_034006','20261007_035006'),('20261007_052006','20261007_053006','20261007_054006')]
X=[];Y=[]
for p,n,x in trip:
    a,b,c=load(p),load(n),load(x); vy,vx,_=motion(a,b); F,adv=features(a,b,vy,vx)
    m=np.all(np.isfinite(F),-1)&np.isfinite(c)&(RR<0.95); idx=np.flatnonzero(m.ravel()); sel=rng.choice(idx,min(len(idx),150000),replace=False)
    X.append(F.reshape(-1,F.shape[-1])[sel]); Y.append((c-adv).ravel()[sel])
X=np.concatenate(X); Y=np.concatenate(Y); mu=X.mean(0); sd=X.std(0)+1e-6
net=MLPRegressor(hidden_layer_sizes=(24,24),activation='relu',alpha=1e-4,batch_size=1024,learning_rate_init=2e-3,max_iter=40,early_stopping=True,validation_fraction=0.1,n_iter_no_change=6,random_state=0)
net.fit((X-mu)/sd,Y); print('final iters',net.n_iter_,'val',net.best_validation_score_)
pickle.dump((net,mu,sd),open('net_final.pkl','wb'))
export(net,mu,sd,'nowcast_v1.txt','MGAS nowcast v1, GK-2A IR10.5, trained on 4 triples 2026-10-07 (03:20-05:40 UTC)')
