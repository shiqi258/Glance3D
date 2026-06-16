// Desktop-aligned log file helper for the web viewer.
//
// Mirrors application/F3DLogFile.cxx so that web-viewer logs land in the same
// place, with the same naming/rotation/format conventions as the desktop app:
//   - Directory:  <user-cache>/Glance3D/logs   (Windows: %LOCALAPPDATA%\Glance3D\logs)
//   - Filename:   g3d_YYYYMMDD_HHMMSS_mmm_web.log   (one file per session)
//   - Line:       [HH:MM:SS.mmm] [LEVEL] [source] message
//   - Rotation:   keep the newest G3D_LOG_KEEP files (default 10)
// Honors the same env switches as desktop: G3D_LOG_FILE, G3D_LOG_DIR, G3D_LOG_KEEP.
//
// The `_web.log` suffix keeps web logs distinguishable from desktop `g3d_*.log`
// files while still matching the desktop `g3d_*.log` glob, and the embedded
// timestamp keeps both sets sorting chronologically so rotation prunes the
// genuinely-oldest file regardless of which app wrote it.

import fs from "fs";
import os from "os";
import path from "path";

const pad = (value, width = 2) => String(value).padStart(width, "0");

const fileStamp = (date) =>
  `${date.getFullYear()}${pad(date.getMonth() + 1)}${pad(date.getDate())}_` +
  `${pad(date.getHours())}${pad(date.getMinutes())}${pad(date.getSeconds())}_` +
  `${pad(date.getMilliseconds(), 3)}`;

const clockStamp = (date) =>
  `${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}.` +
  `${pad(date.getMilliseconds(), 3)}`;

const dateStamp = (date) =>
  `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())} ` +
  `${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}`;

// <user-cache>/Glance3D, matching F3DSystemTools::GetUserCacheDirectory().
const userCacheDir = () => {
  if (process.platform === "win32") {
    const base =
      process.env.LOCALAPPDATA || path.join(os.homedir(), "AppData", "Local");
    return path.join(base, "Glance3D");
  }
  if (process.platform === "darwin") {
    return path.join(os.homedir(), "Library", "Caches", "Glance3D");
  }
  const base = process.env.XDG_CACHE_HOME || path.join(os.homedir(), ".cache");
  return path.join(base, "Glance3D");
};

// Resolve the log directory, or null when file logging is disabled.
export const resolveLogDir = () => {
  const disable = (process.env.G3D_LOG_FILE || "").toLowerCase();
  if (["0", "false", "off", "no"].includes(disable)) {
    return null;
  }

  const override = process.env.G3D_LOG_DIR;
  if (override && override.trim()) {
    return override;
  }

  return path.join(userCacheDir(), "logs");
};

const resolveKeep = (fallback) => {
  const envKeep = Number.parseInt(process.env.G3D_LOG_KEEP ?? "", 10);
  const keep = Number.isInteger(envKeep) ? envKeep : (fallback ?? 10);
  return Math.max(keep, 1);
};

// Prune old web logs, scoped to `g3d_*_web.log` so we never touch desktop logs.
export const pruneOldWebLogs = (logDir, keep) => {
  const allowedExisting = Math.max(resolveKeep(keep) - 1, 0);
  let names = [];
  try {
    names = fs.readdirSync(logDir);
  } catch {
    return;
  }

  const logs = names.filter((name) => /^g3d_.*_web\.log$/.test(name)).sort();
  const toRemove = logs.length - allowedExisting;
  for (let i = 0; i < toRemove; i += 1) {
    try {
      fs.unlinkSync(path.join(logDir, logs[i]));
    } catch {
      // Best-effort cleanup; ignore filesystem errors.
    }
  }
};

export const normalizeLevel = (value) => {
  switch (String(value ?? "").toLowerCase()) {
    case "verbose":
    case "debug":
      return "DEBUG";
    case "warning":
    case "warn":
      return "WARN";
    case "error":
      return "ERROR";
    case "info":
    case "log":
      return "INFO";
    default:
      return "INFO";
  }
};

// Open a new per-session log file and return a writer, or null when disabled.
// `explicitFile`, when given, is used verbatim (no rotation, append mode) so
// callers can force a specific path via --log-file.
export const startWebLogSession = ({ source = "web", meta = [], explicitFile } = {}) => {
  let file = explicitFile;

  if (!file) {
    const logDir = resolveLogDir();
    if (!logDir) {
      return null;
    }
    try {
      fs.mkdirSync(logDir, { recursive: true });
    } catch {
      return null;
    }
    pruneOldWebLogs(logDir);
    file = path.join(logDir, `g3d_${fileStamp(new Date())}_web.log`);
  } else {
    try {
      fs.mkdirSync(path.dirname(file), { recursive: true });
    } catch {
      return null;
    }
  }

  const now = new Date();
  const headerLines = [
    `==== Glance3D Web (${source}) ====`,
    `Session start: ${dateStamp(now)}`,
    ...meta,
    `Log file: ${file}`,
    "========================================",
  ];

  try {
    fs.writeFileSync(file, `${headerLines.join("\n")}\n`);
  } catch {
    return null;
  }

  return {
    file,
    write(level, src, message) {
      const line = `[${clockStamp(new Date())}] [${level}] [${src}] ${message}\n`;
      try {
        fs.appendFileSync(file, line);
      } catch {
        // Logging must never disrupt the collector.
      }
    },
  };
};
