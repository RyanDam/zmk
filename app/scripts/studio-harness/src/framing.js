/*
 * ZMK Studio wire framing (backlog a14 harness).
 *
 * Ported from zmk/app/src/studio/msg_framing.c: SOF 0xAB, ESC 0xAC,
 * EOF 0xAD; special bytes inside a frame are sent as ESC <byte>.
 * The reference client's vectors (ref/zmk-studio-ts-client/
 * test/framing.spec.ts) confirm the escape-before semantics.
 */

export const SOF = 0xab;
export const ESC = 0xac;
export const EOF = 0xad;

const SPECIAL = new Set([SOF, ESC, EOF]);

/** Encode a payload into one framed packet. */
export function encodeFrame(payload) {
  const out = [SOF];
  for (const b of payload) {
    if (SPECIAL.has(b)) {
      out.push(ESC);
    }
    out.push(b);
  }
  out.push(EOF);
  return Uint8Array.from(out);
}

/**
 * Incremental decoder: feed arbitrary chunks, get whole payloads out.
 * Throws on framing errors (SOF mid-frame) so callers can resync.
 */
export class FrameDecoder {
  #idle = true;
  #escaped = false;
  #buf = [];
  #out = [];

  push(chunk) {
    this.#out = [];
    for (const b of chunk) {
      if (this.#idle) {
        if (b === SOF) {
          this.#idle = false;
          this.#escaped = false;
          this.#buf = [];
        }
        continue; // resync: junk before SOF is dropped
      }
      if (this.#escaped) {
        this.#buf.push(b);
        this.#escaped = false;
        continue;
      }
      switch (b) {
        case ESC:
          this.#escaped = true;
          break;
        case EOF:
          this.#out.push(Uint8Array.from(this.#buf));
          this.#idle = true;
          break;
        case SOF:
          throw new Error("framing error: SOF inside a frame (lost sync?)");
        default:
          this.#buf.push(b);
      }
    }
    return this.#out;
  }
}
