CY=128.0
# bold, simplified rainbow spectrum for tiny sizes (no plugs -> survives 16px)
LOBES=[("#7C3AED",44),("#EC2D8A",64),("#EF4444",50),("#FB7115",82),
       ("#34C24E",54),("#08BCD4",78),("#3B82F6",46)]
X0,X1=54.0,202.0
HW=13.0
def lobe(cx,h,w):
    t,b=CY-h,CY+h
    return f"M{cx:.1f},{t:.1f} Q{cx+w:.1f},{CY:.1f} {cx:.1f},{b:.1f} Q{cx-w:.1f},{CY:.1f} {cx:.1f},{t:.1f} Z"
n=len(LOBES); cs=[X0+(X1-X0)*i/(n-1) for i in range(n)]
glow=[]; solid=[]
for (c,h),cx in zip(LOBES,cs):
    glow.append(f'<path d="{lobe(cx,h*1.08,HW*1.7)}" fill="{c}" opacity="0.5"/>')
    solid.append(f'<path d="{lobe(cx,h,HW)}" fill="{c}"/>')
svg=f'''<svg width="256" height="256" viewBox="0 0 256 256" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#152741"/><stop offset="1" stop-color="#0A1120"/>
    </linearGradient>
    <filter id="glow" x="-40%" y="-40%" width="180%" height="180%"><feGaussianBlur stdDeviation="5"/></filter>
  </defs>
  <rect x="8" y="8" width="240" height="240" rx="52" fill="url(#bg)"/>
  <g filter="url(#glow)">{"".join(chr(10)+"    "+g for g in glow)}
  </g>
  <g>{"".join(chr(10)+"    "+s for s in solid)}
  </g>
</svg>
'''
open("B2_small.svg","w",encoding="utf-8").write(svg)
print("wrote B2_small.svg")
