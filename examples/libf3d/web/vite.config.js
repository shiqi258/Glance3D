import { defineConfig } from "vite";
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const BROWSER_LOG_ENDPOINT = "/__browser-console-log";
const browserLogConfigPath = path.join(__dirname, "browser-log.config.json");
const browserLogDefaults = {
  enabled: false,
  logFile: "../../../logs/browser-console.jsonl",
  levels: ["debug", "log", "info", "warn", "error"],
  maxPayloadBytes: 262144,
};

const loadBrowserLogConfig = (enableForCommand) => {
  let fileConfig = {};

  try {
    fileConfig = JSON.parse(fs.readFileSync(browserLogConfigPath, "utf8"));
  } catch (error) {
    if (error.code !== "ENOENT") {
      throw new Error(
        `Failed to read browser log config at ${browserLogConfigPath}: ${error.message}`,
      );
    }
  }

  const config = { ...browserLogDefaults, ...fileConfig };
  const levels = Array.isArray(config.levels)
    ? config.levels.filter((level) => browserLogDefaults.levels.includes(level))
    : browserLogDefaults.levels;

  return {
    enabled: enableForCommand && config.enabled === true,
    logFile: path.resolve(__dirname, config.logFile),
    levels,
    maxPayloadBytes:
      Number.isInteger(config.maxPayloadBytes) && config.maxPayloadBytes > 0
        ? config.maxPayloadBytes
        : browserLogDefaults.maxPayloadBytes,
  };
};

const readRequestBody = (req, limit) => {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let size = 0;
    let rejected = false;

    req.on("data", (chunk) => {
      if (rejected) {
        return;
      }

      size += chunk.length;
      if (size > limit) {
        rejected = true;
        const error = new Error("Browser console log payload too large");
        error.statusCode = 413;
        reject(error);
        req.destroy();
        return;
      }

      chunks.push(chunk);
    });

    req.on("end", () => {
      if (!rejected) {
        resolve(Buffer.concat(chunks).toString("utf8"));
      }
    });

    req.on("error", (error) => {
      if (!rejected) {
        reject(error);
      }
    });
  });
};

const normalizeLogEntry = (entry, levels) => {
  if (!entry || typeof entry !== "object") {
    return null;
  }

  const level = typeof entry.level === "string" ? entry.level : "log";
  if (!levels.has(level)) {
    return null;
  }

  return Object.fromEntries(
    Object.entries({
      ts: typeof entry.ts === "string" ? entry.ts : new Date().toISOString(),
      level,
      kind: typeof entry.kind === "string" ? entry.kind : "console",
      args: Array.isArray(entry.args) ? entry.args : [],
      url: typeof entry.url === "string" ? entry.url : undefined,
      userAgent:
        typeof entry.userAgent === "string" ? entry.userAgent : undefined,
      sessionId:
        typeof entry.sessionId === "string" ? entry.sessionId : undefined,
      stack: typeof entry.stack === "string" ? entry.stack : undefined,
      filename:
        typeof entry.filename === "string" ? entry.filename : undefined,
      lineno: Number.isFinite(entry.lineno) ? entry.lineno : undefined,
      colno: Number.isFinite(entry.colno) ? entry.colno : undefined,
      f3dLevel:
        Number.isFinite(entry.f3dLevel) || typeof entry.f3dLevel === "string"
          ? entry.f3dLevel
          : undefined,
      f3dLevelName:
        typeof entry.f3dLevelName === "string" ? entry.f3dLevelName : undefined,
    }).filter(([, value]) => value !== undefined),
  );
};

const handleBrowserConsoleLog = async (req, res, config) => {
  const body = await readRequestBody(req, config.maxPayloadBytes);
  let payload;

  try {
    payload = JSON.parse(body);
  } catch {
    res.statusCode = 400;
    res.end("Invalid JSON");
    return;
  }

  const levelSet = new Set(config.levels);
  const entries = Array.isArray(payload.entries) ? payload.entries : [payload];
  const lines = entries
    .map((entry) => normalizeLogEntry(entry, levelSet))
    .filter(Boolean)
    .map((entry) => JSON.stringify(entry));

  if (lines.length) {
    await fs.promises.mkdir(path.dirname(config.logFile), { recursive: true });
    await fs.promises.appendFile(config.logFile, `${lines.join("\n")}\n`);
  }

  res.statusCode = 204;
  res.end();
};

const browserConsoleLogMiddleware = (config) => {
  return {
    name: "browser-console-log",
    configureServer(server) {
      if (!config.enabled) {
        return;
      }

      server.middlewares.use((req, res, next) => {
        if (req.method !== "POST" || !req.url?.startsWith(BROWSER_LOG_ENDPOINT)) {
          next();
          return;
        }

        handleBrowserConsoleLog(req, res, config).catch((error) => {
          res.statusCode = error.statusCode || 500;
          res.end(error.message || "Failed to write browser console log");
        });
      });
    },
  };
};

// Custom middleware to serve wasm files with the correct MIME type
// See https://stackoverflow.com/questions/78095780/web-assembly-wasm-errors-in-a-vite-vue-app-using-realm-web-sdk
const wasmMiddleware = () => {
  return {
    name: "wasm-middleware",
    configureServer(server) {
      server.middlewares.use((req, res, next) => {
        if (req.url && req.url.endsWith(".wasm")) {
          const wasmPath = path.join(
            __dirname,
            "node_modules/f3d/dist",
            path.basename(req.url),
          );
          const wasmFile = fs.readFileSync(wasmPath);
          res.setHeader("Content-Type", "application/wasm");
          res.end(wasmFile);
          return;
        }
        next();
      });
    },
  };
};

export default defineConfig(({ command }) => {
  const browserLogConfig = loadBrowserLogConfig(command === "serve");

  return {
    plugins: [wasmMiddleware(), browserConsoleLogMiddleware(browserLogConfig)],
    define: {
      __BROWSER_CONSOLE_LOG_CONFIG__: JSON.stringify({
        enabled: browserLogConfig.enabled,
        endpoint: BROWSER_LOG_ENDPOINT,
        levels: browserLogConfig.levels,
        maxPayloadBytes: browserLogConfig.maxPayloadBytes,
      }),
    },
    base: "./",
  };
});
