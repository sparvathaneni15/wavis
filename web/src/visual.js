// Drawing constants and the audio-drive limiter. The page does not analyse audio.
// Native code (VisualFrame.h) sends waveform peaks, log FFT bins, and bass, mid,
// treble, and level. Everything that changes how far those numbers move the picture
// lives in VISUAL.

// Soft knee. Output tracks `value` until `knee` of the way to `ceiling`, then bends
// and approaches `ceiling` without reaching it. At or below the knee the mapping is
// the identity, so quiet-to-medium motion stays proportional.
export function softLimit(value, ceiling, kneeRatio = 0.7) {
  const v = Math.max(0, Number.isFinite(value) ? value : 0);
  const cap = Math.max(1e-4, Number.isFinite(ceiling) ? ceiling : 1);
  const start = cap * Math.min(0.95, Math.max(0.05, kneeRatio));
  if (v <= start)
    return v;
  const span = cap - start;
  const t = (v - start) / span;
  return start + span * (1 - Math.exp(-t));
}

// Running peak. Fast attack while the input is above the peak, slow release after,
// so a bass hit is caught immediately and the peak stays up through the note.
export function stepRunningPeak(peak, sample, dt, attack, release) {
  const current = Math.max(0, Number.isFinite(peak) ? peak : 0);
  const target = Math.max(0, Number.isFinite(sample) ? sample : 0);
  const rate = target > current ? attack : release;
  const step = 1 - Math.exp(-Math.max(0, rate) * Math.max(0, dt));
  return current + (target - current) * step;
}

// Divide by the recent peak, but never by less than `floor`. A floor of 1 leaves
// bands that are already inside 0..1 alone, and folds a value above the peak back down.
export function normalizeToPeak(sample, peak, floor) {
  const s = Math.max(0, Number.isFinite(sample) ? sample : 0);
  const denom = Math.max(Number.isFinite(peak) ? peak : 0, Number.isFinite(floor) ? floor : 0, 1e-4);
  return s / denom;
}

export function createLimiterState() {
  return { bass: 0, mid: 0, treble: 0, level: 0 };
}

export function limitBands(state, raw, dt, limiter) {
  const out = {};
  for (const key of ["bass", "mid", "treble", "level"]) {
    const sample = Math.max(0, Number(raw[key]) || 0);
    state[key] = stepRunningPeak(state[key], sample, dt, limiter.attack, limiter.release);
    out[key] = softLimit(normalizeToPeak(sample, state[key], limiter.floor), limiter.bandCeiling, limiter.knee);
  }
  return out;
}

// Map limited bands onto the picture. Every returned drive is soft-limited to the
// ceiling stored next to its gain in VISUAL, so a band stuck at 1 (or far above it)
// cannot push bloom, size, brightness, or scale past that ceiling.
export function boundDrives(visual, bands) {
  const knee = visual.limiter.knee;
  const bass = Math.max(0, Number(bands.bass) || 0);
  const mid = Math.max(0, Number(bands.mid) || 0);
  const treble = Math.max(0, Number(bands.treble) || 0);
  const level = Math.max(0, Number(bands.level) || 0);
  const scene = visual.scene;
  const gradient = visual.gradient;
  return {
    bass,
    mid,
    treble,
    level,
    bloom: softLimit(visual.bloom.base + bass * visual.bloom.bass, visual.bloom.ceiling, knee),
    expansion: softLimit(scene.expansion.baseSpeed + mid * scene.expansion.midToSpeed, scene.expansion.ceiling, knee),
    formScale: softLimit(scene.form.baseScale + bass * scene.form.bassScale, scene.form.ceiling, knee),
    size: softLimit(scene.size.base + mid * scene.size.mid + treble * scene.size.treble, scene.size.ceiling, knee),
    brightness: softLimit(
      scene.brightness.base + mid * scene.brightness.mid + treble * scene.brightness.treble,
      scene.brightness.ceiling,
      knee
    ),
    trail: softLimit(scene.trailLow + level * (scene.trailLength - scene.trailLow), scene.trailCeiling, knee),
    emission: softLimit(scene.emission.baseRate + mid * scene.emission.midToRate, scene.emission.ceiling, knee),
    mass: softLimit(bass * scene.curvature.bassToMass, scene.curvature.massCeiling, knee),
    volumeEnergy: softLimit(gradient.energy + level * gradient.loudnessEnergy, gradient.energyCeiling, knee),
  };
}

export const VISUAL = {
  // Smoothing after the limiter. Attack opens quickly; release lets a hit decay.
  attack: 16,
  release: 2.1,
  budgetMs: 20,
  recoverMs: 12,
  warmupFrames: 48,

  // Running-peak normaliser, then a soft ceiling on each band.
  // floor 1: do not amplify a band that is already in 0..1.
  // attack is many times faster than release, so a full-scale bass hit is absorbed
  // in a frame or two and the peak stays high while the note rings.
  limiter: {
    attack: 48,
    release: 0.7,
    floor: 1,
    bandCeiling: 1,
    knee: 0.7,
  },

  // OutputPass applies this exposure with ACES. Materials stay linear; the tone map
  // is the last step, so overlapping additive fragments compress instead of clipping.
  tone: {
    exposure: 0.9,
  },

  // Accent hue for the waveform, bars, and wireframe. The swing stays inside
  // cyan → violet/magenta, and follow is slow so loudness does not strobe the hue.
  hue: {
    from: 0.5,
    to: 0.78,
    saturation: 0.78,
    lightness: 0.5,
    follow: 0.08,
  },

  // Bloom is a halo on the bright cores only. `bass` is how far a full bass band
  // may lift strength; `ceiling` is the soft limit either way.
  bloom: {
    base: 0.06,
    bass: 0.07,
    ceiling: 0.16,
    radius: 0.22,
    threshold: 0.84,
  },

  waveform: {
    trails: 6,
    trailsLow: 3,
    halfWidth: 0.0048,
    // Peak vertical half-extent of the stroke, in view units.
    amplitude: 0.62,
    // Oldest stroke opacity is opacity * fade^i. The sum stays under 1 so the
    // stacked additive strokes cannot become a white ribbon.
    opacity: 0.42,
    fade: 0.52,
    colorGain: 0.78,
  },

  bars: {
    count: 64,
    height: 2.35,
    radius: 0.045,
    // Pulls bar colour down so bloom does not lift a full-scale spectrum to white.
    colorGain: 0.66,
  },

  scene: {
    particlesHigh: 480,
    particlesLow: 140,
    // Trail buffer length, and the loud end of the audio-driven trail.
    trailLength: 8,
    trailLow: 3,
    trailCeiling: 8,
    // Trail fragments are dimmer than the particle head so a stack of segments
    // does not add up to white.
    trailGain: 0.34,
    // Fragment soft knee for additive particles. Higher bends bright cores sooner,
    // which is what stops a cluster of cyan/magenta/violet from summing to white.
    additiveKnee: 2.6,
    maxRadius: 5.6,
    maxAge: 9.5,
    curvature: {
      // Bass still bends paths, but a smaller mass keeps particles from collapsing
      // onto one point and adding every hue into a white core.
      bassToMass: 2.2,
      massCeiling: 2.6,
      softening: 0.2,
      maxAccel: 24,
    },
    wells: [
      { x: 0, y: 0, z: 0, mass: 1, drift: 0 },
      { x: 2.05, y: 0.2, z: 0.45, mass: 0.46, drift: 0.16 },
    ],
    emission: { baseRate: 12, midToRate: 16, ceiling: 34 },
    expansion: { baseSpeed: 0.08, midToSpeed: 0.18, ceiling: 0.32 },
    // Point size in the particle shader is aSize * depthScale / view depth.
    // depthScale used to be a hardcoded 260, which made each sprite hundreds of
    // pixels wide and let them paint the frame white.
    size: { base: 2.05, mid: 0.55, treble: 0.32, ceiling: 3.15, depthScale: 136 },
    brightness: { base: 0.24, mid: 0.09, treble: 0.06, ceiling: 0.4 },
    shimmer: { trebleSpeed: 28, trebleAmount: 0.22 },
    sparkle: { trebleChance: 0.28, whiteMix: 0.16 },
    speedColor: { slow: 0.45, fast: 3.1 },
    // Wireframe radius. bassScale is the pulse on top of baseScale, not the whole size.
    form: { baseScale: 0.72, bassScale: 0.14, ceiling: 0.92 },
  },

  gradient: {
    radius: 17,
    stepsHigh: 7,
    stepsLow: 2,
    octavesHigh: 3,
    octavesLow: 1,
    // Field motion. Previously this was timeSpeed 0.09 multiplied by a hardcoded 8.
    timeSpeed: 0.2,
    // Scales how far layers, noise, and depth pull the hue. Lower is a slower shift.
    hueSpeed: 0.2,
    // Low spatial frequency so a ray through the volume does not average itself
    // into a flat fog. The shapes stay large enough to read across a room.
    noiseScale: 0.22,
    fieldFreq: 0.38,
    warp: 2.4,
    softening: 0.7,
    depthHue: 0.04,
    // Resting volume brightness, plus how far level may lift it, then a ceiling.
    energy: 0.72,
    loudnessEnergy: 0.34,
    energyCeiling: 1.15,
    // Dark valleys and bright ridges. A high floor here is what flattened the field.
    glowFloor: 0.03,
    glowSpan: 1.15,
    // Weight of each ray step. Was a hardcoded 0.2, which stacked the steps past 1.
    scatter: 0.15,
    // In-shader shoulder, applied before bloom: color / (1 + color * compress).
    compress: 0.7,
    accentSpan: 0.07,
    // Palette anchors stay the Neon Night primaries. saturation mixes each toward
    // its luminance; gain scales the result so the field stays darker than the particles.
    saturation: 0.82,
    gain: 0.62,
    cyan: [0.18, 0.9, 1.0],
    magenta: [1.0, 0.14, 0.8],
    violet: [0.52, 0.3, 1.0],
  },
};
