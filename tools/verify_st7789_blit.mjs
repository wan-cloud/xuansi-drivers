#!/usr/bin/env node
// verify_st7789_blit.mjs — st7789 external-blit window/.bss overlap regression
// gate (P3-T2; B12-P1 T8 FINDING-B).
//
// Background: st7789@1.0.0's blit advertised a legal external source window
// [8, 146944] — which covers the module's OWN statics (measured layout of the
// shipped artifact: .rodata/.data [1024,1217) incl. every reply-map key
// literal, scalars + args_slot + cmd_buf + the 115,200B fb up to 116600,
// out_buf at 116608, stage_buf ending at 122752, wasm stack [122752,188288)).
// The B12-P2 T8 consumer recipe (jpge.grab -> wasm.xfer -> st7789 @off8,
// 115,200B -> blit(8)) therefore wrote the frame straight over .rodata and
// the module-state scalars: the post-xfer blit reply came back with null-byte
// keys (the "ok"/"bytes" literals were frame pixels now) and subsequent calls
// degraded until reload — while the SPI stream itself fired fine (fb_stream
// precedes result encode).
//
// Fix under test: blit's nonzero window is clamped to [xfer_lo, 146944] where
// xfer_lo = statics_end() — the module's self-derived statics ceiling (max
// end over every buffer + scalar, ADDRESSES ONLY, never contents — dirty-.bss
// and relayout-proof) — and init's reply now publishes xfer_lo so consumers
// xfer into the overlap-free band. The frame span intentionally covers the
// wasm stack above it: inter-call stack bytes are dead by wasm semantics
// (frames are rebuilt per call), verified here by consecutive-call health.
//
// Scenarios (each = one or more checks; any FAIL fails the gate):
//   S1  layout parse + overlap arithmetic: the xfer destination must sit at
//       or above the parsed statics span end (stack bottom) — overlap zero.
//   S2  composition: xfer a 115,200B frame at the advertised floor, blit it,
//       assert a healthy reply map (ok/bytes/chunks) AND that the SPI stream
//       carried all 115,200 bytes in 29 chunks from the right offsets.
//   S3  consecutive-call health n>=3 after the composition (clear, border
//       pattern, blit again) — every map decodes with clean keys.
//   S4  dirty-boot: same composition under 4 evidence-shaped .bss patterns
//       (zero control included) — the fix must not trust initial statics.
//   S5  bounds honesty: below-floor and above-ceiling offsets answer the
//       clean error map, never a stream.
//   S6  fb path regression: clear + blit() still stream the module fb.
//
// Usage: node tools/verify_st7789_blit.mjs [wasm-path]
//   Against the FIXED artifact: all scenarios PASS.
//   Against the pre-fix artifact (git show 3198e10:modules/st7789/1.0.0/
//   st7789.wasm): S1/S2/S3 FAIL (the recorded finding), S5 below-floor FAILs
//   (off=8 was accepted there) — the RED side of this gate.
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..');
const wasmPath = process.argv[2] ?? join(root, 'modules/st7789/1.0.0/st7789.wasm');

const FB = 115200, LIN_END = 262144, CEIL = LIN_END - FB; // 146944

// ---- parse the wasm: data segments, __stack_pointer, initial memory.
// statics span = [global_base_floor 1024, stack_bottom); wasm-ld appends the
// 64KB stack directly above data+.bss, so stack_bottom = sp - 64KB is the
// statics ceiling byte-for-byte (sp-0x10000 == .bss end for these builds).
function parseLayout(d) {
  const u32 = (i) => {
    let r = 0, s = 0;
    for (;;) { const b = d[i++]; r |= (b & 0x7f) << s; if (!(b & 0x80)) return [r, i]; s += 7; }
  };
  let i = 8, dataEnd = 0, dataLo = Infinity, sp = null, memInit = 0;
  while (i < d.length) {
    const sid = d[i++]; const [sz, j0] = u32(i); i = j0; const end = i + sz;
    if (sid === 5) {
      let j = i; const cnt = u32(j)[0]; j = u32(j)[1];
      if (cnt >= 1) {
        const flags = d[j]; j++;                    // 0x00 min-only, 0x01 min+max
        const [minPages, j2] = u32(j); j = j2;
        if (flags & 1) { const [maxPages, j3] = u32(j); j = j3; }
        memInit = minPages * 65536;
      }
    }
    if (sid === 6) {
      let j = i; const cnt = u32(j)[0]; j = u32(j)[1];
      if (cnt >= 1) {
        j += 2;
        if (d[j] === 0x41) { j++; let r = 0, s = 0, b; do { b = d[j++]; r |= (b & 0x7f) << s; s += 7; } while (b & 0x80); if (b & 0x40) r -= (1 << s); sp = r >>> 0; }
      }
    }
    if (sid === 11) {
      let j = i; const cnt = u32(j)[0]; j = u32(j)[1];
      for (let k = 0; k < cnt; k++) {
        const flags = d[j]; j++;
        if (flags === 0 && d[j] === 0x41) {
          j++; let r = 0, s = 0, b; do { b = d[j++]; r |= (b & 0x7f) << s; s += 7; } while (b & 0x80); if (b & 0x40) r -= (1 << s); j++;
          const [n, j2] = u32(j); j = j2;
          dataLo = Math.min(dataLo, r >>> 0); dataEnd = Math.max(dataEnd, (r >>> 0) + n);
        } else break;
      }
    }
    i = end;
  }
  if (sp === null || dataEnd === 0) throw new Error('layout parse failed');
  return { dataLo, dataEnd, stackBottom: sp - 0x10000, sp, memInit };
}

// ---- minimal msgpack decode of a result map: {key: int|string} ----
function decodeMap(buf) {
  const out = {};
  let p = 0;
  const rd = () => { if (p >= buf.length) throw new Error('eof'); return buf[p++]; };
  const t = rd();
  if ((t & 0xf0) !== 0x80) throw new Error(`not fixmap: 0x${t.toString(16)}`);
  const n = t & 0x0f;
  for (let k = 0; k < n; k++) {
    const kt = rd();
    if ((kt & 0xe0) !== 0xa0) throw new Error(`key not fixstr: 0x${kt.toString(16)}`);
    const key = Buffer.from(buf.subarray(p, p + (kt & 0x1f))).toString('latin1'); p += kt & 0x1f;
    const vt = rd();
    if ((vt & 0xe0) === 0xa0) { out[key] = Buffer.from(buf.subarray(p, p + (vt & 0x1f))).toString('latin1'); p += vt & 0x1f; }
    else if ((vt & 0x80) === 0 || vt >= 0xe0) { out[key] = vt >= 0xe0 ? (vt - 256) : vt; }
    else if (vt === 0xcd) { out[key] = (rd() << 8) | rd(); }
    else if (vt === 0xce) { out[key] = ((rd() * 0x1000000) + (rd() << 16) + (rd() << 8) + rd()); }
    else throw new Error(`value type 0x${vt.toString(16)}`);
  }
  return out;
}

const FRAME = (() => {          // synthetic full frame, hi-byte-first RGB565
  const f = Buffer.alloc(FB);
  for (let i = 0; i < FB / 2; i++) {
    const px = (i & 1) ? 0x0aa0 : 0xa0a0;
    f[2 * i] = px >> 8; f[2 * i + 1] = px & 0xff;
  }
  return f;
})();

let fails = 0, checks = 0;
function check(ok, name, detail = '') {
  checks++;
  console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail ? ' | ' + detail : ''}`);
  if (!ok) fails++;
}

async function runScenario(name, dirtyPat) {
  const wasm = readFileSync(wasmPath);
  const L = parseLayout(wasm);
  let mem, ex, ms = 0;
  const xfers = [];
  const dv = () => new DataView(mem.buffer);
  const env = {
    env: {
      memset: (d, v, n) => { new Uint8Array(mem.buffer, d, n).fill(v); return d; },
      memcpy: (d, s, n) => { new Uint8Array(mem.buffer, d, n).set(new Uint8Array(mem.buffer, s, n)); return d; },
    },
    xuansi: {
      spi_config: (o) => dv().setUint32(o, 0, true),
      spi_xfer: (o, slot, wbuf, len) => { xfers.push({ off: wbuf, len }); dv().setUint32(o, len, true); },
      gpio_set: () => {},
      delay_ms: () => {},
      millis: (o) => dv().setUint32(o, ++ms, true),
    },
  };
  ({ instance: { exports: ex, exports: { memory: mem } } } = await WebAssembly.instantiate(wasm, env));

  // dirty boot: fill .bss (and the whole upper span — the window region too)
  // with the pattern BEFORE any call; data segments already applied above.
  const m8 = new Uint8Array(mem.buffer);
  if (dirtyPat) for (let a = L.dataEnd; a < L.memInit; a++) m8[a] = dirtyPat(a) & 0xff;

  const call = (fn, argsBuf) => {
    const aptr = ex.xuansi_alloc(argsBuf.length);
    if (aptr === 0) throw new Error('alloc refused');
    new Uint8Array(mem.buffer, aptr, argsBuf.length).set(argsBuf);
    const packed = ex[fn](aptr, argsBuf.length);
    const rptr = Number((packed >> 32n) & 0xffffffffn);
    const rlen = Number(packed & 0xffffffffn);
    if (rptr + rlen > mem.buffer.byteLength) throw new Error(`result span OOB (${rptr}+${rlen})`);
    return { map: decodeMap(Buffer.from(new Uint8Array(mem.buffer, rptr, rlen))), rptr };
  };
  const intArg = (v) => {                          // msgpack int at natural width
    if (v >= 0 && v < 128) return [v];
    if (v >= 0 && v < 0x10000) return [0xcd, v >> 8, v & 0xff];
    if (v >= 0) return [0xce, v >>> 24, (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff];
    return [0xd0, v & 0xff];                        // int8 (negatives)
  };
  const int1 = (fn, v) => call(fn, Buffer.from([0x91, ...intArg(v)]));
  const str1 = (fn, s) => call(fn, Buffer.from([0x91, 0xa0 | s.length, ...Buffer.from(s)]));
  const noargs = (fn) => call(fn, Buffer.from([0x90]));

  // init — the fixed module publishes xfer_lo; the pre-fix artifact does not
  // (its then-advertised floor was 8).
  const initR = noargs('init');
  const xferLo = typeof initR.map.xfer_lo === 'number' ? initR.map.xfer_lo : 8;

  // S1: overlap-zero arithmetic — destination at/above the parsed statics end
  check(xferLo >= L.stackBottom,
    `${name} S1 xfer destination >= statics end (overlap zero)`,
    `xfer_lo=${xferLo} statics=[${L.dataLo},${L.stackBottom}) stackTop=${L.sp} mem=${L.memInit}`);
  check(xferLo + FB <= LIN_END,
    `${name} S1b window nonempty under the frozen ceiling`,
    `xfer_lo=${xferLo} <= ${CEIL}`);

  // S2: the T8 composition — host-side frame write at the floor, then blit
  xfers.length = 0;
  m8.set(FRAME, xferLo);
  let ok = true, detail = '';
  try {
    const r = int1('blit', xferLo);
    ok = r.map.ok === 1 && r.map.bytes === FB && r.map.chunks === 29;
    detail = JSON.stringify(r.map);
  } catch (e) { ok = false; detail = `reply undecodable: ${e.message}`; }
  const streamBytes = xfers.filter(x => x.len === 4096 || x.len === 512 || x.len === FB).reduce((s, x) => s + x.len, 0);
  const dataChunks = xfers.filter(x => x.len === 4096 || x.len === 512).length;
  check(ok, `${name} S2 post-xfer blit reply healthy`, detail);
  check(streamBytes === FB && dataChunks === 29 && xfers.some(x => x.off === xferLo),
    `${name} S2b SPI stream fired: 115,200B / 29 chunks from the window`,
    `bytes=${streamBytes} chunks=${dataChunks} firstOff=${xfers.find(x => x.len > 8)?.off}`);

  // S3: consecutive consumer calls after the composition (n=3)
  let healthy = 0;
  const shapes = [];
  for (const step of [
    () => int1('clear', 0), () => str1('pattern', 'border'), () => int1('blit', xferLo),
  ]) {
    try {
      const r = step();
      const good = typeof r.map.ok === 'number' && r.map.ok === 1 && !('error' in r.map);
      if (good) healthy++;
      shapes.push(JSON.stringify(r.map));
    } catch (e) { shapes.push(`undecodable: ${e.message}`); }
  }
  check(healthy === 3, `${name} S3 consecutive calls after composition (n=3)`,
    shapes.join(' ; ').slice(0, 220));

  // S5: bounds honesty (clean refusals, no stream)
  xfers.length = 0;
  let refused = false;
  try { const r = int1('blit', Math.max(8, xferLo - 1)); refused = r.map.error === 'blit offset out of bounds'; } catch (e) { refused = false; }
  const noStream = !xfers.some(x => x.len === 4096);
  check(refused && noStream, `${name} S5a below-floor offset refused cleanly`, `floor=${xferLo}`);
  xfers.length = 0;
  try { const r = int1('blit', CEIL + 1); refused = r.map.error === 'blit offset out of bounds'; } catch (e) { refused = false; }
  check(refused, `${name} S5b above-ceiling offset refused cleanly`, `ceil=${CEIL}`);

  // S6: fb path regression
  try {
    const r1 = int1('clear', 0);
    const r2 = noargs('blit');
    check(r1.map.ok === 1 && r1.map.bytes === FB && r2.map.ok === 1 && r2.map.chunks === 29,
      `${name} S6 fb path intact (clear + blit())`,
      `${JSON.stringify(r1.map)} ; ${JSON.stringify(r2.map)}`);
  } catch (e) { check(false, `${name} S6 fb path intact`, e.message); }
}

const patterns = {
  clean:   null,                                   // control: pristine boot
  zero:    () => 0,                                // zeroed-.bss control
  u32runs: (a) => (a % 4 === 0) ? 40 : 0,          // b12-p1 s2-module shape
  counter: (a) => (a + 159) & 0xff,                // b12-p2 s1-recipe shape
  msgpack: (a) => [0xdc, 0xa7, 0xaa, 0x91, 0x81, 98, 99, 100, 0x08][(a >> 2) % 9],
};

for (const [pname, pat] of Object.entries(patterns)) {
  try { await runScenario(pname, pat); }
  catch (e) { check(false, `${pname} scenario`, e.message); }
}
console.log(fails === 0
  ? `verify_st7789_blit: PASS (${checks} checks, 5 boot patterns, composition + overlap-zero arithmetic)`
  : `verify_st7789_blit: FAIL (${fails}/${checks})`);
process.exit(fails === 0 ? 0 : 1);
