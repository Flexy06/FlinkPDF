# Erzeugt res/app.ico (wird von build.sh automatisch aufgerufen)
from PIL import Image, ImageDraw
S=1024
def lerp(a,b,t): return tuple(int(a[i]+(b[i]-a[i])*t) for i in range(3))
c1=(255,110,61); c2=(224,31,74)
grad=Image.new("RGBA",(S,S))
px=grad.load()
for y in range(S):
    for x in range(S):
        t=(x+y)/(2*S-2); px[x,y]=lerp(c1,c2,t)+(255,)
mask=Image.new("L",(S,S),0)
ImageDraw.Draw(mask).rounded_rectangle([0,0,S-1,S-1],radius=int(S*0.23),fill=255)
img=Image.new("RGBA",(S,S),(0,0,0,0)); img.paste(grad,(0,0),mask)
d=ImageDraw.Draw(img)
k=S/100
P=lambda pts:[(x*k,y*k) for x,y in pts]
d.polygon(P([(29,17),(59,17),(74,32),(74,83),(29,83)]),fill=(255,255,255,255))
d.polygon(P([(59,17),(74,32),(59,32)]),fill=(255,210,198,255))
bolt=Image.new("L",(S,S),0)
ImageDraw.Draw(bolt).polygon(P([(56,28),(39,55),(50.5,55),(45,74),(63,45),(51.5,45),(58.5,28)]),fill=255)
img.paste(grad,(0,0),bolt)
sizes=[16,20,24,32,40,48,64,96,128,256]
img.resize((256,256),Image.LANCZOS).save("app.ico",sizes=[(s,s) for s in sizes])
