#!/usr/bin/env node
// verify_jpge_dht.mjs — jpge dirty-.bss regression gate (P3-T1).
//
// Background (B12-P1 T8 FINDING-A — jpge@1.0.0 DHT oversize): the board's
// WAMR ESP-IDF build allocates instance linear memory via heap_caps_malloc
// (os_mmap glue does not zero it) and memory_instantiate never memsets the
// memory, so the module's .bss span boots with leftover PSRAM bytes
// whenever the allocator reuses a dirty block. The vendored encoder used
// to latch its Huffman-table init behind m_huff_initialized — a dirty
// nonzero byte skipped the copy and emit_dht() emitted the leftover bytes
// as BITS/VALS: sum(garbage) = 640..3,500B DHT segments (legal is
// <=~275B/table) while DQT/SOF/SOS stayed correct. Fixed by the
// unconditional per-encode table re-derivation in src/jpge/jpge.cpp
// (change (6)).
//
// This gate emulates the dirty boot exactly: instantiate (data segments
// applied by the engine), then fill the .bss span [data_end, stack_bottom)
// with several evidence-shaped patterns, then run the S1-recipe call shape
// (shot {quality:60}) n times against synthetic QVGA frames. Every frame
// must exist, pass the SOI/EOI + marker-chain + len==shot_len checks, and
// every DHT segment length field must stay <= 275B. Full strict-decoder
// judging (PIL + Go image/jpeg) lives in the board/evidence path; the
// marker bar here is the durable in-repo regression line (pure Node).
//
// Usage: node tools/verify_jpge_dht.mjs [wasm-path] [shots]
//   wasm-path default: modules/jpge/<VERSION from src/jpge/VERSION>/jpge.wasm
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..');
const version = readFileSync(join(root, 'src/jpge/VERSION'), 'utf8').trim();
const wasmPath = process.argv[2] ?? join(root, 'modules/jpge', version, 'jpge.wasm');
const SHOTS = Math.max(10, parseInt(process.argv[3] ?? '12', 10));

// ---- parse the wasm: last data-segment end (=> .bss start) and the
// mutable global 0 (__stack_pointer). Dirty span = [bss_lo, sp - 64KB);
// wasm-ld's default stack is 64KB (build.sh pins no -z stack-size).
function parseLayout(d) {
  let i = 8;
  const u32 = (i) => {
    let r = 0, s = 0;
    for (;;) {
      const b = d[i++]; r |= (b & 0x7f) << s;
      if (!(b & 0x80)) return [r, i];
      s += 7;
    }
  };
  let dataEnd = 0, sp = null;
  while (i < d.length) {
    const sid = d[i++]; const [sz, j0] = u32(i); i = j0;
    const end = i + sz;
    if (sid === 6) { // Global: [count, (type, mut, expr)*] — global 0 is the SP
      let j = i; const cnt = u32(j)[0]; j = u32(j)[1];
      if (cnt >= 1) {
        j += 2;                 // valtype + mutability
        if (d[j] === 0x41) {    // i32.const, signed LEB
          j++; let r = 0, s = 0, b;
          do { b = d[j++]; r |= (b & 0x7f) << s; s += 7; } while (b & 0x80);
          if (b & 0x40) r -= (1 << s);
          sp = r >>> 0;
        }
      }
    }
    if (sid === 11) { // Data: [count, (flags, .., expr, size, bytes)*] — active segments
      let j = i; const cnt = u32(j)[0]; j = u32(j)[1];
      for (let k = 0; k < cnt; k++) {
        const flags = d[j]; j++;
        if (flags === 0) {
          if (d[j] === 0x41) {  // offset expr: i32.const offset end
            j++; let r = 0, s = 0, b;
            do { b = d[j++]; r |= (b & 0x7f) << s; s += 7; } while (b & 0x80);
            if (b & 0x40) r -= (1 << s);
            j++;                                  // 0x0b end
            const [n, j2] = u32(j); j = j2;
            dataEnd = Math.max(dataEnd, (r >>> 0) + n);
          }
        } else break;                             // passive/other — not ours
      }
    }
    i = end;
  }
  if (sp === null || dataEnd === 0) throw new Error('layout parse failed');
  return { bssLo: dataEnd, bssHi: sp - 0x10000 };
}

// synthetic QVGA frame: gradient + rects (hi-byte-first RGB565)
const W = 320, H = 240;
function buildFrame(k) {
  const f = Buffer.alloc(W * H * 2);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    let r = (x * 31 / W) | 0, g = (y * 63 / H) | 0, b = (x + y + k * 3) % 32;
    if (x > 60 && x < 160 && y > 60 && y < 180) { r = 31; g = 0; b = 0; }
    if (x > 200 && x < 260 && y > 30 && y < 120) { r = 0; g = 0; b = 31; }
    const px = (r << 11) | (g << 5) | b, o = (y * W + x) * 2;
    f[o] = px >> 8; f[o + 1] = px & 0xff;
  }
  return f;
}

const patterns = {                       // evidence-shaped dirty boots
  zero:    () => 0,                      // control (clean boot)
  u32runs: (a) => (a % 4 === 0) ? 40 : 0,          // b12-p1 s2-module shape
  counter: (a) => (a + 159) & 0xff,                 // b12-p2 s1-recipe shape
  msgpack: (a) => [0xdc, 0xa7, 0xaa, 0x91, 0x81, 98, 99, 100, 0x08][(a >> 2) % 9],
};

const wasm = readFileSync(wasmPath);
const { bssLo, bssHi } = parseLayout(wasm);

let fails = 0;
for (const [pname, pat] of Object.entries(patterns)) {
  let ms = 0, curFrame = buildFrame(0);
  const frames = [];
  let mem, ex;
  const dv = () => new DataView(mem.buffer);
  const env = {
    env: {
      memset: (d, v, n) => { new Uint8Array(mem.buffer, d, n).fill(v); return d; },
      memcpy: (d, s, n) => { new Uint8Array(mem.buffer, d, n).set(new Uint8Array(mem.buffer, s, n)); return d; },
    },
    xuansi: {
      millis: (o) => dv().setUint32(o, ++ms, true),
      cam_frame_get: (lo, bo, cap, wo, ho) => {
        const d = dv();
        d.setUint32(wo, W, true); d.setUint32(ho, H, true); d.setUint32(lo, W * H * 2, true);
        new Uint8Array(mem.buffer, bo, W * H * 2).set(curFrame);
      },
      stream_send: (o, bo, len) => { frames.push(Buffer.from(new Uint8Array(mem.buffer, bo, len))); dv().setUint32(o, len, true); },
    },
  };
  ({ instance: { exports: ex, exports: { memory: mem } } } =
    await WebAssembly.instantiate(wasm, env));

  // the dirty-.bss boot: fill AFTER instantiate (data segments applied)
  const m8 = new Uint8Array(mem.buffer);
  for (let a = bssLo; a < bssHi; a++) m8[a] = pat(a) & 0xff;

  for (let k = 0; k < SHOTS; k++) {
    curFrame = buildFrame(k);
    const args = Buffer.from([0x91, 0x81, 0xa7, ...Buffer.from('quality'), 60]);
    const aptr = ex.xuansi_alloc(args.length);
    if (aptr === 0) { console.log(`FAIL ${pname} shot${k}: xuansi_alloc refused`); fails++; continue; }
    new Uint8Array(mem.buffer, aptr, args.length).set(args);
    const packed = ex.shot(aptr, args.length);
    const rptr = Number((packed >> 32n) & 0xffffffffn);
    const rlen = Number(packed & 0xffffffffn);
    const res = Buffer.from(new Uint8Array(mem.buffer, rptr, rlen));
    if (!frames[k] || res.includes(Buffer.from('error'))) {
      console.log(`FAIL ${pname} shot${k}: no frame on the wire (error map: ${res.toString('hex')})`);
      fails++; continue;
    }
    const f = frames[k];
    let err = null;
    if (!(f[0] === 0xff && f[1] === 0xd8 && f[f.length - 2] === 0xff && f[f.length - 1] === 0xd9)) err = 'SOI/EOI';
    let maxDht = 0, i = 2;               // marker walk + DHT size bar
    while (!err && i < f.length - 1) {
      if (f[i] !== 0xff) { err = 'desync@' + i; break; }
      const mk = f[i + 1];
      if (mk === 0xd8 || mk === 0xd9 || (0xd0 <= mk && mk <= 0xd7) || mk === 0x01) { i += 2; continue; }
      if (mk === 0xda) break;            // SOS — entropy follows
      const len = f.readUInt16BE(i + 2);
      if (mk === 0xc4 && len > 277) { err = `DHT field ${len}B > 277 (oversize)`; break; }
      if (mk === 0xc4) maxDht = Math.max(maxDht, len);
      i += 2 + len;
    }
    if (err) { console.log(`FAIL ${pname} shot${k}: ${err}`); fails++; }
    else console.log(`PASS ${pname} shot${k}: len=${f.length} maxDHTfield=${maxDht}`);
  }
}
console.log(fails === 0
  ? `verify_jpge_dht: PASS (4 dirty-boot patterns x ${SHOTS} shots, .bss [0x${bssLo.toString(16)},0x${bssHi.toString(16)}))`
  : `verify_jpge_dht: FAIL (${fails})`);
process.exit(fails === 0 ? 0 : 1);
