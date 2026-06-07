const DEFAULT_CONFIG = {
  enabled: false,
  endpoint: "/__browser-console-log",
  levels: ["debug", "log", "info", "warn", "error"],
  maxPayloadBytes: 262144,
};

const CONFIG =
  typeof __BROWSER_CONSOLE_LOG_CONFIG__ === "undefined"
    ? DEFAULT_CONFIG
    : { ...DEFAULT_CONFIG, ...__BROWSER_CONSOLE_LOG_CONFIG__ };
const ENABLED_LEVELS = new Set(CONFIG.levels || DEFAULT_CONFIG.levels);

const INSTALL_KEY = "__f3dBrowserConsoleLoggerInstalled";
const ORIGINAL_CONSOLE_KEY = "__f3dBrowserConsoleLoggerOriginalConsole";
const SESSION_KEY = "__f3dBrowserConsoleLoggerSessionId";
const MAX_DEPTH = 4;
const MAX_KEYS = 50;
const MAX_STRING_LENGTH = 2000;
const FLUSH_DELAY_MS = 250;
const MAX_BATCH_SIZE = 20;

let queue = [];
let flushTimer = null;

const makeSessionId = () => {
  if (globalThis.crypto?.randomUUID) {
    return globalThis.crypto.randomUUID();
  }
  return `${Date.now()}-${Math.random().toString(16).slice(2)}`;
};

const truncateString = (value) => {
  if (value.length <= MAX_STRING_LENGTH) {
    return value;
  }
  return `${value.slice(0, MAX_STRING_LENGTH)}... [truncated]`;
};

const serializeValue = (value, depth = 0, seen = new WeakSet()) => {
  if (value == null) {
    return value;
  }

  if (typeof value === "string") {
    return truncateString(value);
  }

  if (typeof value === "number" || typeof value === "boolean") {
    return value;
  }

  if (typeof value === "bigint") {
    return `${value.toString()}n`;
  }

  if (typeof value === "symbol" || typeof value === "function") {
    return String(value);
  }

  if (value instanceof Error) {
    return {
      name: value.name,
      message: truncateString(value.message),
      stack: value.stack ? truncateString(value.stack) : undefined,
    };
  }

  if (typeof Element !== "undefined" && value instanceof Element) {
    return value.outerHTML
      ? truncateString(value.outerHTML)
      : `[${value.tagName.toLowerCase()} element]`;
  }

  if (depth >= MAX_DEPTH) {
    return "[MaxDepth]";
  }

  if (seen.has(value)) {
    return "[Circular]";
  }

  seen.add(value);

  if (Array.isArray(value)) {
    return value.map((item) => serializeValue(item, depth + 1, seen));
  }

  const result = {};
  for (const key of Object.keys(value).slice(0, MAX_KEYS)) {
    try {
      result[key] = serializeValue(value[key], depth + 1, seen);
    } catch (error) {
      result[key] = `[Unserializable: ${error?.message || "unknown"}]`;
    }
  }
  return result;
};

const getStack = () => {
  const stack = new Error().stack;
  if (!stack) {
    return undefined;
  }
  return truncateString(stack);
};

const sendEntries = (entries, preferBeacon = false) => {
  if (!entries.length || !CONFIG.enabled) {
    return;
  }

  const body = JSON.stringify({ entries });

  if (
    CONFIG.maxPayloadBytes &&
    body.length > CONFIG.maxPayloadBytes &&
    entries.length > 1
  ) {
    for (const entry of entries) {
      sendEntries([entry], preferBeacon);
    }
    return;
  }

  if (preferBeacon && navigator.sendBeacon) {
    const payload = new Blob([body], { type: "application/json" });
    if (navigator.sendBeacon(CONFIG.endpoint, payload)) {
      return;
    }
  }

  fetch(CONFIG.endpoint, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body,
    keepalive: true,
  }).catch(() => {});
};

const flush = (preferBeacon = false) => {
  if (flushTimer) {
    clearTimeout(flushTimer);
    flushTimer = null;
  }

  const entries = queue;
  queue = [];
  sendEntries(entries, preferBeacon);
};

const enqueue = (entry) => {
  queue.push(entry);

  if (queue.length >= MAX_BATCH_SIZE) {
    flush();
    return;
  }

  if (!flushTimer) {
    flushTimer = setTimeout(() => flush(), FLUSH_DELAY_MS);
  }
};

const buildEntry = (level, kind, args, extra = {}) => ({
  ts: new Date().toISOString(),
  level,
  kind,
  args: args.map((arg) => serializeValue(arg)),
  url: window.location.href,
  userAgent: navigator.userAgent,
  sessionId: window[SESSION_KEY],
  stack: level === "warn" || level === "error" ? getStack() : undefined,
  ...extra,
});

export const recordBrowserLog = (level, kind, args, extra = {}) => {
  if (!CONFIG.enabled || typeof window === "undefined") {
    return;
  }

  if (!ENABLED_LEVELS.has(level)) {
    return;
  }

  window[SESSION_KEY] = window[SESSION_KEY] || makeSessionId();
  enqueue(buildEntry(level, kind, args, extra));
};

export const installBrowserConsoleLogger = () => {
  if (!CONFIG.enabled || typeof window === "undefined") {
    return;
  }

  if (window[INSTALL_KEY]) {
    return;
  }

  const originalConsole = {};

  for (const level of DEFAULT_CONFIG.levels) {
    originalConsole[level] = (console[level] || console.log).bind(console);
  }

  window[INSTALL_KEY] = true;
  window[ORIGINAL_CONSOLE_KEY] = originalConsole;
  window[SESSION_KEY] = window[SESSION_KEY] || makeSessionId();

  for (const level of DEFAULT_CONFIG.levels) {
    console[level] = (...args) => {
      originalConsole[level](...args);

      recordBrowserLog(level, "console", args);
    };
  }

  window.addEventListener("error", (event) => {
    recordBrowserLog("error", "error", [event.error || event.message], {
      filename: event.filename,
      lineno: event.lineno,
      colno: event.colno,
    });
  });

  window.addEventListener("unhandledrejection", (event) => {
    recordBrowserLog("error", "unhandledrejection", [event.reason]);
  });

  window.addEventListener("pagehide", () => flush(true));
  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState === "hidden") {
      flush(true);
    }
  });
};
