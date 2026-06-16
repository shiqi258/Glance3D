// Mirror the *entire* browser DevTools console to a desktop-aligned log file
// via the Chrome DevTools Protocol (CDP). Unlike the in-page console hook, this
// captures browser-native messages too: WebGL / GPU diagnostics (source
// "rendering"), network, security, CSP and deprecation warnings — anything that
// shows up in DevTools, including things page JavaScript can never observe.
//
// Run a Chromium browser with remote debugging first, e.g.:
//   msedge --remote-debugging-port=9222 --user-data-dir="%TEMP%\glance3d-cdp" http://localhost:5173/
// then start this collector. `npm run dev:logged` wires all of that up for you.

import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";

import {
  normalizeLevel,
  resolveLogDir,
  startWebLogSession,
} from "./glance3d-weblog.mjs";

const __filename = fileURLToPath(import.meta.url);
const scriptsDir = path.dirname(__filename);
const webDir = path.resolve(scriptsDir, "..");
const configPath = path.join(webDir, "browser-log.config.json");
const defaultConfig = {
  devtools: {
    host: "127.0.0.1",
    port: 9222,
    urlPattern: "127.0.0.1",
    captureRuntimeConsole: true,
  },
};

const args = new Map(
  process.argv
    .slice(2)
    .filter((arg) => arg.startsWith("--"))
    .map((arg) => {
      const [key, ...value] = arg.slice(2).split("=");
      return [key, value.length ? value.join("=") : "true"];
    }),
);

if (args.has("help")) {
  console.log(`Usage:
  npm run collect:browser-logs -- [--host=127.0.0.1] [--port=9222] [--url=localhost] [--log-file=PATH]

Captures the full DevTools console (console API, uncaught exceptions, and
browser-native messages such as WebGL/GPU warnings) to a desktop-aligned log
file under %LOCALAPPDATA%\\Glance3D\\logs\\g3d_*_web.log (override with G3D_LOG_DIR,
disable with G3D_LOG_FILE=0, keep count via G3D_LOG_KEEP).

Before running this collector, start Edge or Chrome with remote debugging:
  msedge --remote-debugging-address=127.0.0.1 --remote-debugging-port=9222 --user-data-dir="%TEMP%\\glance3d-cdp"
`);
  process.exit(0);
}

const loadConfig = () => {
  try {
    const fileConfig = JSON.parse(fs.readFileSync(configPath, "utf8"));
    return {
      ...defaultConfig,
      ...fileConfig,
      devtools: {
        ...defaultConfig.devtools,
        ...(fileConfig.devtools || {}),
      },
    };
  } catch (error) {
    if (error.code === "ENOENT") {
      return defaultConfig;
    }
    throw error;
  }
};

const config = loadConfig();
const devtoolsConfig = config.devtools || defaultConfig.devtools;
const host = args.get("host") || devtoolsConfig.host;
const port = Number(args.get("port") || devtoolsConfig.port);
const urlPattern = args.get("url") || devtoolsConfig.urlPattern;
const captureRuntimeConsole =
  args.get("runtime-console") === "false"
    ? false
    : devtoolsConfig.captureRuntimeConsole !== false;
const explicitLogFile = args.has("log-file")
  ? path.resolve(webDir, args.get("log-file"))
  : undefined;
const targetListUrl = `http://${host}:${port}/json/list`;
let shuttingDown = false;

if (!explicitLogFile && resolveLogDir() === null) {
  console.log("File logging disabled via G3D_LOG_FILE; nothing to collect to.");
  process.exit(0);
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// Render a CDP RemoteObjectPreview (the shallow property snapshot Chrome
// attaches to console-API object args) instead of dropping it. Without this,
// objects logged as a separate console arg collapse to a bare "Object".
const previewToString = (preview) => {
  if (!preview || !Array.isArray(preview.properties)) {
    return undefined;
  }
  const parts = preview.properties.map((prop) =>
    prop.value !== undefined ? `${prop.name}: ${prop.value}` : `${prop.name}: ${prop.type}`,
  );
  const body = parts.join(", ");
  if (Array.isArray(preview.entries) || preview.subtype === "array") {
    return preview.overflow ? `[${body}, …]` : `[${body}]`;
  }
  return preview.overflow ? `{${body}, …}` : `{${body}}`;
};

const remoteObjectValue = (arg) => {
  if (!arg || typeof arg !== "object") {
    return arg;
  }
  if ("unserializableValue" in arg) {
    return arg.unserializableValue;
  }
  if ("value" in arg) {
    return arg.value;
  }
  const previewed = previewToString(arg.preview);
  if (previewed !== undefined) {
    return previewed;
  }
  return arg.description || arg.className || arg.type;
};

const stringifyArg = (value) => {
  if (typeof value === "string") {
    return value;
  }
  if (value === null || value === undefined) {
    return String(value);
  }
  if (typeof value === "number" || typeof value === "boolean") {
    return String(value);
  }
  try {
    return JSON.stringify(value);
  } catch {
    return String(value);
  }
};

const loadTargets = async () => {
  const response = await fetch(targetListUrl);
  if (!response.ok) {
    throw new Error(`CDP target list returned ${response.status}`);
  }
  return response.json();
};

const findTarget = async () => {
  const targets = await loadTargets();
  return targets.find(
    (target) =>
      target.type === "page" &&
      target.webSocketDebuggerUrl &&
      target.url?.includes(urlPattern),
  );
};

class CDPConnection {
  constructor(webSocketUrl) {
    this.nextId = 1;
    this.pending = new Map();
    this.listeners = new Map();
    this.webSocket = new WebSocket(webSocketUrl);
  }

  async open() {
    await new Promise((resolve, reject) => {
      this.webSocket.addEventListener("open", resolve, { once: true });
      this.webSocket.addEventListener("error", reject, { once: true });
    });

    this.webSocket.addEventListener("message", (event) => {
      const message = JSON.parse(event.data);
      if (message.id && this.pending.has(message.id)) {
        const { resolve, reject } = this.pending.get(message.id);
        this.pending.delete(message.id);
        if (message.error) {
          reject(new Error(message.error.message));
        } else {
          resolve(message.result);
        }
        return;
      }

      const listeners = this.listeners.get(message.method) || [];
      for (const listener of listeners) {
        listener(message.params || {});
      }
    });
  }

  call(method, params = {}) {
    const id = this.nextId++;
    this.webSocket.send(JSON.stringify({ id, method, params }));
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
    });
  }

  on(method, listener) {
    const listeners = this.listeners.get(method) || [];
    listeners.push(listener);
    this.listeners.set(method, listeners);
  }

  waitForClose() {
    return new Promise((resolve) => {
      this.webSocket.addEventListener("close", resolve, { once: true });
    });
  }

  close() {
    this.webSocket.close();
  }
}

const connectAndCollect = async (target) => {
  const session = startWebLogSession({
    source: "cdp-devtools",
    explicitFile: explicitLogFile,
    meta: [
      `Source: CDP DevTools mirror (${host}:${port})`,
      `Page: ${target.url}`,
    ],
  });

  if (!session) {
    console.log("Could not open a log file; aborting.");
    process.exit(1);
  }

  const connection = new CDPConnection(target.webSocketDebuggerUrl);
  await connection.open();

  // Browser-native messages: WebGL/GPU ("rendering"), network, security, etc.
  connection.on("Log.entryAdded", ({ entry }) => {
    let message = entry.text || "";
    if (entry.url) {
      message += ` (${entry.url}${
        entry.lineNumber != null ? `:${entry.lineNumber}` : ""
      })`;
    }
    session.write(normalizeLevel(entry.level), entry.source || "other", message);
  });

  // Uncaught exceptions and unhandled rejections with their stacks.
  connection.on("Runtime.exceptionThrown", ({ exceptionDetails }) => {
    const details = exceptionDetails || {};
    const exception = details.exception || {};
    const message =
      exception.description ||
      details.text ||
      remoteObjectValue(exception) ||
      "Uncaught exception";
    session.write("ERROR", "exception", message);
  });

  // console.* calls (covers WASM/f3d stdout/stderr that Emscripten routes to
  // console, plus the in-page viewer logs).
  if (captureRuntimeConsole) {
    connection.on("Runtime.consoleAPICalled", (event) => {
      const message = (event.args || [])
        .map(remoteObjectValue)
        .map(stringifyArg)
        .join(" ");
      session.write(normalizeLevel(event.type), "console", message);
    });
  }

  await connection.call("Log.enable");
  await connection.call("Runtime.enable");

  console.log(`Collecting DevTools logs from ${target.url}`);
  console.log(`Writing to ${session.file}`);

  if (args.has("once")) {
    await sleep(Number(args.get("timeout") || 3000));
    connection.close();
  }

  await connection.waitForClose();
};

process.on("SIGINT", () => {
  shuttingDown = true;
  process.exit(0);
});

while (!shuttingDown) {
  try {
    const target = await findTarget();
    if (!target) {
      console.log(
        `Waiting for CDP page matching "${urlPattern}" at ${targetListUrl}`,
      );
      await sleep(1000);
      continue;
    }

    await connectAndCollect(target);
  } catch (error) {
    if (!shuttingDown) {
      console.log(`CDP collector retrying: ${error.message}`);
      await sleep(1000);
    }
  }
}
