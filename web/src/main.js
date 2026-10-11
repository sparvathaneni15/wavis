import * as THREE from "three";
import { EffectComposer } from "three/examples/jsm/postprocessing/EffectComposer.js";
import { RenderPass } from "three/examples/jsm/postprocessing/RenderPass.js";
import { UnrealBloomPass } from "three/examples/jsm/postprocessing/UnrealBloomPass.js";
import { OutputPass } from "three/examples/jsm/postprocessing/OutputPass.js";

// Drawing only. The native engine sends waveform peaks, log FFT bins, and
// bass/mid/treble levels. This page smooths them and draws. It does not analyse audio.
//
// VISUAL is the single place to retune envelopes, bloom, particle motion, and the
// colour volume. Band ranges themselves are measured in native code (VisualFrame.h).

const VISUAL = {
  attack: 16,
  release: 2.1,
  accentFollow: 0.32,
  budgetMs: 20,
  recoverMs: 12,
  warmupFrames: 48,
  bloom: { base: 0.32, bass: 0.62, radius: 0.38, threshold: 0.58 },
  dynamics: {
    target: 0.46,
    knee: 0.62,
    ceiling: 1,
    maxMakeup: 2.2,
    peakAttack: 10,
    peakRelease: 0.28,
    maxBloom: 0.82,
    maxEnergy: 1.05,
    maxBrightness: 1.05,
    maxParticleProduct: 280,
    addCap: 1.15,
    luminanceCeiling: 0.48,
    luminanceRelease: 0.8,
    luminanceFloorGain: 0.35,
  },
  waveform: { trails: 14, trailsLow: 5, halfWidth: 0.0065 },
  bars: { count: 64, height: 3.2, radius: 0.045 },
  scene: {
    particlesHigh: 480,
    particlesLow: 140,
    trailLength: 12,
    trailLow: 4,
    maxRadius: 5.6,
    maxAge: 9.5,
    curvature: { bassToMass: 6.5, softening: 0.2, maxAccel: 24 },
    wells: [
      { x: 0, y: 0, z: 0, mass: 1, drift: 0 },
      { x: 2.05, y: 0.2, z: 0.45, mass: 0.46, drift: 0.16 },
    ],
    emission: { baseRate: 22, midToRate: 80 },
    expansion: { baseSpeed: 0.16, midToSpeed: 0.72 },
    size: { base: 9, mid: 11, treble: 9 },
    brightness: { base: 0.32, mid: 0.38, treble: 0.42 },
    shimmer: { trebleSpeed: 28, trebleAmount: 0.32 },
    sparkle: { trebleChance: 0.62 },
    speedColor: { slow: 0.45, fast: 3.1 },
    form: { baseScale: 0.78, bassScale: 0.62 },
  },
  room: {
    half: 4.2,
    floorY: -1.35,
    wallHeight: 2.55,
    speakerHalf: 3.6,
    cabinet: { w: 0.46, h: 0.78, d: 0.34 },
  },
  gradient: {
    radius: 17,
    stepsHigh: 7,
    stepsLow: 2,
    octavesHigh: 3,
    octavesLow: 1,
    timeSpeed: 0.09,
    noiseScale: 0.46,
    fieldFreq: 1.15,
    warp: 3.8,
    softening: 0.7,
    depthHue: 0.05,
    energy: 0.62,
    loudnessEnergy: 0.38,
    accentSpan: 0.3,
    cyan: [0.18, 0.9, 1.0],
    magenta: [1.0, 0.14, 0.8],
    violet: [0.52, 0.3, 1.0],
  },
};

const WAVE = "oscilloscope";
const BARS = "spectrum";
const SCENE = "radial";
const MODE_LABEL = { oscilloscope: "Waveform", spectrum: "Spectrum", radial: "3D scene" };

const CYAN = new THREE.Color(0x3df0ff);
const MAGENTA = new THREE.Color(0xff2bd6);
const VIOLET = new THREE.Color(0x8b5cff);

function callNative(name, params = []) {
  const backend = window.__JUCE__ && window.__JUCE__.backend;
  if (!backend || typeof backend.emitEvent !== "function")
    return;
  backend.emitEvent("__juce__invoke", {
    name,
    params,
    resultId: Math.floor(Math.random() * 1e9),
  });
}

function follow(current, target, dt, attack, release) {
  const rate = target > current ? attack : release;
  return current + (target - current) * (1 - Math.exp(-rate * dt));
}

function lerp(a, b, t) {
  return a + (b - a) * t;
}

function clamp01(value) {
  return Math.max(0, Math.min(1, value));
}

// Same curve as visualSoftKnee / visualCompress in src/core/VisualDynamics.h.
function softKnee(x, knee, ceiling) {
  if (!(x > 0))
    return 0;
  if (!(ceiling > knee))
    return Math.min(x, Math.max(ceiling, 0));
  if (x <= knee)
    return x;
  const span = ceiling - knee;
  const bent = knee + span * Math.tanh((x - knee) / span);
  // Same cap as std::nextafter(ceiling, 0) in VisualDynamics.h: the knee approaches the ceiling and stays under it.
  return Math.min(bent, ceiling - Math.max(Math.abs(ceiling) * 1e-6, 1e-6));
}

function makeDynamics() {
  return { reference: VISUAL.dynamics.target * 0.45 };
}

function compress(input, state, dt) {
  const cfg = VISUAL.dynamics;
  const x = Math.max(0, input);
  const rate = x > state.reference ? cfg.peakAttack : cfg.peakRelease;
  state.reference += (x - state.reference) * (1 - Math.exp(-rate * Math.max(dt, 0)));
  if (state.reference < 1e-4)
    state.reference = 1e-4;
  const makeup = Math.min(cfg.maxMakeup, cfg.target / state.reference);
  const bent = softKnee(x * makeup, cfg.knee, cfg.ceiling);
  return Math.min(bent, cfg.ceiling - 1e-4);
}

const probe = document.createElement("canvas");
if (!probe.getContext("webgl2") && !probe.getContext("webgl"))
  callNative("webglUnavailable");

const renderer = new THREE.WebGLRenderer({ antialias: true, alpha: false, preserveDrawingBuffer: true });
renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 1.25));
renderer.setClearColor(0x070414, 1);
renderer.toneMapping = THREE.ACESFilmicToneMapping;
renderer.toneMappingExposure = 1;
renderer.setSize(window.innerWidth, window.innerHeight, false);
document.body.appendChild(renderer.domElement);

const ortho = new THREE.OrthographicCamera(-1.25, 1.25, 1.05, -1.05, 0.1, 10);
ortho.position.z = 2;
const perspective = new THREE.PerspectiveCamera(42, 1, 0.1, 80);
perspective.position.set(0.2, 1.05, 6.5);
perspective.lookAt(0, 0, 0);

const waveScene = new THREE.Scene();
const barScene = new THREE.Scene();
const worldScene = new THREE.Scene();

const ribbonCount = VISUAL.waveform.trails;
const ribbons = [];
for (let i = 0; i < ribbonCount; ++i) {
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute("position", new THREE.BufferAttribute(new Float32Array((128 - 1) * 18), 3));
  const material = new THREE.MeshBasicMaterial({
    color: CYAN,
    transparent: true,
    opacity: 1,
    blending: THREE.AdditiveBlending,
    depthWrite: false,
    side: THREE.DoubleSide,
    toneMapped: false,
  });
  const mesh = new THREE.Mesh(geometry, material);
  mesh.frustumCulled = false;
  waveScene.add(mesh);
  ribbons.push(mesh);
}
const waveHistory = [];

const barCount = VISUAL.bars.count;
const barGeometry = new THREE.CapsuleGeometry(VISUAL.bars.radius, 1, 2, 6);
const barMaterial = new THREE.MeshBasicMaterial({ color: 0xffffff, toneMapped: false });
const barMesh = new THREE.InstancedMesh(barGeometry, barMaterial, barCount);
const barReflection = new THREE.InstancedMesh(
  barGeometry,
  new THREE.MeshBasicMaterial({ color: 0xffffff, transparent: true, opacity: 0.22, toneMapped: false }),
  barCount
);
barMesh.instanceColor = new THREE.InstancedBufferAttribute(new Float32Array(barCount * 3), 3);
barReflection.instanceColor = new THREE.InstancedBufferAttribute(new Float32Array(barCount * 3), 3);
barScene.add(barMesh);
barScene.add(barReflection);
const barDummy = new THREE.Object3D();
const barColor = new THREE.Color();
const smoothBins = new Float32Array(barCount);

const sceneCfg = VISUAL.scene;
const particleCount = sceneCfg.particlesHigh;
const trailLength = sceneCfg.trailLength;
const px = new Float32Array(particleCount);
const py = new Float32Array(particleCount);
const pz = new Float32Array(particleCount);
const vx = new Float32Array(particleCount);
const vy = new Float32Array(particleCount);
const vz = new Float32Array(particleCount);
const age = new Float32Array(particleCount);
const seed = new Float32Array(particleCount);
const alive = new Uint8Array(particleCount);
const trail = new Float32Array(particleCount * trailLength * 4);

const pointGeometry = new THREE.BufferGeometry();
pointGeometry.setAttribute("position", new THREE.BufferAttribute(new Float32Array(particleCount * 3), 3));
pointGeometry.setAttribute("aSize", new THREE.BufferAttribute(new Float32Array(particleCount), 1));
pointGeometry.setAttribute("aColor", new THREE.BufferAttribute(new Float32Array(particleCount * 3), 3));
const pointMaterial = new THREE.ShaderMaterial({
  transparent: true,
  depthWrite: false,
  blending: THREE.AdditiveBlending,
  toneMapped: false,
  vertexShader: `
    attribute float aSize;
    attribute vec3 aColor;
    varying vec3 vColor;
    void main() {
      vColor = aColor;
      vec4 mv = modelViewMatrix * vec4(position, 1.0);
      gl_PointSize = aSize * (260.0 / max(0.2, -mv.z));
      gl_Position = projectionMatrix * mv;
    }
  `,
  fragmentShader: `
    varying vec3 vColor;
    void main() {
      vec2 d = gl_PointCoord - vec2(0.5);
      float r = length(d);
      if (r > 0.5) discard;
      float glow = smoothstep(0.5, 0.05, r);
      gl_FragColor = vec4(vColor * glow, glow);
    }
  `,
});
const points = new THREE.Points(pointGeometry, pointMaterial);
points.frustumCulled = false;
worldScene.add(points);

const segmentCount = particleCount * (trailLength - 1);
const trailGeometry = new THREE.BufferGeometry();
trailGeometry.setAttribute("position", new THREE.BufferAttribute(new Float32Array(segmentCount * 2 * 3), 3));
trailGeometry.setAttribute("color", new THREE.BufferAttribute(new Float32Array(segmentCount * 2 * 3), 3));
const trailMaterial = new THREE.LineBasicMaterial({
  vertexColors: true,
  transparent: true,
  blending: THREE.AdditiveBlending,
  depthWrite: false,
  toneMapped: false,
});
const trailLines = new THREE.LineSegments(trailGeometry, trailMaterial);
trailLines.frustumCulled = false;
worldScene.add(trailLines);

const form = new THREE.Mesh(
  new THREE.IcosahedronGeometry(1, 1),
  new THREE.MeshBasicMaterial({ color: VIOLET, wireframe: true, toneMapped: false })
);
worldScene.add(form);

const room = VISUAL.room;
const floorMaterial = new THREE.MeshBasicMaterial({
  color: 0x140818,
  transparent: true,
  opacity: 0.72,
  toneMapped: false,
});
const floor = new THREE.Mesh(new THREE.PlaneGeometry(room.half * 2, room.half * 2), floorMaterial);
floor.rotation.x = -Math.PI / 2;
floor.position.y = room.floorY;
worldScene.add(floor);

const wallMaterial = new THREE.MeshBasicMaterial({
  color: 0x241038,
  transparent: true,
  opacity: 0.34,
  side: THREE.DoubleSide,
  toneMapped: false,
});
function addWall(width, height, x, y, z, yaw) {
  const mesh = new THREE.Mesh(new THREE.PlaneGeometry(width, height), wallMaterial);
  mesh.position.set(x, y, z);
  mesh.rotation.y = yaw;
  worldScene.add(mesh);
}
const wallY = room.floorY + room.wallHeight * 0.5;
addWall(room.half * 2, room.wallHeight, 0, wallY, -room.half, 0);
addWall(room.half * 2, room.wallHeight, -room.half, wallY, 0, Math.PI / 2);
addWall(room.half * 2, room.wallHeight, room.half, wallY, 0, -Math.PI / 2);

const edgeMaterial = new THREE.LineBasicMaterial({ color: 0x7d5cff, transparent: true, opacity: 0.45, toneMapped: false });
function addEdge(ax, ay, az, bx, by, bz) {
  const geometry = new THREE.BufferGeometry().setFromPoints([
    new THREE.Vector3(ax, ay, az),
    new THREE.Vector3(bx, by, bz),
  ]);
  worldScene.add(new THREE.Line(geometry, edgeMaterial));
}
const top = room.floorY + room.wallHeight;
addEdge(-room.half, room.floorY, -room.half, room.half, room.floorY, -room.half);
addEdge(-room.half, room.floorY, -room.half, -room.half, room.floorY, room.half);
addEdge(room.half, room.floorY, -room.half, room.half, room.floorY, room.half);
addEdge(-room.half, top, -room.half, room.half, top, -room.half);
addEdge(-room.half, top, -room.half, -room.half, top, room.half);
addEdge(room.half, top, -room.half, room.half, top, room.half);
addEdge(-room.half, room.floorY, -room.half, -room.half, top, -room.half);
addEdge(room.half, room.floorY, -room.half, room.half, top, -room.half);
addEdge(-room.half, room.floorY, room.half, -room.half, top, room.half);
addEdge(room.half, room.floorY, room.half, room.half, top, room.half);

const speakerRoot = new THREE.Group();
worldScene.add(speakerRoot);
const speakerById = new Map();
let dragId = null;
let demoSpeakers = [];

function clampRoom(value) {
  const limit = room.speakerHalf;
  if (!Number.isFinite(value))
    return 0;
  return Math.max(-limit, Math.min(limit, value));
}

function makeLabel(text) {
  const canvas = document.createElement("canvas");
  canvas.width = 256;
  canvas.height = 64;
  const ctx = canvas.getContext("2d");
  ctx.fillStyle = "rgba(8, 4, 18, 0.78)";
  ctx.fillRect(8, 8, 240, 48);
  ctx.font = "28px sans-serif";
  ctx.fillStyle = "#d7f6ff";
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  const shown = String(text || "Output").slice(0, 18);
  ctx.fillText(shown, 128, 32);
  const texture = new THREE.CanvasTexture(canvas);
  const sprite = new THREE.Sprite(new THREE.SpriteMaterial({ map: texture, transparent: true, toneMapped: false }));
  sprite.position.y = room.cabinet.h + 0.28;
  sprite.scale.set(1.35, 0.34, 1);
  sprite.userData.texture = texture;
  return sprite;
}

function disposeSpeaker(group) {
  group.traverse((child) => {
    if (child.geometry)
      child.geometry.dispose();
    const materials = child.material ? [].concat(child.material) : [];
    for (const material of materials) {
      if (material.map)
        material.map.dispose();
      material.dispose();
    }
    if (child.userData && child.userData.texture)
      child.userData.texture.dispose();
  });
}

function makeCabinet(id, label) {
  const group = new THREE.Group();
  group.userData.speakerId = id;
  group.userData.label = label;
  const body = new THREE.Mesh(
    new THREE.BoxGeometry(room.cabinet.w, room.cabinet.h, room.cabinet.d),
    new THREE.MeshBasicMaterial({ color: 0x1a0c22, toneMapped: false })
  );
  body.position.y = room.cabinet.h * 0.5;
  const woofer = new THREE.Mesh(
    new THREE.CircleGeometry(0.15, 18),
    new THREE.MeshBasicMaterial({ color: 0xff2bd6, toneMapped: false })
  );
  woofer.position.set(0, room.cabinet.h * 0.3, room.cabinet.d * 0.51);
  const tweeter = new THREE.Mesh(
    new THREE.CircleGeometry(0.07, 16),
    new THREE.MeshBasicMaterial({ color: 0x3df0ff, toneMapped: false })
  );
  tweeter.position.set(0, room.cabinet.h * 0.62, room.cabinet.d * 0.51);
  group.add(body, woofer, tweeter, makeLabel(label));
  return group;
}

function syncSpeakers(list) {
  const next = new Set();
  for (const spot of list) {
    if (!spot || typeof spot.id !== "string" || spot.id.length === 0)
      continue;
    next.add(spot.id);
    let group = speakerById.get(spot.id);
    const label = typeof spot.label === "string" && spot.label.length ? spot.label : spot.id;
    if (!group) {
      group = makeCabinet(spot.id, label);
      speakerById.set(spot.id, group);
      speakerRoot.add(group);
    } else if (group.userData.label !== label) {
      const old = group.children.find((child) => child.isSprite);
      if (old) {
        group.remove(old);
        if (old.material) {
          if (old.material.map)
            old.material.map.dispose();
          old.material.dispose();
        }
      }
      group.add(makeLabel(label));
      group.userData.label = label;
    }
    if (dragId !== spot.id) {
      group.position.set(clampRoom(Number(spot.x)), room.floorY, clampRoom(Number(spot.z)));
    }
  }
  for (const [id, group] of speakerById) {
    if (next.has(id))
      continue;
    if (dragId === id)
      dragId = null;
    speakerRoot.remove(group);
    disposeSpeaker(group);
    speakerById.delete(id);
  }
}

const raycaster = new THREE.Raycaster();
const pointerNdc = new THREE.Vector2();
const floorPlane = new THREE.Plane(new THREE.Vector3(0, 1, 0), -room.floorY);
const floorHit = new THREE.Vector3();

function pointerToNdc(event) {
  const rect = renderer.domElement.getBoundingClientRect();
  const width = rect.width || 1;
  const height = rect.height || 1;
  pointerNdc.x = ((event.clientX - rect.left) / width) * 2 - 1;
  pointerNdc.y = -((event.clientY - rect.top) / height) * 2 + 1;
}

renderer.domElement.addEventListener("pointerdown", (event) => {
  if (mode !== SCENE)
    return;
  pointerToNdc(event);
  raycaster.setFromCamera(pointerNdc, perspective);
  const hits = raycaster.intersectObjects(speakerRoot.children, true);
  if (!hits.length)
    return;
  let object = hits[0].object;
  while (object && !object.userData.speakerId)
    object = object.parent;
  if (!object || !object.userData.speakerId)
    return;
  dragId = object.userData.speakerId;
  if (renderer.domElement.setPointerCapture)
    renderer.domElement.setPointerCapture(event.pointerId);
});

renderer.domElement.addEventListener("pointermove", (event) => {
  if (!dragId || mode !== SCENE)
    return;
  pointerToNdc(event);
  raycaster.setFromCamera(pointerNdc, perspective);
  if (!raycaster.ray.intersectPlane(floorPlane, floorHit))
    return;
  const group = speakerById.get(dragId);
  if (!group)
    return;
  group.position.set(clampRoom(floorHit.x), room.floorY, clampRoom(floorHit.z));
});

function finishSpeakerDrag() {
  if (!dragId)
    return;
  const group = speakerById.get(dragId);
  const id = dragId;
  dragId = null;
  if (!group)
    return;
  const x = clampRoom(group.position.x);
  const z = clampRoom(group.position.z);
  group.position.set(x, room.floorY, z);
  demoSpeakers = demoSpeakers.map((spot) => (spot.id === id ? { ...spot, x, z } : spot));
  callNative("placeSpeaker", [id, x, z]);
}

renderer.domElement.addEventListener("pointerup", finishSpeakerDrag);
renderer.domElement.addEventListener("pointercancel", finishSpeakerDrag);

const gradient = VISUAL.gradient;
const volumeMaterial = new THREE.ShaderMaterial({
  side: THREE.BackSide,
  depthWrite: false,
  toneMapped: false,
  uniforms: {
    uCameraPos: { value: new THREE.Vector3() },
    uTime: { value: 0 },
    uAccent: { value: 0 },
    uLevel: { value: 0 },
    uMassA: { value: 0 },
    uMassB: { value: 0 },
    uWellA: { value: new THREE.Vector3() },
    uWellB: { value: new THREE.Vector3() },
    uWarp: { value: gradient.warp },
    uSoft: { value: gradient.softening },
    uNoiseScale: { value: gradient.noiseScale },
    uFieldFreq: { value: gradient.fieldFreq },
    uDepthHue: { value: gradient.depthHue },
    uEnergy: { value: gradient.energy },
    uLoudnessEnergy: { value: gradient.loudnessEnergy },
    uAddCap: { value: VISUAL.dynamics.addCap },
    uAccentSpan: { value: gradient.accentSpan },
    uSteps: { value: gradient.stepsHigh },
    uOctaves: { value: gradient.octavesHigh },
    uCyan: { value: new THREE.Vector3().fromArray(gradient.cyan) },
    uMagenta: { value: new THREE.Vector3().fromArray(gradient.magenta) },
    uViolet: { value: new THREE.Vector3().fromArray(gradient.violet) },
  },
  vertexShader: `
    varying vec3 vWorld;
    void main() {
      vec4 world = modelMatrix * vec4(position, 1.0);
      vWorld = world.xyz;
      gl_Position = projectionMatrix * viewMatrix * world;
    }
  `,
  fragmentShader: `
    varying vec3 vWorld;
    uniform vec3 uCameraPos;
    uniform float uTime;
    uniform float uAccent;
    uniform float uLevel;
    uniform float uMassA;
    uniform float uMassB;
    uniform vec3 uWellA;
    uniform vec3 uWellB;
    uniform float uWarp;
    uniform float uSoft;
    uniform float uNoiseScale;
    uniform float uFieldFreq;
    uniform float uDepthHue;
    uniform float uEnergy;
    uniform float uLoudnessEnergy;
    uniform float uAddCap;
    uniform float uAccentSpan;
    uniform float uSteps;
    uniform float uOctaves;
    uniform vec3 uCyan;
    uniform vec3 uMagenta;
    uniform vec3 uViolet;

    float hash13(vec3 p) {
      p = fract(p * 0.1031);
      p += dot(p, p.zyx + 31.32);
      return fract((p.x + p.y) * p.z);
    }

    float valueNoise(vec3 p) {
      vec3 i = floor(p);
      vec3 f = fract(p);
      f = f * f * (3.0 - 2.0 * f);
      float n000 = hash13(i);
      float n100 = hash13(i + vec3(1.0, 0.0, 0.0));
      float n010 = hash13(i + vec3(0.0, 1.0, 0.0));
      float n110 = hash13(i + vec3(1.0, 1.0, 0.0));
      float n001 = hash13(i + vec3(0.0, 0.0, 1.0));
      float n101 = hash13(i + vec3(1.0, 0.0, 1.0));
      float n011 = hash13(i + vec3(0.0, 1.0, 1.0));
      float n111 = hash13(i + vec3(1.0, 1.0, 1.0));
      return mix(
        mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
        mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y),
        f.z);
    }

    float fbm(vec3 p) {
      float sum = 0.0;
      float weight = 0.5;
      float total = 0.0;
      for (int octave = 0; octave < 3; ++octave) {
        if (float(octave) >= uOctaves)
          break;
        sum += valueNoise(p) * weight;
        total += weight;
        p = p * 2.03 + vec3(1.7, 9.2, 2.4);
        weight *= 0.5;
      }
      return sum / max(total, 0.001);
    }

    vec3 bend(vec3 p) {
      vec3 q = p;
      vec3 towardA = uWellA - p;
      q += towardA * (uMassA * uWarp) / (dot(towardA, towardA) + uSoft);
      vec3 towardB = uWellB - p;
      q += towardB * (uMassB * uWarp) / (dot(towardB, towardB) + uSoft);
      return q;
    }

    vec3 palette(float hue) {
      float wrapped = fract(hue);
      vec3 color = mix(uCyan, uViolet, smoothstep(0.0, 0.58, wrapped));
      return mix(color, uMagenta, smoothstep(0.4, 1.0, wrapped));
    }

    vec3 field(vec3 p, float depthHue) {
      vec3 q = bend(p);
      float layers = sin(q.x * uFieldFreq + q.y * uFieldFreq * 0.37 + uTime);
      float sheets = sin(q.z * uFieldFreq * 0.86 - q.y * uFieldFreq * 0.48 - uTime * 0.65);
      float columns = sin(q.y * uFieldFreq * 1.12 + q.x * 0.22 + uTime * 0.4);
      float noise = fbm(q * uNoiseScale + vec3(0.0, uTime * 0.35, uTime * 0.15));
      float hue = fract(0.08 + layers * 0.16 + sheets * 0.14 + columns * 0.1 + noise * 0.42 + depthHue + uAccent * uAccentSpan);
      float glow = 0.38 + 0.62 * (0.5 + 0.5 * layers) * (0.6 + 0.4 * noise);
      return palette(hue) * glow;
    }

    void main() {
      vec3 ray = vWorld - uCameraPos;
      float far = max(length(ray), 0.001);
      vec3 direction = ray / far;
      float steps = clamp(uSteps, 1.0, 8.0);
      float stepLen = far / steps;
      vec3 color = vec3(0.03, 0.015, 0.05);
      float remaining = 1.0;
      float energy = uEnergy + uLoudnessEnergy * uLevel;
      for (int i = 0; i < 8; ++i) {
        if (float(i) >= steps)
          break;
        float distance = (float(i) + 0.5) * stepLen;
        vec3 samplePos = uCameraPos + direction * distance;
        vec3 sampleColor = field(samplePos, distance * uDepthHue);
        color += sampleColor * 0.2 * remaining * energy;
        remaining *= 0.8;
      }
      color = min(color, vec3(uAddCap));
      gl_FragColor = vec4(color, 1.0);
    }
  `,
});
const volume = new THREE.Mesh(new THREE.SphereGeometry(gradient.radius, 36, 24), volumeMaterial);
volume.frustumCulled = false;
volume.renderOrder = -2;
worldScene.add(volume);

const renderPass = new RenderPass(waveScene, ortho);
const bloomPass = new UnrealBloomPass(
  new THREE.Vector2(window.innerWidth, window.innerHeight),
  VISUAL.bloom.base,
  VISUAL.bloom.radius,
  VISUAL.bloom.threshold
);
const composer = new EffectComposer(renderer);
composer.addPass(renderPass);
composer.addPass(bloomPass);
composer.addPass(new OutputPass());

const caption = document.getElementById("caption");
const accentColor = new THREE.Color(0x3df0ff);

let mode = WAVE;
let latest = null;
let frameSerial = 0;
let drawnSerial = -1;
let bass = 0;
let mid = 0;
let treble = 0;
let level = 0;
const bandState = { bass: makeDynamics(), mid: makeDynamics(), treble: makeDynamics(), level: makeDynamics() };
const binState = Array.from({ length: barCount }, () => makeDynamics());
let lumaGain = 1;
let lumaTick = 0;
let probeLock = false;
let accent = 0;
let emitBudget = 0;
let trailPhase = 0;
let simTime = 0;
let qualityHigh = true;
let slowFrames = 0;
let fastFrames = 0;
let renderedFrames = 0;
let activeParticles = particleCount;
let activeTrails = sceneCfg.trailLength;
let lastNow = performance.now();

function spawn(index) {
  const angle = Math.random() * Math.PI * 2;
  const radius = 0.4 + Math.random() * 1.1;
  const side = Math.random() < 0.5 ? -1 : 1;
  px[index] = Math.cos(angle) * radius;
  pz[index] = Math.sin(angle) * radius;
  py[index] = (Math.random() - 0.5) * 0.55;
  const outward = 0.12 + mid * 0.2;
  const tangent = (1.05 + Math.random() * 0.85) * side;
  vx[index] = Math.cos(angle) * outward - Math.sin(angle) * tangent;
  vz[index] = Math.sin(angle) * outward + Math.cos(angle) * tangent;
  vy[index] = (Math.random() - 0.5) * 0.3;
  age[index] = 0;
  alive[index] = 1;
  const speed = Math.hypot(vx[index], vy[index], vz[index]);
  const base = index * trailLength * 4;
  for (let point = 0; point < trailLength; ++point) {
    trail[base + point * 4] = px[index];
    trail[base + point * 4 + 1] = py[index];
    trail[base + point * 4 + 2] = pz[index];
    trail[base + point * 4 + 3] = speed;
  }
}

for (let i = 0; i < particleCount; ++i) {
  seed[i] = Math.random() * Math.PI * 2;
  if (i < particleCount * 0.55)
    spawn(i);
}

function wellPosition(index, time, target) {
  const well = sceneCfg.wells[index];
  const turned = time * well.drift;
  const x = Math.cos(turned) * well.x - Math.sin(turned) * well.z;
  const z = Math.sin(turned) * well.x + Math.cos(turned) * well.z;
  target.set(x, well.y, z);
  return well.mass;
}

function integrateParticles(dt) {
  const mass = bass * sceneCfg.curvature.bassToMass;
  const wellA = volumeMaterial.uniforms.uWellA.value;
  const wellB = volumeMaterial.uniforms.uWellB.value;
  const massA = mass * wellPosition(0, simTime, wellA);
  const massB = mass * wellPosition(1, simTime, wellB);
  volumeMaterial.uniforms.uMassA.value = massA;
  volumeMaterial.uniforms.uMassB.value = massB;

  trailPhase += 1;
  emitBudget += (sceneCfg.emission.baseRate + mid * sceneCfg.emission.midToRate) * dt;
  let births = Math.floor(emitBudget);
  emitBudget -= births;

  const soft = sceneCfg.curvature.softening;
  const maxAccel = sceneCfg.curvature.maxAccel;
  const radial = sceneCfg.expansion.baseSpeed + mid * sceneCfg.expansion.midToSpeed;

  for (let i = 0; i < activeParticles; ++i) {
    if (!alive[i]) {
      if (births > 0) {
        spawn(i);
        births -= 1;
      }
      continue;
    }

    let ax = 0;
    let ay = 0;
    let az = 0;
    const wells = [[wellA, massA], [wellB, massB]];
    for (let w = 0; w < wells.length; ++w) {
      const center = wells[w][0];
      const dx = center.x - px[i];
      const dy = center.y - py[i];
      const dz = center.z - pz[i];
      const r2 = dx * dx + dy * dy + dz * dz + soft;
      const inv = wells[w][1] / r2;
      const invR = 1 / Math.sqrt(r2);
      ax += dx * invR * inv;
      ay += dy * invR * inv;
      az += dz * invR * inv;
    }
    const accel = Math.hypot(ax, ay, az);
    if (accel > maxAccel) {
      const scale = maxAccel / accel;
      ax *= scale;
      ay *= scale;
      az *= scale;
    }

    const radius = Math.hypot(px[i], py[i], pz[i]) + 0.001;
    vx[i] += (ax + (px[i] / radius) * radial) * dt;
    vy[i] += (ay + (py[i] / radius) * radial * 0.35) * dt;
    vz[i] += (az + (pz[i] / radius) * radial) * dt;

    const speed = Math.hypot(vx[i], vy[i], vz[i]);
    const maxSpeed = 4.5;
    if (speed > maxSpeed) {
      const scale = maxSpeed / speed;
      vx[i] *= scale;
      vy[i] *= scale;
      vz[i] *= scale;
    }

    px[i] += vx[i] * dt;
    py[i] += vy[i] * dt;
    pz[i] += vz[i] * dt;
    age[i] += dt;

    const base = i * trailLength * 4;
    if (trailPhase % 4 === 0) {
      for (let point = trailLength - 1; point > 0; --point) {
        trail[base + point * 4] = trail[base + (point - 1) * 4];
        trail[base + point * 4 + 1] = trail[base + (point - 1) * 4 + 1];
        trail[base + point * 4 + 2] = trail[base + (point - 1) * 4 + 2];
        trail[base + point * 4 + 3] = trail[base + (point - 1) * 4 + 3];
      }
    }
    trail[base] = px[i];
    trail[base + 1] = py[i];
    trail[base + 2] = pz[i];
    trail[base + 3] = Math.hypot(vx[i], vy[i], vz[i]);

    if (Math.hypot(px[i], py[i], pz[i]) > sceneCfg.maxRadius || age[i] > sceneCfg.maxAge)
      alive[i] = 0;
  }
}

function speedColor(speed, out) {
  const t = clamp01((speed - sceneCfg.speedColor.slow) / (sceneCfg.speedColor.fast - sceneCfg.speedColor.slow));
  out.copy(CYAN).lerp(MAGENTA, t);
  return out;
}

function writeParticles() {
  const positions = pointGeometry.attributes.position.array;
  const sizes = pointGeometry.attributes.aSize.array;
  const colors = pointGeometry.attributes.aColor.array;
  const trailPositions = trailGeometry.attributes.position.array;
  const trailColors = trailGeometry.attributes.color.array;
  const color = new THREE.Color();
  const white = new THREE.Color(0xffffff);
  const shimmerAmp = treble * sceneCfg.shimmer.trebleAmount;
  const shimmerSpeed = sceneCfg.shimmer.trebleSpeed;
  const pointSize = sceneCfg.size.base + mid * sceneCfg.size.mid + treble * sceneCfg.size.treble;
  let brightness = sceneCfg.brightness.base + mid * sceneCfg.brightness.mid + treble * sceneCfg.brightness.treble;
  brightness = Math.min(VISUAL.dynamics.maxBrightness, brightness);
  brightness = Math.min(brightness, VISUAL.dynamics.maxParticleProduct / Math.max(1, activeParticles));
  brightness *= lumaGain;
  let vertex = 0;

  for (let i = 0; i < particleCount; ++i) {
    const visible = i < activeParticles && alive[i];
    const wobble = Math.sin(simTime * shimmerSpeed + seed[i]) * shimmerAmp;
    const sparkle = visible && Math.random() < treble * sceneCfg.sparkle.trebleChance * 0.08;
    const drawX = px[i] + Math.cos(seed[i]) * wobble;
    const drawY = py[i] + Math.sin(seed[i] * 1.7) * wobble;
    const drawZ = pz[i] + Math.sin(seed[i]) * wobble;
    positions[i * 3] = visible ? drawX : 0;
    positions[i * 3 + 1] = visible ? drawY : 0;
    positions[i * 3 + 2] = visible ? drawZ : 0;
    sizes[i] = visible ? pointSize * (sparkle ? 1.8 : 1) : 0;
    speedColor(Math.hypot(vx[i], vy[i], vz[i]), color);
    if (sparkle)
      color.lerp(white, 0.65);
    color.multiplyScalar(visible ? brightness : 0);
    colors[i * 3] = color.r;
    colors[i * 3 + 1] = color.g;
    colors[i * 3 + 2] = color.b;

    for (let segment = 0; segment < trailLength - 1; ++segment) {
      const draw = visible && segment >= trailLength - activeTrails;
      const a = i * trailLength * 4 + segment * 4;
      const fade0 = draw ? (segment + 1) / trailLength : 0;
      const fade1 = draw ? (segment + 2) / trailLength : 0;
      speedColor(trail[a + 3], color);
      trailPositions[vertex] = trail[a];
      trailColors[vertex++] = color.r * fade0 * brightness;
      trailPositions[vertex] = trail[a + 1];
      trailColors[vertex++] = color.g * fade0 * brightness;
      trailPositions[vertex] = trail[a + 2];
      trailColors[vertex++] = color.b * fade0 * brightness;
      speedColor(trail[a + 7], color);
      trailPositions[vertex] = trail[a + 4];
      trailColors[vertex++] = color.r * fade1 * brightness;
      trailPositions[vertex] = trail[a + 5];
      trailColors[vertex++] = color.g * fade1 * brightness;
      trailPositions[vertex] = trail[a + 6];
      trailColors[vertex++] = color.b * fade1 * brightness;
    }
  }

  pointGeometry.attributes.position.needsUpdate = true;
  pointGeometry.attributes.aSize.needsUpdate = true;
  pointGeometry.attributes.aColor.needsUpdate = true;
  trailGeometry.attributes.position.needsUpdate = true;
  trailGeometry.attributes.color.needsUpdate = true;
}

function writeRibbon(mesh, samples, width, opacity) {
  const positions = mesh.geometry.attributes.position.array;
  const count = Math.min(samples.length, 128);
  mesh.material.opacity = opacity;
  mesh.material.color.copy(accentColor);
  if (count < 2) {
    positions.fill(0);
    mesh.geometry.attributes.position.needsUpdate = true;
    return;
  }
  const span = ortho.right - ortho.left;
  let cursor = 0;
  for (let i = 0; i < count - 1; ++i) {
    const x0 = ortho.left + span * (i / (count - 1));
    const x1 = ortho.left + span * ((i + 1) / (count - 1));
    const y0 = Math.max(-1, Math.min(1, samples[i])) * 0.78;
    const y1 = Math.max(-1, Math.min(1, samples[i + 1])) * 0.78;
    const tri = [
      x0, y0 - width, 0, x1, y1 - width, 0, x1, y1 + width, 0,
      x0, y0 - width, 0, x1, y1 + width, 0, x0, y0 + width, 0,
    ];
    for (let v = 0; v < tri.length; ++v)
      positions[cursor++] = tri[v];
  }
  mesh.geometry.attributes.position.needsUpdate = true;
}

function resize() {
  const width = Math.max(1, window.innerWidth);
  const height = Math.max(1, window.innerHeight);
  renderer.setSize(width, height, false);
  composer.setSize(width, height);
  perspective.aspect = width / height;
  perspective.updateProjectionMatrix();
  const viewHeight = 1.05;
  ortho.top = viewHeight;
  ortho.bottom = -viewHeight;
  ortho.left = -viewHeight * (width / height);
  ortho.right = viewHeight * (width / height);
  ortho.updateProjectionMatrix();
}

function applyQuality(high) {
  qualityHigh = high;
  bloomPass.enabled = high;
  activeParticles = high ? sceneCfg.particlesHigh : sceneCfg.particlesLow;
  activeTrails = high ? sceneCfg.trailLength : sceneCfg.trailLow;
  volumeMaterial.uniforms.uSteps.value = high ? gradient.stepsHigh : gradient.stepsLow;
  volumeMaterial.uniforms.uOctaves.value = high ? gradient.octavesHigh : gradient.octavesLow;
}

function applyFrame(dt) {
  const frame = latest;
  const rawBass = frame && typeof frame.bass === "number" ? frame.bass : 0;
  const rawMid = frame && typeof frame.mid === "number" ? frame.mid : 0;
  const rawTreble = frame && typeof frame.treble === "number" ? frame.treble : 0;
  const rawLevel = frame && typeof frame.level === "number" ? frame.level : 0;
  bass = follow(bass, compress(rawBass, bandState.bass, dt), dt, VISUAL.attack, VISUAL.release);
  mid = follow(mid, compress(rawMid, bandState.mid, dt), dt, VISUAL.attack, VISUAL.release);
  treble = follow(treble, compress(rawTreble, bandState.treble, dt), dt, VISUAL.attack, VISUAL.release);
  level = follow(level, compress(rawLevel, bandState.level, dt), dt, VISUAL.attack, VISUAL.release);
  accent += (level - accent) * (1 - Math.exp(-VISUAL.accentFollow * dt));
  accentColor.setHSL(lerp(0.51, 0.89, clamp01(accent)), 0.86, 0.62);

  if (frame && (frame.mode === WAVE || frame.mode === BARS || frame.mode === SCENE))
    mode = frame.mode;

  const nativeSpeakers = frame && Array.isArray(frame.speakers) ? frame.speakers : [];
  const nativeIds = new Set(nativeSpeakers.map((spot) => spot && spot.id));
  const incoming = demoSpeakers.length
    ? nativeSpeakers.concat(demoSpeakers.filter((spot) => spot && !nativeIds.has(spot.id)))
    : nativeSpeakers;
  syncSpeakers(incoming);

  if (caption) {
    caption.textContent = MODE_LABEL[mode] || "Waveform";
    caption.classList.toggle("away", !!(frame && frame.fullscreen));
    caption.style.boxShadow = `0 0 0 1px rgba(${Math.round(accentColor.r * 255)}, ${Math.round(accentColor.g * 255)}, ${Math.round(accentColor.b * 255)}, 0.45)`;
  }

  const samples = frame && Array.isArray(frame.wave) ? frame.wave : null;
  if (samples && samples.length > 1 && frameSerial !== drawnSerial) {
    waveHistory.unshift(samples.slice());
    if (waveHistory.length > ribbonCount)
      waveHistory.pop();
    drawnSerial = frameSerial;
  }
  const shownTrails = qualityHigh ? VISUAL.waveform.trails : VISUAL.waveform.trailsLow;
  const waveDrive = (0.4 + 0.6 * level) * lumaGain;
  for (let i = 0; i < ribbons.length; ++i) {
    const samplesForRibbon = waveHistory[i];
    const fade = i < shownTrails && samplesForRibbon ? Math.pow(0.72, i) * waveDrive : 0;
    writeRibbon(ribbons[i], samplesForRibbon || [0, 0], VISUAL.waveform.halfWidth * (1 - i * 0.04), fade);
  }

  const bins = frame && Array.isArray(frame.bins) ? frame.bins : [];
  const floor = -1.55;
  for (let i = 0; i < barCount; ++i) {
    const shaped = compress(Math.max(0, bins[i] || 0), binState[i], dt);
    smoothBins[i] = follow(smoothBins[i], shaped, dt, VISUAL.attack, VISUAL.release);
    const height = 0.08 + smoothBins[i] * VISUAL.bars.height;
    const x = -6.4 + (12.8 * (i + 0.5)) / barCount;
    barDummy.position.set(x, floor + height * 0.5, 0);
    barDummy.scale.set(1, height, 1);
    barDummy.updateMatrix();
    barMesh.setMatrixAt(i, barDummy.matrix);
    barColor.copy(VIOLET).lerp(accentColor, 0.35 + smoothBins[i] * 0.65);
    barMesh.setColorAt(i, barColor);

    barDummy.position.set(x, floor - height * 0.22, 0);
    barDummy.scale.set(1, height * 0.38, 1);
    barDummy.updateMatrix();
    barReflection.setMatrixAt(i, barDummy.matrix);
    barColor.multiplyScalar(0.35);
    barReflection.setColorAt(i, barColor);
  }
  barMesh.instanceMatrix.needsUpdate = true;
  barReflection.instanceMatrix.needsUpdate = true;
  if (barMesh.instanceColor)
    barMesh.instanceColor.needsUpdate = true;
  if (barReflection.instanceColor)
    barReflection.instanceColor.needsUpdate = true;

  simTime += dt;
  integrateParticles(dt);
  writeParticles();
  const formScale = sceneCfg.form.baseScale + bass * sceneCfg.form.bassScale;
  form.scale.setScalar(formScale);
  form.rotation.y = simTime * 0.35;
  form.rotation.x = simTime * 0.18;
  form.material.color.copy(accentColor);

  const uniforms = volumeMaterial.uniforms;
  uniforms.uCameraPos.value.copy(perspective.position);
  uniforms.uTime.value = simTime * gradient.timeSpeed * 8;
  uniforms.uAccent.value = accent;
  uniforms.uLevel.value = level;
  uniforms.uAddCap.value = VISUAL.dynamics.addCap;
  const energy = Math.min(VISUAL.dynamics.maxEnergy, (gradient.energy + gradient.loudnessEnergy * level) * lumaGain);
  uniforms.uEnergy.value = energy;
  uniforms.uLoudnessEnergy.value = 0;
  bloomPass.strength = Math.min(VISUAL.dynamics.maxBloom, (VISUAL.bloom.base + bass * VISUAL.bloom.bass) * lumaGain);
  bloomPass.radius = VISUAL.bloom.radius;
  bloomPass.threshold = VISUAL.bloom.threshold;
  renderer.toneMappingExposure = lumaGain;
}

function renderScene() {
  if (mode === BARS) {
    renderPass.scene = barScene;
    renderPass.camera = perspective;
  } else if (mode === SCENE) {
    renderPass.scene = worldScene;
    renderPass.camera = perspective;
  } else {
    renderPass.scene = waveScene;
    renderPass.camera = ortho;
  }
  composer.render();
}

function frame(now) {
  requestAnimationFrame(frame);
  const dt = Math.min(0.05, Math.max(0.001, (now - lastNow) / 1000));
  const frameMs = now - lastNow;
  lastNow = now;
  renderedFrames += 1;
  if (renderedFrames > VISUAL.warmupFrames) {
    if (frameMs > VISUAL.budgetMs) {
      slowFrames += 1;
      fastFrames = 0;
    } else if (frameMs < VISUAL.recoverMs) {
      fastFrames += 1;
      slowFrames = 0;
    }
    if (qualityHigh && slowFrames > 12)
      applyQuality(false);
    else if (!qualityHigh && fastFrames > 180)
      applyQuality(true);
  }
  applyFrame(dt);
  renderScene();
  if ((lumaTick++ % 10) === 0)
    lumaGain = guardLuminance(lumaGain, dt * 10);
}

function averageLuminance() {
  const sample = averageLuminance.canvas || (averageLuminance.canvas = document.createElement("canvas"));
  sample.width = 48;
  sample.height = 27;
  const ctx = sample.getContext("2d", { willReadFrequently: true });
  ctx.drawImage(renderer.domElement, 0, 0, sample.width, sample.height);
  const data = ctx.getImageData(0, 0, sample.width, sample.height).data;
  let sum = 0;
  const count = data.length / 4;
  for (let i = 0; i < data.length; i += 4)
    sum += 0.2126 * data[i] + 0.7152 * data[i + 1] + 0.0722 * data[i + 2];
  return sum / (count * 255);
}

function guardLuminance(gain, dt) {
  let measured = gain;
  try {
    measured = averageLuminance();
  } catch (error) {
    return gain;
  }
  const ceiling = VISUAL.dynamics.luminanceCeiling;
  if (measured > ceiling && measured > 1e-4) {
    return Math.max(VISUAL.dynamics.luminanceFloorGain, gain * (ceiling / measured));
  }
  const restored = gain + (1 - gain) * (1 - Math.exp(-VISUAL.dynamics.luminanceRelease * dt));
  return Math.min(1, restored);
}

window.addEventListener("resize", resize);
window.addEventListener("keydown", (event) => {
  if (event.key === "Escape") {
    event.preventDefault();
    callNative("exitFullscreen");
  }
}, true);

window.wavisCapture = () => renderer.domElement.toDataURL("image/png");
window.wavisLuminance = () => {
  try {
    return averageLuminance();
  } catch (error) {
    return -1;
  }
};
window.wavisProbe = (payload) => {
  probeLock = true;
  latest = payload;
  frameSerial += 1;
};
window.wavisDemoSpeakers = (list) => {
  demoSpeakers = Array.isArray(list) ? list : [];
};
window.wavisMoveSpeaker = (id, x, z) => {
  const nx = clampRoom(Number(x));
  const nz = clampRoom(Number(z));
  let found = false;
  demoSpeakers = demoSpeakers.map((spot) => {
    if (!spot || spot.id !== id)
      return spot;
    found = true;
    return { ...spot, x: nx, z: nz };
  });
  if (!found)
    demoSpeakers.push({ id, label: id, x: nx, z: nz });
  const group = speakerById.get(id);
  if (group)
    group.position.set(nx, room.floorY, nz);
  callNative("placeSpeaker", [id, nx, nz]);
  return nx + "," + nz;
};

const backend = window.__JUCE__ && window.__JUCE__.backend;
if (backend && typeof backend.addEventListener === "function") {
  backend.addEventListener("wavisFrame", (payload) => {
    if (probeLock)
      return;
    latest = payload;
    frameSerial += 1;
  });
}

resize();
waveHistory.push(new Array(128).fill(0));
requestAnimationFrame(frame);
