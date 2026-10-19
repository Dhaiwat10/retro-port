// Plays the game's mixed audio: a ring of stereo float frames in the module's shared memory
// (see port_audio_ring in host/dsound.c: struct { u32 w, r, rate, frames; f32 data[frames * 2] }).
class PortRing extends AudioWorkletProcessor {
  constructor() {
    super();
    this.memory = null;
    this.base = 0;
    this.port.onmessage = (e) => { this.memory = e.data.memory; this.base = e.data.ring; };
  }
  process(inputs, outputs) {
    const out = outputs[0];
    const L = out[0], R = out[1] || out[0];
    if (!this.memory) { L.fill(0); R.fill(0); return true; }
    const buf = this.memory.buffer;   // a new buffer object after memory growth
    if (!this.u32 || this.u32.buffer !== buf) { this.u32 = new Uint32Array(buf); this.f32 = new Float32Array(buf); }
    const hdr = this.base >> 2, frames = this.u32[hdr + 3], data = hdr + 4;
    const w = Atomics.load(this.u32, hdr), r = Atomics.load(this.u32, hdr + 1);
    const avail = (w - r) >>> 0, n = Math.min(avail, L.length);
    for (let i = 0; i < n; i++) {
      const k = ((r + i) >>> 0) % frames;
      L[i] = this.f32[data + 2 * k];
      R[i] = this.f32[data + 2 * k + 1];
    }
    for (let i = n; i < L.length; i++) { L[i] = 0; R[i] = 0; }
    Atomics.store(this.u32, hdr + 1, (r + n) >>> 0);
    return true;
  }
}
registerProcessor('port-ring', PortRing);
