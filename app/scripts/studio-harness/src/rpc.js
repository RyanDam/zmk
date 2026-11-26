/*
 * RPC core over a raw serial stream: framing + zmk.studio.Request/
 * Response with request-id matching, a global single-flight mutex
 * (mirroring the reference client's call_rpc), meta-error mapping and
 * a notification tap.
 */

import { encodeFrame, FrameDecoder } from "./framing.js";
import { loadProtos } from "./protos.js";

export class MetaError extends Error {}
export class NoResponseError extends Error {}

const RPC_TIMEOUT_MS = 10000;

export class StudioRpc {
  constructor(port, root) {
    this.port = port;
    this.Request = root.lookupType("zmk.studio.Request");
    this.Response = root.lookupType("zmk.studio.Response");
    this.decoder = new FrameDecoder();
    this.currentRequestId = 0;
    this.pending = null;
    this.queue = Promise.resolve(); // strict one-in-flight, like call_rpc
    this.notificationHandlers = [];
    this.rawLog = false;

    port.on("data", (chunk) => this.#onData(chunk));
  }

  static async connect(port) {
    const root = await loadProtos();
    return new StudioRpc(port, root);
  }

  onNotification(cb) {
    this.notificationHandlers.push(cb);
  }

  /**
   * call(subsystem, requestType, payload, opts) -> resolves with the
   * decoded RequestResponse object (e.g. {requestId, screen:
   * {responseType: {screens: [...]}}}); rejects on meta errors /
   * timeouts. opts.timeoutMs overrides RPC_TIMEOUT_MS (a15 upload_end
   * blocks the device until the flash commit lands).
   */
  call(subsystem, requestType, payload = {}, opts = {}) {
    const run = () => this.#call(subsystem, requestType, payload, opts);
    const p = this.queue.then(run, run);
    this.queue = p.then(
      () => {},
      () => {}
    );
    return p;
  }

  #call(subsystem, requestType, payload, opts = {}) {
    return new Promise((resolve, reject) => {
      const id = this.currentRequestId++;
      let obj;
      try {
        /* protobufjs fromObject takes oneof MEMBERS directly (the
         * virtual `requestType` wrapper is toObject-only) — and it
         * silently drops unknown keys, so the nested form here once
         * encoded every request as an empty subsystem. */
        obj = this.Request.fromObject({
          requestId: id,
          [subsystem]: { [requestType]: payload },
        });
        const bytes = this.Request.encode(obj).finish();
        if (this.rawLog) {
          console.error(`TX ${Buffer.from(bytes).toString("hex")}`);
        }
        this.port.write(encodeFrame(bytes));
      } catch (e) {
        reject(e);
        return;
      }

      const timer = setTimeout(() => {
        this.pending = null;
        reject(new Error(`timeout waiting for response to #${id} (${subsystem}.${requestType})`));
      }, opts.timeoutMs ?? RPC_TIMEOUT_MS);

      this.pending = { id, resolve, reject, timer };
    });
  }

  #onData(chunk) {
    let frames;
    try {
      frames = this.decoder.push(chunk);
    } catch (e) {
      console.error(`framing: ${e.message} (resynced)`);
      return;
    }

    for (const frame of frames) {
      let resp;
      try {
        resp = this.Response.decode(frame);
      } catch (e) {
        console.error(`decode failed: ${e.message}`);
        continue;
      }
      const obj = this.Response.toObject(resp, { defaults: false });

      if (obj.notification) {
        for (const cb of this.notificationHandlers) {
          cb(obj.notification);
        }
        continue;
      }

      const rr = obj.requestResponse;
      if (!rr || !this.pending) {
        continue;
      }
      if (rr.requestId !== this.pending.id) {
        continue; // stale
      }

      const { resolve, reject, timer } = this.pending;
      this.pending = null;
      clearTimeout(timer);

      const meta = rr.meta;
      if (meta && "noResponse" in meta) {
        resolve(null);
      } else if (meta && "simpleError" in meta) {
        // GENERIC is enum 0 — truthiness would miss it, so test presence
        reject(new MetaError(`device error: ${meta.simpleError}`));
      } else {
        resolve(rr);
      }
    }
  }
}

/** The subsystem payload out of a decoded RequestResponse. protobufjs
 *  toObject yields oneof MEMBERS flat, so rr.assets is already the
 *  inner zmk.assets.Response object ({listAssets: {…}}). */
export function responsePayload(rr, subsystem) {
  return rr?.[subsystem] ?? {};
}

export function notificationPayload(n, subsystem) {
  return n?.[subsystem] ?? {};
}
