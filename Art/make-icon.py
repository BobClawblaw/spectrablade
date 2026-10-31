# Writes spectrablade-icon.svg, the app icon (1024 x 1024, macOS icon grid).
import math, os, random
random.seed(7)
S, cx, cy = 1024, 512, 512

def env(t):
    return (0.25 + 0.75 * abs(math.sin(t * math.pi * 3.2)) ** 1.5) * math.sin(t * math.pi) ** 0.6

# Waveform in (left) and spectrum out (right), behind the blade.
pts = []
for i in range(0, 150):
    x = 70 + i
    y = cy + 90 * (0.4 + 0.6 * abs(math.sin(i / 150 * 4 + 0.8))) * math.sin(i * 0.3) * math.sin(i * 0.04 + 1)
    pts.append(f"{x:.1f},{y:.1f}")
wave = "M" + " L".join(pts)
bars = []
for i in range(11):
    x = 818 + i * 13
    f = i / 10
    h = 130 * (0.3 + 0.7 * abs(math.sin(f * 5 + 0.6))) * math.exp(-f * 1.3) + random.uniform(6, 18)
    bars.append(f'<rect x="{x:.1f}" y="{cy-h:.1f}" width="7" height="{2*h:.1f}" rx="3.5" fill="hsl({300-120*f:.0f},95%,62%)" opacity="{0.95-0.45*f:.2f}"/>')

bw, bh = 640, 280
L, R, T, B = cx - bw/2, cx + bw/2, cy - bh/2, cy + bh/2
n, c = 40, 26
blade = (f"M{L+c},{T} L{R-c},{T} L{R},{T+c} L{R},{cy-n} A{n},{n} 0 0 0 {R},{cy+n} "
         f"L{R},{B-c} L{R-c},{B} L{L+c},{B} L{L},{B-c} L{L},{cy+n} A{n},{n} 0 0 0 {L},{cy-n} L{L},{T+c} Z")
sl, sr = cx - 220, cx + 220
top, bot = [], []
for i in range(0, 101):
    t = i / 100
    x = sl + t * (sr - sl)
    a = 8 + 56 * env(t) * (0.6 + 0.4 * abs(math.sin(i * 0.9)))
    top.append(f"{x:.1f},{cy-a:.1f}")
    bot.append(f"{x:.1f},{cy+a:.1f}")
slot = "M" + " L".join(top) + " L" + " L".join(reversed(bot)) + " Z"
holes = f"M{sl-36},{cy} a19,19 0 1 0 38,0 a19,19 0 1 0 -38,0 Z M{sr-2},{cy} a19,19 0 1 0 38,0 a19,19 0 1 0 -38,0 Z"

svg = f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {S} {S}" width="{S}" height="{S}">
  <defs>
    <radialGradient id="bg" cx="50%" cy="38%" r="80%">
      <stop offset="0" stop-color="#2a1f4d"/><stop offset="0.55" stop-color="#110e20"/><stop offset="1" stop-color="#06050c"/>
    </radialGradient>
    <linearGradient id="rim" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#ffffff" stop-opacity="0.22"/><stop offset="1" stop-color="#ffffff" stop-opacity="0.03"/>
    </linearGradient>
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
      <feGaussianBlur stdDeviation="7" result="b"/><feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge>
    </filter>
    <filter id="shadow" x="-20%" y="-30%" width="140%" height="170%">
      <feDropShadow dx="0" dy="20" stdDeviation="20" flood-color="#000" flood-opacity="0.75"/>
    </filter>
    <filter id="tileShadow" x="-10%" y="-10%" width="120%" height="125%">
      <feDropShadow dx="0" dy="12" stdDeviation="14" flood-color="#000" flood-opacity="0.45"/>
    </filter>
    <clipPath id="tile"><rect x="100" y="100" width="824" height="824" rx="185"/></clipPath>
    <clipPath id="bladeClip"><path d="{blade}"/></clipPath>
  </defs>

  <rect x="100" y="100" width="824" height="824" rx="185" fill="url(#bg)" filter="url(#tileShadow)"/>
  <g clip-path="url(#tile)">
    <g stroke="#ffffff" stroke-opacity="0.05">
      {''.join(f'<line x1="{x}" y1="100" x2="{x}" y2="924"/>' for x in range(100, 925, 48))}
      {''.join(f'<line x1="100" y1="{y}" x2="924" y2="{y}"/>' for y in range(100, 925, 48))}
    </g>
    <g transform="rotate(-45 {cx} {cy})">
      <path d="{wave}" fill="none" stroke="#00e5ff" stroke-width="5" filter="url(#glow)"/>
      <g filter="url(#glow)">{''.join(bars)}</g>
    </g>
    <g transform="rotate(-45 {cx} {cy}) translate({cx} {cy}) scale(1.0) translate({-cx} {-cy})">
      <path d="{slot} {holes}" fill="#0a0814"/>
      <g filter="url(#shadow)"><path d="{blade} {slot} {holes}" fill="url(#steel)" fill-rule="evenodd"/></g>
      <g clip-path="url(#bladeClip)">
        <rect x="{L}" y="{T}" width="{bw}" height="{bh}" fill="url(#sheen)"/>
        <rect x="{L}" y="{T}" width="{bw}" height="18" fill="#ffffff" opacity="0.35"/>
        <rect x="{L}" y="{B-18}" width="{bw}" height="18" fill="#1d2130" opacity="0.35"/>
      </g>
      <path d="{slot}" fill="none" stroke="url(#spectral)" stroke-width="4" filter="url(#glow)"/>
      <line x1="{L+12}" y1="{T+1}" x2="{R-12}" y2="{T+1}" stroke="url(#edge)" stroke-width="4" filter="url(#glow)"/>
      <line x1="{L+12}" y1="{B-1}" x2="{R-12}" y2="{B-1}" stroke="url(#edge)" stroke-width="4" filter="url(#glow)"/>
    </g>
  </g>
  <rect x="101" y="101" width="822" height="822" rx="184" fill="none" stroke="url(#rim)" stroke-width="2"/>
</svg>'''
open(os.path.join(os.path.dirname(__file__), "spectrablade-icon.svg"), "w").write(svg)
