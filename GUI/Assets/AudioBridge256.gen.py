#!/usr/bin/env python3
"""Generate the refined 'Jack Bridge' logo: two 1/4" guitar plugs + rainbow spectrum wave."""

CY = 128.0

# rainbow spectrum: (color, half-height) left->right, two tall peaks (orange, cyan)
LOBES = [
    ("#7C3AED", 24),  # purple
    ("#C026D3", 36),  # magenta
    ("#EC2D8A", 46),  # pink
    ("#EF4444", 54),  # red
    ("#FB7115", 74),  # orange  <- peak
    ("#F7C400", 40),  # yellow
    ("#34C24E", 48),  # green
    ("#0FB894", 36),  # teal
    ("#08BCD4", 70),  # cyan    <- peak
    ("#3B82F6", 46),  # blue
    ("#5B6EE6", 28),  # indigo
]

X0, X1 = 88.0, 168.0   # span of lobe centres (between the two gold tips)
HALFW = 6.6

def lobe_path(cx, h, w):
    top, bot = CY - h, CY + h
    return (f"M{cx:.1f},{top:.1f} "
            f"Q{cx+w:.1f},{CY:.1f} {cx:.1f},{bot:.1f} "
            f"Q{cx-w:.1f},{CY:.1f} {cx:.1f},{top:.1f} Z")

n = len(LOBES)
centres = [X0 + (X1 - X0) * i / (n - 1) for i in range(n)]

glow_paths, solid_paths = [], []
for (col, h), cx in zip(LOBES, centres):
    glow_paths.append(f'<path d="{lobe_path(cx, h*1.10, HALFW*1.7)}" fill="{col}" opacity="0.5"/>')
    solid_paths.append(f'<path d="{lobe_path(cx, h, HALFW)}" fill="{col}"/>')

# ---- one 1/4" guitar plug, pointing right, centred on CY. Drawn in its own group
# so the right one is a mirror. Chrome barrel + knurl + shaft + black ring + gold tip.
def plug():
    # Slender 1/4" TS plug, pointing right, centred on y=128.
    # Long thin chrome barrel (runs off the tile edge), narrow sleeve, gold tip.
    BT, BB = 117, 139          # barrel top / bottom (height 22, thin)
    p = []
    # barrel body
    p.append(f'<rect x="-16" y="{BT}" width="68" height="{BB-BT}" rx="7" fill="url(#chrome)"/>')
    p.append(f'<rect x="-16" y="{BT}" width="68" height="{BB-BT}" rx="7" fill="none" stroke="#2A3346" stroke-width="1.3" opacity="0.6"/>')
    # two knurl bands (diamond crosshatch), clipped to bands on the barrel
    for band in ("knurlA", "knurlB"):
        p.append(f'<g clip-path="url(#{band})">')
        for x in range(-24, 56, 4):
            p.append(f'<line x1="{x}" y1="{BT-2}" x2="{x+22}" y2="{BB+2}" stroke="#39424f" stroke-width="1.2" opacity="0.6"/>')
            p.append(f'<line x1="{x+22}" y1="{BT-2}" x2="{x}" y2="{BB+2}" stroke="#39424f" stroke-width="1.2" opacity="0.6"/>')
            p.append(f'<line x1="{x+1}" y1="{BT-2}" x2="{x+23}" y2="{BB+2}" stroke="#f2f6fa" stroke-width="0.7" opacity="0.3"/>')
        p.append('</g>')
    # collar step
    p.append(f'<rect x="51" y="120" width="5" height="16" rx="2" fill="url(#chrome)" stroke="#2A3346" stroke-width="1"/>')
    # shaft / sleeve (thin)
    p.append(f'<rect x="55" y="123" width="12" height="10" rx="4" fill="url(#chromeShaft)" stroke="#2A3346" stroke-width="1"/>')
    # black insulator ring
    p.append(f'<rect x="65.5" y="123" width="3" height="10" rx="1.2" fill="#141821"/>')
    # chrome tip base
    p.append(f'<rect x="68" y="124" width="4" height="8" rx="2" fill="url(#chromeShaft)" stroke="#2A3346" stroke-width="0.8"/>')
    # gold conical tip (bullet)
    p.append('<path d="M71 124 Q80 124 84 128 Q80 132 71 132 Z" fill="url(#gold)" stroke="#8a5a12" stroke-width="0.9"/>')
    # tiny specular highlight on tip
    p.append('<ellipse cx="76" cy="126.5" rx="2.4" ry="0.9" fill="#fffbe8" opacity="0.5"/>')
    return "\n      ".join(p)

svg = f'''<svg width="256" height="256" viewBox="0 0 256 256" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#132038"/>
      <stop offset="1" stop-color="#0A1120"/>
    </linearGradient>
    <linearGradient id="chrome" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#eef3f8"/>
      <stop offset="0.16" stop-color="#c3ccd8"/>
      <stop offset="0.42" stop-color="#8b97a6"/>
      <stop offset="0.5" stop-color="#f6f9fc"/>
      <stop offset="0.6" stop-color="#8b97a6"/>
      <stop offset="0.85" stop-color="#5c6675"/>
      <stop offset="1" stop-color="#454e5b"/>
    </linearGradient>
    <linearGradient id="chromeShaft" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#f2f6fa"/>
      <stop offset="0.5" stop-color="#aab6c4"/>
      <stop offset="0.52" stop-color="#f6f9fc"/>
      <stop offset="1" stop-color="#5c6675"/>
    </linearGradient>
    <linearGradient id="gold" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#fef3c7"/>
      <stop offset="0.32" stop-color="#fcd34d"/>
      <stop offset="0.5" stop-color="#fffbe6"/>
      <stop offset="0.62" stop-color="#f59e0b"/>
      <stop offset="1" stop-color="#b45309"/>
    </linearGradient>
    <clipPath id="knurlA"><rect x="-2" y="117" width="14" height="22"/></clipPath>
    <clipPath id="knurlB"><rect x="28" y="117" width="16" height="22"/></clipPath>
    <clipPath id="tile"><rect x="8" y="8" width="240" height="240" rx="56"/></clipPath>
    <filter id="glow" x="-40%" y="-40%" width="180%" height="180%">
      <feGaussianBlur stdDeviation="4"/>
    </filter>
  </defs>

  <rect x="8" y="8" width="240" height="240" rx="56" fill="url(#bg)"/>

  <!-- rainbow spectrum soundwave -->
  <g filter="url(#glow)">
    {"".join(chr(10)+"    "+g for g in glow_paths)}
  </g>
  <g>
    {"".join(chr(10)+"    "+s for s in solid_paths)}
  </g>

  <!-- left guitar plug (points right) -->
  <g clip-path="url(#tile)">
      {plug()}
  </g>
  <!-- right guitar plug (mirror) -->
  <g clip-path="url(#tile)" transform="translate(256,0) scale(-1,1)">
      {plug()}
  </g>
</svg>
'''

with open("B2_jackbridge.svg", "w", encoding="utf-8") as f:
    f.write(svg)
print("wrote B2_jackbridge.svg", len(svg), "bytes")
