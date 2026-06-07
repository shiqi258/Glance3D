import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";

const __filename = fileURLToPath(import.meta.url);
const scriptsDir = path.dirname(__filename);
const webDir = path.resolve(scriptsDir, "..");
const configPath = path.join(webDir, "browser-log.config.json");
const defaultConfig = {
  logFile: "../../../logs/browser-console.jsonl",
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
  npm run collect:browser-logs -- [--host=127.0.0.1] [--port=9222] [--url=127.0.0.1:5175]

Before running this collector, start Edge or Chrome with remote debugging, for example:
  msedge --remote-debugging-address=127.0.0.1 --remote-debugging-port=9222 --user-data-dir="%TEMP%\\glance3d-edge-cdp"
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
const logFile = path.resolve(
  webDir,
  args.get("log-file") || config.logFile || defaultConfig.logFile,
);
const targetListUrl = `http://${host}:${port}/json/list`;
let shuttingDown = false;

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

const appendEntry = async (entry) => {
  await fs.promises.mkdir(path.dirname(logFile), { recursive: true });
  await fs.promises.appendFile(logFile, `${JSON.stringify(entry)}\n`, "utf8");
};

const levelFromDevTools = (levelOrType) => {
  switch (levelOrType) {
    case "debug":
      return "debug";
    case "warning":
    case "warn":
      return "warn";
    case "error":
      return "error";
    case "info":
      return "info";
    default:
      return "log";
  }
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
  return arg.description || arg.className || arg.type;
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
  const connection = new CDPConnection(target.webSocketDebuggerUrl);
  await connection.open();

  connection.on("Log.entryAdded", ({ entry }) => {
    appendEntry({
      ts: new Date().toISOString(),
      level: levelFromDevTools(entry.level),
      kind: "devtools",
      source: "cdp.Log.entryAdded",
      cdpSource: entry.source,
      args: [entry.text],
      url: entry.url || target.url,
      lineNumber: entry.lineNumber,
      stackTrace: entry.stackTrace,
    }).catch(() => {});
  });

  if (captureRuntimeConsole) {
    connection.on("Runtime.consoleAPICalled", (event) => {
      appendEntry({
        ts: new Date(event.timestamp).toISOString(),
        level: levelFromDevTools(event.type),
        kind: "devtools",
        source: "cdp.Runtime.consoleAPICalled",
        args: (event.args || []).map(remoteObjectValue),
        url: event.stackTrace?.callFrames?.[0]?.url || target.url,
        stackTrace: event.stackTrace,
      }).catch(() => {});
    });
  }

  await connection.call("Log.enable");
  await connection.call("Runtime.enable");

  console.log(`Collecting DevTools logs from ${target.url}`);
  console.log(`Writing to ${logFile}`);

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
