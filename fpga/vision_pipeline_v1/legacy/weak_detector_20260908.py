"""Native-pixel, multiscale low-contrast proposals; no learned model."""
import cv2
import numpy as np
from scipy.ndimage import median_filter

SCALES=(1.0,1.6,2.5,4.0,6.0,9.0,13.0)

def noise_map(response, valid, step=128):
    h,w=response.shape
    grid=np.empty(((h+step-1)//step,(w+step-1)//step),np.float32)
    values=response[valid][::8]
    fallback=max(.06,float(np.median(np.abs(values-np.median(values)))*1.4826)) if len(values) else .5
    for iy,y in enumerate(range(0,h,step)):
        for ix,x in enumerate(range(0,w,step)):
            r=response[y:y+step:2,x:x+step:2];m=valid[y:y+step:2,x:x+step:2]
            z=r[m]
            grid[iy,ix]=max(.06,.6*fallback,float(np.median(np.abs(z-np.median(z)))*1.4826)) if z.size>=128 else fallback
    return cv2.resize(grid,(w,h),interpolation=cv2.INTER_LINEAR)

def detect_tile(a, valid=None, min_z=3.8):
    a=a.astype(np.float32)
    if valid is None:valid=np.ones(a.shape,bool)
    if not valid.any():return []
    found=[]
    for si,s in enumerate(SCALES):
        response=cv2.GaussianBlur(a,(0,0),s)-cv2.GaussianBlur(a,(0,0),s*2.2)
        noise=noise_map(response,valid)
        norm=np.abs(response)/noise
        # Hysteresis components integrate weak neighboring pixels; strong pixel
        # amplitude is not required as in the original detector.
        mask=(norm>=2.35)&(np.abs(response)>=.25)&valid
        for polarity in (-1,1):
            n,labels,stats,centers=cv2.connectedComponentsWithStats((mask&(response*polarity>0)).astype(np.uint8),8)
            for k in range(1,n):
                x,y,w,h,area=stats[k]
                if area<max(3,int(s*s*.45)) or max(w,h)>max(18,10*s):continue
                if max(w,h)>max(12,min(w,h)*7):continue
                rr=norm[y:y+h,x:x+w];m=labels[y:y+h,x:x+w]==k
                p=np.where(m,rr,-1);py,px=np.unravel_index(p.argmax(),p.shape)
                z=float(p[py,px]);amp=float(abs(response[y+py,x+px]))
                if z<min_z or amp<.45:continue
                cx,cy=int(x+px),int(y+py);radius=int(np.ceil(4*s))
                lx,ly=max(0,cx-radius),max(0,cy-radius);rx,ry=min(a.shape[1],cx+radius+1),min(a.shape[0],cy+radius+1)
                yy,xx=np.ogrid[ly:ry,lx:rx];dist=(xx-cx)**2+(yy-cy)**2
                core=dist<=max(1,.8*s)**2;annulus=(dist>=(2.5*s)**2)&(dist<=(4*s)**2)
                patch=a[ly:ry,lx:rx]
                if not annulus.any():continue
                contrast=float((np.mean(patch[core])-np.median(patch[annulus]))*polarity)
                # Reject convolution halos that have no corresponding raw-image
                # center-versus-background contrast.
                if contrast<.6:continue
                found.append(dict(bbox=[int(x),int(y),int(x+w),int(y+h)],center=[int(x+px),int(y+py)],scale=s,scale_id=si,
                    z=z,amplitude_dn=amp,contrast_dn=contrast,noise_dn=float(noise[y+py,x+px]),area=int(area),polarity=polarity))
    return found

def consolidate(candidates):
    # Adjacent scales must agree spatially, or a single scale must be very strong.
    cells={}
    for i,r in enumerate(candidates):
        x,y=r['center'];cells.setdefault((x//32,y//32),[]).append(i)
    accepted=[]
    for i,r in enumerate(candidates):
        x,y=r['center'];others=[]
        for a in range(x//32-2,x//32+3):
            for b in range(y//32-2,y//32+3):others.extend(cells.get((a,b),[]))
        support={r['scale_id']}
        for j in others:
            q=candidates[j]
            if abs(q['scale_id']-r['scale_id'])>1 or q['polarity']!=r['polarity']:continue
            d=np.hypot(x-q['center'][0],y-q['center'][1])
            if d<=max(3,1.5*min(r['scale'],q['scale'])):support.add(q['scale_id'])
        if len(support)<2 and r['z']<6:continue
        r={**r,'scale_support':len(support),'kind':'low_contrast' if r['z']>=4.5 and len(support)>=2 else 'sensitive'}
        accepted.append(r)
    kept=[];index={}
    for r in sorted(accepted,key=lambda v:-v['z']):
        x,y=r['center'];near=[]
        for a in range(x//32-2,x//32+3):
            for b in range(y//32-2,y//32+3):near.extend(index.get((a,b),[]))
        duplicate=False
        for j in near:
            q=kept[j];u,v=r['bbox'],q['bbox']
            inter=max(0,min(u[2],v[2])-max(u[0],v[0]))*max(0,min(u[3],v[3])-max(u[1],v[1]))
            union=(u[2]-u[0])*(u[3]-u[1])+(v[2]-v[0])*(v[3]-v[1])-inter
            if inter/max(1,union)>.3 or np.hypot(x-q['center'][0],y-q['center'][1])<=max(2,min(r['scale'],q['scale'])):
                duplicate=True;break
        if not duplicate:index.setdefault((x//32,y//32),[]).append(len(kept));kept.append(r)
    return kept

def geometry(a):
    h,w=a.shape
    small=cv2.resize(a,(w//4,h//4),interpolation=cv2.INTER_AREA)
    bright=small[:,:small.shape[1]//2]>220
    edge=bright.shape[1]-1-np.argmax(bright[:,::-1],axis=1)
    edge[~bright.any(axis=1)]=0
    edge=median_filter(edge,size=31)*4
    edge=np.repeat(edge,4)[:h]
    if len(edge)<h:edge=np.pad(edge,(0,h-len(edge)),mode='edge')
    col=np.median(a[::8],axis=0).astype(np.float32)
    stripe=col-cv2.GaussianBlur(col.reshape(1,-1),(0,0),60).ravel()
    return edge,stripe
