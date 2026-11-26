/*
 * Protobuf definitions, loaded live from the west module checkout the
 * firmware codegens against (modules/msgs/zmk-studio-messages) — the
 * harness can never drift from the device's wire format, and there is
 * no build step.
 */

import path from "node:path";
import { fileURLToPath } from "node:url";

import protobuf from "protobufjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));

export const PROTO_DIR = path.resolve(
  HERE,
  "../../../../modules/msgs/zmk-studio-messages/proto/zmk"
);

let rootPromise;

export function loadProtos() {
  rootPromise ??= protobuf.load(path.join(PROTO_DIR, "studio.proto"));
  return rootPromise;
}
