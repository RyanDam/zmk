/* Framing vectors from ref/zmk-studio-ts-client/test/framing.spec.ts —
 * run with: npm run framing-test */
import assert from "node:assert";
import { encodeFrame, FrameDecoder } from "../src/framing.js";

const hex = (u8) => Buffer.from(u8).toString("hex");

// Vector from the reference client's spec.
assert.equal(hex(encodeFrame([1, 0xab, 0xac, 2])), "ab01acabacac02ad");

// Round-trip through chunked delivery (arbitrary splits).
const payload = new Uint8Array(Array.from({ length: 200 }, (_, i) => (i * 37) % 256));
const wire = encodeFrame(payload);
const dec = new FrameDecoder();
let got = [];
for (let i = 0; i < wire.length; i += 7) {
  got = got.concat(dec.push(wire.subarray(i, i + 7)));
}
assert.equal(got.length, 1);
assert.ok(Buffer.from(got[0]).equals(Buffer.from(payload)), "round-trip payload");

// Junk before SOF is dropped (resync), then a clean frame decodes.
const dec2 = new FrameDecoder();
const out2 = dec2.push(Uint8Array.from([0x11, 0x22, ...encodeFrame([7, 8])]));
assert.equal(out2.length, 1);
assert.equal(hex(out2[0]), "0708");

// Two frames in one chunk.
const dec3 = new FrameDecoder();
const out3 = dec3.push(
  Uint8Array.from([...encodeFrame([1]), ...encodeFrame([2, 3])])
);
assert.equal(out3.length, 2);
assert.equal(hex(out3[1]), "0203");

console.log("framing: 5/5 vectors pass");
