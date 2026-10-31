import math, random
random.seed(7)
W, H = 1600, 900
cx, cy = 800, 400

def env(t):  # waveform envelope 0..1 for t in 0..1
    return (0.25 + 0.75 * abs(math.sin(t * math.pi * 3.2)) ** 1.5) * math.sin(t * math.pi) ** 0.6

# Incoming waveform (left of blade): dense oscillation
pts = []
for i in range(0, 470):
    x = 40 + i
    t = i / 470
    a = 120 * (0.35 + 0.65 * abs(math.sin(t * 9.0))) * (0.5 + 0.5 * t)
    y = cy + a * math.sin(i * 0.23) * math.sin(i * 0.031 + 1)
    pts.append(f"{x:.1f},{y:.1f}")
wave = "M" + " L".join(pts)

# Outgoing spectrum (right of blade): bars mirrored around cy
bars = []
for i in range(46):
    x = 1065 + i * 11.5
    f = i / 45
    h = 150 * (0.25 + 0.75 * abs(math.sin(f * 7 + 0.6))) * math.exp(-f * 1.6) + random.uniform(4, 22)
    hue = 300 - 120 * f
    bars.append(f'<rect x="{x:.1f}" y="{cy - h:.1f}" width="6" height="{2*h:.1f}" rx="3" fill="hsl({hue:.0f},95%,62%)" opacity="{0.95 - 0.5*f:.2f}"/>')

# Blade body: double-edge razor, 600 x 250, notched ends
bw, bh = 600, 250
L, R, T, B = cx - bw/2, cx + bw/2, cy - bh/2, cy + bh/2
n = 34  # end notch radius
c = 22  # corner chamfer
blade = (f"M{L+c},{T} L{R-c},{T} L{R},{T+c} L{R},{cy-n} A{n},{n} 0 0 0 {R},{cy+n} "
         f"L{R},{B-c} L{R-c},{B} L{L+c},{B} L{L},{B-c} L{L},{cy+n} A{n},{n} 0 0 0 {L},{cy-n} L{L},{T+c} Z")

# Central slot shaped as a mirrored waveform, with round holes at each end
sl, sr = cx - 210, cx + 210
top, bot = [], []
for i in range(0, 141):
    t = i / 140
    x = sl + t * (sr - sl)
    a = 6 + 44 * env(t) * (0.55 + 0.45 * abs(math.sin(i * 0.9)))
    top.append(f"{x:.1f},{cy - a:.1f}")
    bot.append(f"{x:.1f},{cy + a:.1f}")
slot = "M" + " L".join(top) + " L" + " L".join(reversed(bot)) + " Z"
holes = f"M{sl-30},{cy} a16,16 0 1 0 32,0 a16,16 0 1 0 -32,0 Z M{sr-2},{cy} a16,16 0 1 0 32,0 a16,16 0 1 0 -32,0 Z"

svg = f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}">
  <defs>
    <radialGradient id="bg" cx="50%" cy="42%" r="75%">
      <stop offset="0" stop-color="#1a1530"/><stop offset="0.55" stop-color="#0b0a14"/><stop offset="1" stop-color="#040408"/>
    </radialGradient>
    <linearGradient id="steel" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#f4f6fb"/><stop offset="0.28" stop-color="#9aa3b5"/>
      <stop offset="0.5" stop-color="#e9edf5"/><stop offset="0.72" stop-color="#6f7789"/><stop offset="1" stop-color="#c9cfdb"/>
    </linearGradient>
    <linearGradient id="spectral" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#00e5ff"/><stop offset="0.5" stop-color="#7a5cff"/><stop offset="1" stop-color="#ff2bd6"/>
    </linearGradient>
    <linearGradient id="edge" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#00e5ff" stop-opacity="0"/><stop offset="0.2" stop-color="#00e5ff"/>
      <stop offset="0.8" stop-color="#ff2bd6"/><stop offset="1" stop-color="#ff2bd6" stop-opacity="0"/>
    </linearGradient>
    <linearGradient id="sheen" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0.3" stop-color="#fff" stop-opacity="0"/><stop offset="0.45" stop-color="#fff" stop-opacity="0.55"/>
      <stop offset="0.5" stop-color="#fff" stop-opacity="0"/>
    </linearGradient>
    <filter id="glow" x="-20%" y="-50%" width="140%" height="200%">
      <feGaussianBlur stdDeviation="6" result="b"/><feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge>
    </filter>
    <filter id="shadow" x="-20%" y="-30%" width="140%" height="170%">
      <feDropShadow dx="0" dy="18" stdDeviation="18" flood-color="#000" flood-opacity="0.7"/>
    </filter>
    <clipPath id="bladeClip"><path d="{blade}"/></clipPath>
  </defs>

  <rect width="{W}" height="{H}" fill="url(#bg)"/>
  <g stroke="#ffffff" stroke-opacity="0.04">
    {''.join(f'<line x1="{x}" y1="0" x2="{x}" y2="{H}"/>' for x in range(0, W, 40))}
    {''.join(f'<line x1="0" y1="{y}" x2="{W}" y2="{y}"/>' for y in range(0, H, 40))}
  </g>

  <path d="{wave}" fill="none" stroke="#00e5ff" stroke-width="2.4" filter="url(#glow)" opacity="0.9"/>
  <g filter="url(#glow)">{''.join(bars)}</g>

  <g transform="rotate(-8 {cx} {cy})">
    <g filter="url(#shadow)">
      <path d="{blade} {slot} {holes}" fill="url(#steel)" fill-rule="evenodd"/>
    </g>
    <g clip-path="url(#bladeClip)">
      <rect x="{L}" y="{T}" width="{bw}" height="{bh}" fill="url(#sheen)"/>
      <rect x="{L}" y="{T}" width="{bw}" height="16" fill="#ffffff" opacity="0.35"/>
      <rect x="{L}" y="{B-16}" width="{bw}" height="16" fill="#1d2130" opacity="0.35"/>
    </g>
    <path d="{slot}" fill="none" stroke="url(#spectral)" stroke-width="2.5" filter="url(#glow)"/>
    <line x1="{L+10}" y1="{T+1}" x2="{R-10}" y2="{T+1}" stroke="url(#edge)" stroke-width="3" filter="url(#glow)"/>
    <line x1="{L+10}" y1="{B-1}" x2="{R-10}" y2="{B-1}" stroke="url(#edge)" stroke-width="3" filter="url(#glow)"/>
  </g>

  <text x="{cx}" y="720" text-anchor="middle" font-family="Avenir Next, Helvetica Neue, Arial, sans-serif"
        font-weight="800" font-size="104" letter-spacing="22" fill="url(#steel)">SPECTRA<tspan fill="url(#spectral)">BLADE</tspan></text>
  <rect x="{cx-330}" y="756" width="660" height="2" fill="url(#edge)"/>
  <text x="{cx}" y="800" text-anchor="middle" font-family="Avenir Next, Helvetica Neue, Arial, sans-serif"
        font-weight="500" font-size="24" letter-spacing="12" fill="#8f97ad">CUT · SHAPE · RESTORE</text>
</svg>'''
open(__import__("os").path.join(__import__("os").path.dirname(__file__), "spectrablade-banner.svg"), "w").write(svg)
