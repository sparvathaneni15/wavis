import test from "node:test";
import assert from "node:assert/strict";
import {
  VISUAL,
  softLimit,
  stepRunningPeak,
  normalizeToPeak,
  createLimiterState,
  limitBands,
  boundDrives,
} from "./visual.js";

const dt = 1 / 60;

test("soft limiter is linear under the knee and never reaches the ceiling", () => {
  const ceiling = 1;
  const knee = VISUAL.limiter.knee;
  assert.equal(softLimit(0, ceiling, knee), 0);
  assert.equal(softLimit(-4, ceiling, knee), 0);
  assert.equal(softLimit(Number.NaN, ceiling, knee), 0);
  const start = ceiling * knee;
  assert.equal(softLimit(start * 0.5, ceiling, knee), start * 0.5);
  assert.equal(softLimit(start, ceiling, knee), start);

  let previous = 0;
  for (let i = 1; i <= 40; ++i) {
    const value = softLimit(i * 0.25, ceiling, knee);
    assert.ok(value > previous, `expected a monotonic limiter at input ${i * 0.25}`);
    assert.ok(value < ceiling, `limiter reached the ceiling at input ${i * 0.25}`);
    previous = value;
  }
});

test("running peak attacks quickly and releases slowly", () => {
  const { attack, release } = VISUAL.limiter;
  const risen = stepRunningPeak(0, 8, dt, attack, release);
  const fallen = stepRunningPeak(8, 0, dt, attack, release);
  const rise = risen - 0;
  const fall = 8 - fallen;
  assert.ok(rise > 2, `attack should cover a large part of a step, rose ${rise}`);
  assert.ok(fall < 0.25, `release should barely move in one frame, fell ${fall}`);
  assert.ok(rise > fall * 15, `attack ${rise} should be much faster than release ${fall}`);
  assert.ok(fallen > 7, "a slow release keeps the peak elevated");
});

test("unity floor does not amplify in-range bands, and an overload is folded down", () => {
  assert.equal(normalizeToPeak(0.4, 0.4, 1), 0.4);
  assert.equal(normalizeToPeak(1, 0.2, 1), 1);
  assert.ok(Math.abs(normalizeToPeak(8, 8, 1) - 1) < 1e-9);

  const early = limitBands(createLimiterState(), { bass: 8, mid: 6, treble: 5, level: 7 }, dt, VISUAL.limiter);
  for (const key of ["bass", "mid", "treble", "level"])
    assert.ok(early[key] < VISUAL.limiter.bandCeiling, `${key} escaped the band ceiling on the first loud frame`);

  const state = createLimiterState();
  let settled = early;
  for (let i = 0; i < 240; ++i)
    settled = limitBands(state, { bass: 8, mid: 6, treble: 5, level: 7 }, dt, VISUAL.limiter);
  assert.ok(settled.bass > 0.85 && settled.bass < VISUAL.limiter.bandCeiling);
  assert.ok(state.bass > 7, "the peak should have caught the sustained overload");

  const quietAfter = limitBands(state, { bass: 0.2, mid: 0.2, treble: 0.2, level: 0.2 }, dt, VISUAL.limiter);
  assert.ok(quietAfter.bass < 0.2, "while the peak is still high, a quiet band is ducked below itself");
});

test("driven parameters stay under their ceilings and still move with the music", () => {
  const quiet = boundDrives(VISUAL, { bass: 0.08, mid: 0.1, treble: 0.05, level: 0.1 });
  const loud = boundDrives(VISUAL, { bass: 1, mid: 1, treble: 1, level: 1 });
  const keys = ["bloom", "expansion", "formScale", "size", "brightness", "trail", "emission", "mass", "volumeEnergy"];
  for (const key of keys)
    assert.ok(loud[key] > quiet[key], `${key} should rise from a quiet band to a full-scale band`);

  const ceilings = {
    bloom: VISUAL.bloom.ceiling,
    expansion: VISUAL.scene.expansion.ceiling,
    formScale: VISUAL.scene.form.ceiling,
    size: VISUAL.scene.size.ceiling,
    brightness: VISUAL.scene.brightness.ceiling,
    trail: VISUAL.scene.trailCeiling,
    emission: VISUAL.scene.emission.ceiling,
    mass: VISUAL.scene.curvature.massCeiling,
    volumeEnergy: VISUAL.gradient.energyCeiling,
  };
  const overload = boundDrives(VISUAL, { bass: 8, mid: 6, treble: 5, level: 7 });
  for (const key of keys) {
    assert.ok(loud[key] < ceilings[key], `${key} at full scale reached its ceiling`);
    assert.ok(overload[key] < ceilings[key], `${key} under an overload reached its ceiling`);
  }

  // Once the running peak has caught a sustained overload, the normalised bands sit
  // on the same ceiling as a legitimate full-scale band, so the picture does not
  // keep climbing.
  const state = createLimiterState();
  let settled = null;
  for (let frame = 0; frame < 240; ++frame)
    settled = limitBands(state, { bass: 8, mid: 6, treble: 5, level: 7 }, dt, VISUAL.limiter);
  const held = boundDrives(VISUAL, settled);
  const full = boundDrives(VISUAL, limitBands(createLimiterState(), { bass: 1, mid: 1, treble: 1, level: 1 }, 2, VISUAL.limiter));
  for (const key of keys) {
    assert.ok(held[key] < ceilings[key], `${key} after the peak settled reached its ceiling`);
    assert.ok(Math.abs(held[key] - full[key]) < 1e-6, `${key} should match a full-scale band once the peak catches up`);
  }
});
