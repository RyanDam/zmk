#!/usr/bin/env node
/*
 * Coban Tab 1a Studio RPC bench harness (backlog a14).
 *
 * Raw-serial ZMK Studio client: framing + protobuf straight from the
 * west module, so what runs here is exactly what the firmware speaks.
 * Bench wiring + baud: zmk/esp-port/tab1a-bench-runbook.md
 * (UART0 TX GPIO43 / RX GPIO44, P1 header, 115200 8N1).
 *
 * Usage: node src/cli.js --port /dev/cu.usbserial-XXXX <command> [...]
 *        node src/cli.js ... selftest
 */

import { parseArgs } from "node:util";
import { readFileSync } from "node:fs";
import { basename } from "node:path";
import { SerialPort } from "serialport";

import { StudioRpc, responsePayload, notificationPayload, MetaError } from "./rpc.js";

const USAGE = `studio-harness — Coban Tab 1a Studio RPC bench client (a14 + a15)

Global options:
  --port <path>   serial device (required; e.g. /dev/cu.usbserial-FT6...)
  --baud <n>      default 115200
  --raw           hex-dump every request frame (debug)

Commands:
  info | lock | reboot
  screens
  screen <name>
  time --sync | time
  theme get | theme set <bg> <textP> <textS> <accent> <inactive>   (hex, 0xRRGGBB)
  stats config [layout m1 m2 m3 m4]
  stats push [--demo] [--hz N] [--for SECS]
  pomo get | status | set <sets> <work_s> <rest_s> <noti> | start | pause | resume | cancel
  assets list | activate <name> | delete <name> | status | gc
  assets upload <file> [--name <n>] [--kind gif|json] [--fps N]
                  [--corrupt] [--resume]
                  (a15: 1 KB chunked upload, CRC-32 verified device-side;
                   --corrupt flips a byte to prove commit-last safety;
                   --resume continues an interrupted session via upload_status)
  keymap get | rebind <layer> <pos> <behaviorId> [param1 param2] | save | discard
  macro list | rename <idx> <name> | save
  selftest
`;

function parse() {
  const { values, positionals } = parseArgs({
    options: {
      port: { type: "string" },
      baud: { type: "string", default: "115200" },
      raw: { type: "boolean", default: false },
      demo: { type: "boolean", default: false },
      sync: { type: "boolean", default: false },
      hz: { type: "string", default: "2" },
      for: { type: "string", default: "10" },
      name: { type: "string" },
      kind: { type: "string" },
      fps: { type: "string", default: "15" },
      corrupt: { type: "boolean", default: false },
      resume: { type: "boolean", default: false },
    },
    allowPositionals: true,
  });
  return { values, args: positionals };
}

const ok = (msg) => console.log(`  ✔ ${msg}`);
const pjson = (label, obj) => console.log(`  ${label}: ${JSON.stringify(obj)}`);

function colorToProto(hex) {
  const v = parseInt(hex, 16);
  return { r: (v >> 16) & 0xff, g: (v >> 8) & 0xff, b: v & 0xff };
}
const colorFromProto = (c) =>
  "0x" + [c.r, c.g, c.b].map((x) => x.toString(16).padStart(2, "0")).join("");

/* zlib-compatible CRC-32 (the device verifies blobs with the same one) */
const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();
function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

const UPLOAD_CHUNK = 1024; // firmware assets.options max_size for data chunks

/**
 * a15 chunked upload: begin -> data* -> end, lockstep (the RPC layer is
 * one-in-flight), progress from the device's upload_progress
 * notifications. Returns { committed, error, freeBytes }.
 */
async function uploadAsset(rpcClient, file, opts, ctx) {
  const data = readFileSync(file);
  const stem = basename(file).replace(/@(\d+)\.[^.]+$/, "").replace(/\.[^.]+$/, "");
  const name = (opts.name ?? stem).slice(0, 15);
  if (!name) throw new Error("empty asset name");
  const kind = (opts.kind ?? (file.endsWith(".json") ? "json" : "gif")) === "json" ? 1 : 0;
  const fps = Number(opts.fps);
  const sum = crc32(data);

  let resumeFrom = 0;
  if (opts.resume) {
    const st = responsePayload(
      await rpcClient.call("assets", "uploadStatus", true),
      "assets"
    ).uploadStatus;
    if (!st?.active) throw new Error("--resume: no active session on the device");
    if (st.name !== name) throw new Error(`--resume: device session is "${st.name}", file is "${name}"`);
    if (st.accepted % UPLOAD_CHUNK !== 0) {
      throw new Error(`--resume: accepted ${st.accepted} not chunk-aligned — abort and restart`);
    }
    resumeFrom = st.accepted;
    console.log(`  resuming "${name}" at ${resumeFrom} B`);
  } else {
    const b = responsePayload(
      await rpcClient.call("assets", "uploadBegin", {
        name,
        size: data.length,
        crc32: sum,
        kind,
        fps,
      }),
      "assets"
    ).uploadBegin;
    if (!b?.accepted) throw new Error(`begin refused: ${b?.error ?? "no response"}`);
    console.log(`  begin "${name}" ${data.length} B crc=${sum.toString(16)} (free ${(b.freeBytes ?? 0) / 1024 | 0} KiB)`);
  }

  const t0 = Date.now();
  const seenProgress = new Set();
  const tap = (n) => {
    const p = notificationPayload(n, "assets")?.uploadProgress;
    if (p && p.name === name && !p.gcRunning && p.totalBytes > 0) {
      const pct = Math.floor((p.acceptedBytes / p.totalBytes) * 20) * 5;
      if (!seenProgress.has(pct)) {
        seenProgress.add(pct);
        process.stdout.write(`\r  ${String(pct).padStart(3)}%`);
      }
    }
  };
  rpcClient.onNotification(tap);

  try {
    let seq = Math.floor(resumeFrom / UPLOAD_CHUNK);
    for (let off = resumeFrom; off < data.length; off += UPLOAD_CHUNK, seq++) {
      const chunk = Buffer.from(data.subarray(off, Math.min(off + UPLOAD_CHUNK, data.length)));
      if (opts.corrupt && off <= resumeFrom && chunk.length > 8) {
        chunk[4] ^= 0xff; // first staged chunk corrupted -> device CRC must catch it
      }
      const r = responsePayload(
        await rpcClient.call("assets", "uploadData", { seq, data: chunk }),
        "assets"
      ).uploadData;
      if (r?.error) {
        await rpcClient.call("assets", "uploadAbort", true).catch(() => {});
        throw new Error(`data #${seq} refused: ${r.error}`);
      }
    }
    const e = responsePayload(
      await rpcClient.call("assets", "uploadEnd", true, { timeoutMs: 30000 }),
      "assets"
    ).uploadEnd;
    const secs = (Date.now() - t0) / 1000;
    process.stdout.write("\r");
    console.log(
      `  end: committed=${!!e?.committed} ${e?.error ? `error="${e.error}" ` : ""}` +
      `(${((data.length - resumeFrom) / 1024 / secs).toFixed(1)} KiB/s over the wire, ` +
      `${data.length} B total, free ${((e?.freeBytes ?? 0) / 1024) | 0} KiB)`
    );
    return { committed: !!e?.committed, error: e?.error ?? "", name };
  } finally {
    const i = rpcClient.notificationHandlers.indexOf(tap);
    if (i >= 0) rpcClient.notificationHandlers.splice(i, 1);
  }
}

async function rpc(rr) {
  if (rr === null) return {};
  for (const subsys of ["screen", "theme", "stats", "pomo", "assets", "keymap", "macros", "core"]) {
    if (rr[subsys]) return responsePayload(rr, subsys);
  }
  return rr;
}

/* ---- commands ------------------------------------------------------- */

const cmds = {
  async info(rpcClient) {
    const p = await rpc(await rpcClient.call("core", "getDeviceInfo", true));
    const name = p.getDeviceInfo?.name ?? "(no name)";
    ok(`device: ${name}`);
    const lock = await rpc(await rpcClient.call("core", "getLockState", true));
    pjson("lock", lock.getLockState);
  },

  async lock(rpcClient) {
    await rpcClient.call("core", "lock", true);
    ok("unlocked (10 min idle timeout)");
  },

  async reboot(rpcClient) {
    await rpcClient.call("core", "reboot", true);
    ok("reboot requested — device drops in ~500 ms");
  },

  async screens(rpcClient) {
    const p = await rpc(await rpcClient.call("screen", "getScreens", {}));
    const g = p.getScreens ?? {};
    for (const s of g.screens ?? []) {
      console.log(`  ${s.name === g.current ? ">" : " "} ${s.name} (order ${s.order})`);
    }
    pjson("current", g.current);
  },

  async screen(rpcClient, [name]) {
    if (!name) throw new Error("screen <name>");
    await rpc(await rpcClient.call("screen", "setScreen", { name }));
    ok(`screen -> ${name} (watch the panel)`);
  },

  async time(rpcClient, _a, opts) {
    if (opts.sync) {
      const epoch = Math.floor(Date.now() / 1000);
      await rpc(await rpcClient.call("screen", "setTime", { epochS: epoch }));
      ok(`set_time ${epoch} (host clock)`);
    }
    const p = await rpc(await rpcClient.call("screen", "getTime", true));
    const g = p.getTime ?? {};
    pjson("device clock", { epochS: g.epochS, synced: g.synced, utc: new Date((g.epochS ?? 0) * 1000).toISOString() });
    if (opts.sync && !g.synced) throw new Error("synced flag still false after set_time");
  },

  async theme(rpcClient, [what, ...rest]) {
    if (what === "set") {
      if (rest.length !== 5) throw new Error("theme set <bg> <textP> <textS> <accent> <inactive>");
      const [background, textPrimary, textSecondary, accent, inactive] = rest.map(colorToProto);
      await rpcClient.call("theme", "setTheme", { theme: { background, textPrimary, textSecondary, accent, inactive } });
      ok("theme applied live + persisted");
      return;
    }
    const p = await rpc(await rpcClient.call("theme", "getTheme", true));
    const t = p.getTheme ?? {};
    for (const [k, v] of Object.entries(t)) {
      console.log(`  ${k.padEnd(15)} ${colorFromProto(v)}`);
    }
  },

  async stats(rpcClient, [what, ...rest], opts) {
    if (what === "config" && rest.length > 0) {
      const [layout, ...metrics] = rest.map(Number);
      await rpcClient.call("stats", "setStatsConfig", { config: { layout, metricIds: metrics } });
      ok(`stats config -> layout ${layout}, metrics ${metrics.join(",")}`);
      return;
    }
    if (what === "push") {
      const hz = Number(opts.hz);
      const secs = Number(opts.for);
      const end = Date.now() + secs * 1000;
      let n = 0;
      console.log(`  pushing demo frames at ${hz} Hz for ${secs}s (Ctrl-C stops)`);
      while (Date.now() < end) {
        const t = n / hz;
        const values = [
          { id: 0x01, value: Math.round(50 + 50 * Math.sin(t * 2)), max: 100, unitId: 0x01 },
          { id: 0x02, value: Math.round(40 + 20 * Math.sin(t * 1.3)), max: 95, unitId: 0x02 },
          { id: 0x03, value: Math.round(16 + 12 * Math.sin(t * 0.7)), max: 32, unitId: 0x05 },
          { id: 0x04, value: Math.round(50 + 50 * Math.cos(t * 1.7)), max: 100, unitId: 0x01 },
        ];
        await rpcClient.call("stats", "pushStatsData", { values });
        n++;
        await new Promise((r) => setTimeout(r, 1000 / hz));
      }
      ok(`${n} frames pushed (stats screen animates; bars grey out after the stale window)`);
      return;
    }
    const p = await rpc(await rpcClient.call("stats", "getStatsConfig", true));
    pjson("stats config", p.getStatsConfig);
  },

  async pomo(rpcClient, [what, ...rest]) {
    const map = { start: "start", pause: "pause", resume: "resume", cancel: "cancel" };
    if (map[what]) {
      const p = await rpc(await rpcClient.call("pomo", map[what], true));
      ok(`pomo ${what}: ${JSON.stringify(p[map[what]])}`);
      return;
    }
    if (what === "set") {
      if (rest.length !== 4) throw new Error("pomo set <sets> <work_s> <rest_s> <noti>");
      const [sets, workS, restS, notiMode] = rest.map(Number);
      await rpcClient.call("pomo", "setConfig", { config: { sets, workS, restS, notiMode } });
      ok("pomo config set (clamped) + persisted");
      return;
    }
    if (what === "status") {
      const p = await rpc(await rpcClient.call("pomo", "getStatus", true));
      pjson("status", p.getStatus);
      return;
    }
    const p = await rpc(await rpcClient.call("pomo", "getConfig", true));
    pjson("config", p.getConfig);
  },

  async assets(rpcClient, [what, ...rest], opts, ctx) {
    if (what === "activate") {
      await rpcClient.call("assets", "activateAsset", { name: rest[0] });
      ok(`activated ${rest[0]} (background swaps within ~200 ms)`);
      return;
    }
    if (what === "delete") {
      await rpc(await rpcClient.call("assets", "deleteAsset", { name: rest[0] }));
      ok(`deleted ${rest[0]} (new manifest generation; built-ins survive as fallback)`);
      return;
    }
    if (what === "status") {
      const p = await rpc(await rpcClient.call("assets", "uploadStatus", true));
      pjson("session", p.uploadStatus ?? {});
      return;
    }
    if (what === "gc") {
      const p = await rpc(await rpcClient.call("assets", "uploadGc", true, { timeoutMs: 30000 }));
      pjson("gc", p.uploadGc ?? {});
      console.log("  (compaction runs async on the device; watch upload_progress notifications)");
      return;
    }
    if (what === "upload") {
      if (!rest[0]) throw new Error("assets upload <file>");
      const r = await uploadAsset(rpcClient, rest[0], opts, ctx);
      if (!r.committed) throw new Error(`upload not committed: ${r.error}`);
      ok(`uploaded "${r.name}" (list to confirm; activate to play it live)`);
      return;
    }
    const p = await rpc(await rpcClient.call("assets", "listAssets", true));
    const l = p.listAssets ?? {};
    pjson("ready/active", {
      ready: l.ready,
      active: l.active,
      generation: l.generation,
      free: `${((l.freeBytes ?? 0) / 1024) | 0} KiB`,
    });
    for (const a of l.assets ?? []) {
      console.log(`  ${a.name.padEnd(16)} kind=${a.kind} len=${a.len} fps=${a.fps}`);
    }
  },

  async keymap(rpcClient, [what, ...rest]) {
    if (what === "rebind") {
      const [layer, pos, behaviorId, p1 = 0, p2 = 0] = rest.map(Number);
      const p = await rpc(await rpcClient.call("keymap", "setLayerBinding", {
        layerId: layer, keyPosition: pos,
        binding: { behaviorId, param1: p1, param2: p2 },
      }));
      ok(`set_layer_binding -> ${p.setLayerBinding}`);
      return;
    }
    if (what === "save" || what === "discard") {
      const p = await rpc(await rpcClient.call("keymap", what === "save" ? "saveChanges" : "discardChanges", true));
      ok(`keymap ${what}: ${JSON.stringify(p[what === "save" ? "saveChanges" : "discardChanges"])}`);
      return;
    }
    const p = await rpc(await rpcClient.call("keymap", "getKeymap", true));
    const km = p.getKeymap ?? {};
    for (const [i, layer] of (km.layers ?? []).entries()) {
      console.log(`  layer ${i}: ${layer.bindings?.length} bindings, ${layer.name ?? layer.displayName ?? ""}`);
    }
    pjson("physicalLayouts", km.physicalLayouts?.layouts?.map((l) => l.name) ?? km.physicalLayouts);
  },

  async macro(rpcClient, [what, ...rest]) {
    if (what === "rename") {
      const [idx, name] = [Number(rest[0]), rest.slice(1).join(" ")];
      await rpcClient.call("macros", "setMacroName", { index: idx, name });
      ok(`macro ${idx} -> "${name}" (staged; save persists)`);
      return;
    }
    if (what === "save") {
      await rpcClient.call("macros", "saveChanges", true);
      ok("macros saved");
      return;
    }
    const p = await rpc(await rpcClient.call("macros", "getMacroList", true));
    for (const m of p.getMacroList?.macros ?? []) {
      console.log(`  ${m.index}: "${m.name}" (max ${m.maxSteps} steps)`);
    }
  },

  /* ---- selftest: the a14 acceptance sequence ------------------------ */
  async selftest(rpcClient, _a, _o, ctx) {
    const results = [];
    const step = async (name, fn) => {
      try {
        await fn();
        results.push([name, true]);
        console.log(`  ✔ ${name}`);
      } catch (e) {
        results.push([name, false]);
        console.error(`  ✘ ${name}: ${e.message}`);
      }
    };

    await step("core.getDeviceInfo (transport alive)", async () => {
      await cmds.info(rpcClient);
    });

    await step("core.lock (SECURED handlers reachable)", async () => {
      await cmds.lock(rpcClient);
    });

    let screens;
    await step("screen list + navigate + screen_changed event", async () => {
      const p = await rpc(await rpcClient.call("screen", "getScreens", {}));
      screens = p.getScreens.screens.map((s) => s.name);
      if (screens.length < 5) throw new Error(`expected >=5 screens, got ${screens.length}`);

      const target = screens.find((s) => s !== p.getScreens.current);
      const ev = ctx.waitForNotification(
        (n) => notificationPayload(n, "screen").screenChanged,
        5000
      );
      await rpc(await rpcClient.call("screen", "setScreen", { name: target }));
      const seen = await ev;
      if (seen?.name !== target) throw new Error(`screen_changed said ${seen?.name}, wanted ${target}`);
      // restore
      await rpc(await rpcClient.call("screen", "setScreen", { name: p.getScreens.current }));
    });

    await step("set_time -> clock shows synced", async () => {
      const epoch = Math.floor(Date.now() / 1000);
      await rpc(await rpcClient.call("screen", "setTime", { epochS: epoch }));
      const p = await rpc(await rpcClient.call("screen", "getTime", true));
      if (!p.getTime.synced) throw new Error("synced false after set_time");
      if (Math.abs(p.getTime.epochS - epoch) > 3) throw new Error("clock did not move");
      console.log(`    (owner check: the clock page shows the synced state)`);
    });

    await step("theme get/set/restore + theme_changed event", async () => {
      const before = (await rpc(await rpcClient.call("theme", "getTheme", true))).getTheme;
      const accent = { ...before.accent, r: (before.accent.r + 32) & 0xff };
      const ev = ctx.waitForNotification((n) => notificationPayload(n, "theme"), 5000);
      await rpcClient.call("theme", "setTheme", { theme: { ...before, accent } });
      await ev;
      const after = (await rpc(await rpcClient.call("theme", "getTheme", true))).getTheme;
      if (after.accent.r !== accent.r) throw new Error("theme not applied");
      await rpcClient.call("theme", "setTheme", { theme: before }); // restore
      console.log(`    (owner check: accent recolors live, then restores)`);
    });

    await step("stats config round-trip + data frame -> screen", async () => {
      const before = (await rpc(await rpcClient.call("stats", "getStatsConfig", true))).getStatsConfig;
      const next = { ...before, layout: (before.layout + 1) % 4 };
      await rpcClient.call("stats", "setStatsConfig", { config: next });
      const now = (await rpc(await rpcClient.call("stats", "getStatsConfig", true))).getStatsConfig;
      if (now.layout !== next.layout) throw new Error("layout not persisted");
      await rpcClient.call("stats", "setStatsConfig", { config: before }); // restore

      await rpcClient.call("stats", "pushStatsData", {
        values: [
          { id: 0x01, value: 42, max: 100, unitId: 0x01 },
          { id: 0x02, value: 51, max: 95, unitId: 0x02 },
        ],
      });
      console.log(`    (owner check on the stats page: layout flips back/forth, CPU "42%" bar lands)`);
    });

    await step("pomo config + start/cancel + state event", async () => {
      const before = (await rpc(await rpcClient.call("pomo", "getConfig", true))).getConfig;
      const cfg = { sets: 2, workS: 120, restS: 60, notiMode: 0 };
      await rpcClient.call("pomo", "setConfig", { config: cfg });
      const now = (await rpc(await rpcClient.call("pomo", "getConfig", true))).getConfig;
      if (now.workS !== 120 || now.sets !== 2) throw new Error(`config ${JSON.stringify(now)}`);

      const seen = [];
      const ev = ctx.waitForNotification(
        (n) => {
          const p = notificationPayload(n, "pomo").stateChanged;
          if (p) seen.push(p);
          return p;
        },
        8000,
        () => seen.some((s) => s.state === 1) // reached WORK
      );
      await rpc(await rpcClient.call("pomo", "start", true));
      await ev;
      await rpc(await rpcClient.call("pomo", "cancel", true));
      await rpcClient.call("pomo", "setConfig", { config: before }); // restore
      console.log(`    (owner check: pomo page starts + cancels on the tab)`);
    });

    await step("assets list + activate (a14 seam still works)", async () => {
      const p = await rpc(await rpcClient.call("assets", "listAssets", true));
      const l = p.listAssets;
      if (l.assets.length === 0) throw new Error("asset store empty — pack + flash assets.bin first");
      await rpc(await rpcClient.call("assets", "activateAsset", { name: l.assets[0].name }));
    });

    await step("a15 upload 64 KiB -> list + generation bump -> activate live", async () => {
      const before = (await rpc(await rpcClient.call("assets", "listAssets", true))).listAssets;
      const data = Buffer.alloc(64 * 1024);
      for (let i = 0; i < data.length; i++) data[i] = (i * 37 + 11) & 0xff;
      const tmp = "/tmp/a15-selftest-probe.gif";
      (await import("node:fs")).writeFileSync(tmp, data);
      const r = await uploadAsset(rpcClient, tmp, { name: "a15probe", kind: "gif", fps: 15 }, ctx);
      if (!r.committed) throw new Error(`committed=false (${r.error})`);
      const after = (await rpc(await rpcClient.call("assets", "listAssets", true))).listAssets;
      if (after.generation <= (before.generation ?? 0)) {
        throw new Error(`generation did not advance (${before.generation} -> ${after.generation})`);
      }
      if (!after.assets.some((a) => a.name === "a15probe")) throw new Error("probe asset not listed");
      await rpc(await rpcClient.call("assets", "activateAsset", { name: "a15probe" }));
      console.log(`    (owner check: background swaps to the flat 64 KiB pattern, no artifacts)`);
    });

    await step("a15 corrupt stream: committed=false, live image survives", async () => {
      const data = Buffer.alloc(8 * 1024, 0xa5);
      const tmp = "/tmp/a15-selftest-corrupt.gif";
      (await import("node:fs")).writeFileSync(tmp, data);
      const r = await uploadAsset(rpcClient, tmp, { name: "a15bad", kind: "gif", fps: 15, corrupt: true }, ctx);
      if (r.committed) throw new Error("corrupt stream was committed!");
      if (!/crc/i.test(r.error)) throw new Error(`expected crc error, got "${r.error}"`);
      const l = (await rpc(await rpcClient.call("assets", "listAssets", true))).listAssets;
      if (l.assets.some((a) => a.name === "a15bad")) throw new Error("corrupt entry leaked into manifest");
      await rpc(await rpcClient.call("assets", "activateAsset", { name: "a15probe" }));
      console.log(`    (owner check: art unaffected by the failed upload)`);
    });

    await step("a15 delete makes it real + builtin fallback survives", async () => {
      await rpc(await rpcClient.call("assets", "deleteAsset", { name: "a15probe" }));
      const l = (await rpc(await rpcClient.call("assets", "listAssets", true))).listAssets;
      if (l.assets.some((a) => a.name === "a15probe")) throw new Error("deleted asset still listed");
      if (l.assets.length === 0) throw new Error("built-ins vanished after delete");
      await rpc(await rpcClient.call("assets", "activateAsset", { name: l.assets[0].name }));
      console.log(`    (owner check: deleted art gone, original background back)`);
    });

    await step("keymap RPC rebind one position (stock keymap RPC)", async () => {
      const km = (await rpc(await rpcClient.call("keymap", "getKeymap", true))).getKeymap;
      const b4 = km.layers[0].bindings[4];
      const b5 = km.layers[0].bindings[5];
      const set = async (pos, binding) => {
        const p = await rpc(await rpcClient.call("keymap", "setLayerBinding", {
          layerId: 0, keyPosition: pos,
          binding: { behaviorId: binding.behaviorId, param1: binding.param1, param2: binding.param2 },
        }));
        if (p.setLayerBinding !== "SET_LAYER_BINDING_RESP_OK") throw new Error(`resp ${p.setLayerBinding}`);
      };
      await set(4, b5);
      await rpc(await rpcClient.call("keymap", "saveChanges", true));
      const km2 = (await rpc(await rpcClient.call("keymap", "getKeymap", true))).getKeymap;
      if (km2.layers[0].bindings[4].behaviorId !== b5.behaviorId) throw new Error("rebind not live");
      await set(4, b4);
      await rpc(await rpcClient.call("keymap", "saveChanges", true));
      console.log(`    (owner check: tile 5's label moved onto tile 4, then back)`);
    });

    await step("macros RPC rename a dynamic macro (stock macros RPC)", async () => {
      const list = (await rpc(await rpcClient.call("macros", "getMacroList", true))).getMacroList.macros;
      const m0 = list[0];
      const before = m0.name ?? "";
      await rpcClient.call("macros", "setMacroName", { index: 0, name: "MAIL" });
      await rpcClient.call("macros", "saveChanges", true);
      const after = (await rpc(await rpcClient.call("macros", "getMacroList", true))).getMacroList.macros[0];
      if (after.name !== "MAIL") throw new Error(`name after save: "${after.name}"`);
      await rpcClient.call("macros", "setMacroName", { index: 0, name: before });
      await rpcClient.call("macros", "saveChanges", true);
      console.log(`    (owner check: keymap screen tile relabels live)`);
    });

    const failed = results.filter(([, pass]) => !pass);
    console.log(`\nselftest: ${results.length - failed.length}/${results.length} passed`);
    if (failed.length) throw new Error(`${failed.length} step(s) failed`);
  },
};

/* ---- main ----------------------------------------------------------- */

const { values, args } = parse();
const command = args[0];
if (!command || command === "help") {
  console.log(USAGE);
  process.exit(command ? 0 : 1);
}
if (!values.port) {
  console.error("error: --port is required\n" + USAGE);
  process.exit(1);
}
if (!cmds[command]) {
  console.error(`unknown command "${command}"\n` + USAGE);
  process.exit(1);
}

const port = new SerialPort({ path: values.port, baudRate: Number(values.baud) });
await new Promise((resolve, reject) => {
  port.once("open", resolve);
  port.once("err", reject);
});

const rpcClient = await StudioRpc.connect(port);
rpcClient.rawLog = values.raw;

const notifications = [];
rpcClient.onNotification((n) => notifications.push(n));

const ctx = {
  /** Resolve with the first mapped notification passing pred (or wait
   * one tick if pred omitted). */
  waitForNotification(map, timeoutMs, pred) {
    const t0 = notifications.length;
    return new Promise((resolve, reject) => {
      const check = () => {
        for (let i = t0; i < notifications.length; i++) {
          const mapped = map(notifications[i]);
          if (mapped && (!pred || pred(mapped))) {
            clearInterval(iv);
            clearTimeout(to);
            resolve(mapped);
            return true;
          }
        }
        return false;
      };
      const iv = setInterval(() => { if (check()) return; }, 50);
      const to = setTimeout(() => {
        clearInterval(iv);
        reject(new Error("no matching notification in time — did the firmware log show the event?"));
      }, timeoutMs);
      check();
    });
  },
};

try {
  await cmds[command](rpcClient, args.slice(1), values, ctx);
  console.log("done");
  process.exitCode = 0;
} catch (e) {
  console.error(`FAIL: ${e.message}`);
  process.exitCode = 1;
} finally {
  await new Promise((r) => port.close(r));
}
