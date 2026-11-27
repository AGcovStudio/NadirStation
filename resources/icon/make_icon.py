# NadirStation Station icon: graphite tile, Earth disk with a terminator curve, a thin geostationary orbit
# arc and the satellite on it. Drawn at 1024 px and reduced; the small sizes use heavier strokes.
from PIL import Image, ImageDraw
import math
BG=(30,31,34); DISK=(47,74,99); EDGE=(74,143,208); ORBIT=(163,168,176); SAT=(236,238,241)
def draw(N, heavy=False):
    S=N*4
    im=Image.new('RGBA',(S,S),(0,0,0,0)); d=ImageDraw.Draw(im)
    r=int(S*0.22)
    d.rounded_rectangle([0,0,S-1,S-1],radius=r,fill=BG)
    k=S/1024
    w=lambda x:max(1,int(x*k*(1.8 if heavy else 1)))
    # Earth disk
    cx,cy,R=440*k,590*k,300*k
    d.ellipse([cx-R,cy-R,cx+R,cy+R],fill=DISK,outline=EDGE,width=w(18))
    # terminator / limb curve inside the disk
    box=[cx-R*0.45,cy-R,cx+R*0.45,cy+R]
    d.arc(box,start=-90,end=90,fill=EDGE,width=w(12))
    # orbit arc around the disk
    Ro=440*k
    d.arc([cx-Ro,cy-Ro,cx+Ro,cy+Ro],start=-80,end=10,fill=ORBIT,width=w(10))
    # satellite on the orbit
    a=math.radians(-35); sx,sy=cx+Ro*math.cos(a),cy+Ro*math.sin(a); sr=46*k*(1.3 if heavy else 1)
    d.ellipse([sx-sr,sy-sr,sx+sr,sy+sr],fill=SAT)
    # downlink: two short arcs from the satellite towards the disk
    if not heavy:
        for i,rad in enumerate((95,150)):
            rr=rad*k
            d.arc([sx-rr,sy-rr,sx+rr,sy+rr],start=118,end=172,fill=EDGE,width=w(12))
    return im.resize((N,N),Image.LANCZOS)
sizes=[16,24,32,48,64,128,256,512,1024]
imgs={n:draw(n,heavy=n<=32) for n in sizes}
for n,im in imgs.items(): im.save(f'nadirstation_icon_{n}.png')
imgs[256].save('nadirstation.ico',sizes=[(n,n) for n in (16,24,32,48,64,128,256)],append_images=[imgs[n] for n in (16,24,32,48,64,128)])
# preview strip
strip=Image.new('RGBA',(16+24+32+48+64+128+256+8*9,270),(236,238,241,255))
x=8
for n in (16,24,32,48,64,128,256):
    strip.paste(imgs[n],(x,262-n),imgs[n]); x+=n+8
strip.save('preview.png')
print('ok')
