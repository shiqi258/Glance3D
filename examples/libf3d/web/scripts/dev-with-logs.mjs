// One command to capture everything the web viewer prints — including
// browser-native WebGL/GPU diagnostics — into a desktop-aligned log file.
//
// It (1) starts the Vite dev server, (2) launches Edge/Chrome with remote
// debugging against a throwaway profile, and (3) starts the CDP collector that
// mirrors the full DevTools console to %LOCALAPPDATA%\f3d\logs\f3d_*_web.log.
//
//   npm run dev:logged
//
// Ctrl+C tears the dev server and collector down. Override the browser with
// GLANCE3D_BROWSER=<path-to-chrome-or-edge>.

import { spawn } from "child_process";
import fs from "fs";
import os from "os";
import path from "path";
import { fileURLToPath } from "url";

import { resolveLogDir } from "./glance3d-weblog.mjs";

const __filename = fileURLToPath(import.meta.url);
const scriptsDir = path.dirname(__filename);
const webDir = path.resolve(scriptsDir, "..");

const config = (() => {
  try {
    return JSON.parse(
      fs.readFileSync(path.join(webDir, "browser-log.config.json"), "utf8"),
    );
  } catch {
    return {};
  }
})();
const port = Number(config.devtools?.port ?? 9222);
const userDataDir = path.join(os.tmpdir(), "glance3d-cdp");

const children = [];
let tornDown = false;

const teardown = () => {
  if (tornDown) {
    return;
  }
  tornDown = true;
  for (const child of children) {
    try {
      child.kill();
    } catch {
      // already gone
    }
  }
};

process.on("SIGINT", () => {
  teardown();
  process.exit(0);
});
process.on("exit", teardown);

const findBrowser = () => {
  if (process.env.GLANCE3D_BROWSER) {
    return process.env.GLANCE3D_BROWSER;
  }

  if (process.platform === "win32") {
    const pf = process.env["ProgramFiles"] || "C:\\Program Files";
    const pfx86 =
      process.env["ProgramFiles(x86)"] || "C:\\Program Files (x86)";
    const local = process.env["LOCALAPPDATA"] || "";
    const candidates = [
      path.join(pf, "Google/Chrome/Application/chrome.exe"),
      path.join(pfx86, "Google/Chrome/Application/chrome.exe"),
      path.join(local, "Google/Chrome/Application/chrome.exe"),
      path.join(pfx86, "Microsoft/Edge/Application/msedge.exe"),
      path.join(pf, "Microsoft/Edge/Application/msedge.exe"),
    ];
    return candidates.find((candidate) => {
      try {
        return fs.existsSync(candidate);
      } catch {
        return false;
      }
    });
  }

  // macOS / Linux: rely on PATH (or GLANCE3D_BROWSER above).
  return process.platform === "darwin"
    ? "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
    : "google-chrome";
};

const viteBin = path.join(
  webDir,
  "node_modules",
  ".bin",
  process.platform === "win32" ? "vite.cmd" : "vite",
);

const startVite = () =>
  new Promise((resolve, reject) => {
    const useNpx = !fs.existsSync(viteBin);
    const command = useNpx ? (process.platform === "win32" ? "npx.cmd" : "npx") : viteBin;
    const cmdArgs = useNpx ? ["vite"] : [];
    const vite = spawn(command, cmdArgs, {
      cwd: webDir,
      shell: process.platform === "win32",
      stdio: ["ignore", "pipe", "pipe"],
    });
    children.push(vite);

    const urlPattern = /(https?:\/\/(?:localhost|127\.0\.0\.1|\[::1\]):\d+\/?)/i;
    let settled = false;
    const onData = (buffer) => {
      const text = buffer.toString();
      process.stdout.write(text);
      const match = text.match(urlPattern);
      if (match && !settled) {
        settled = true;
        resolve(match[1].replace(/\/+$/, "/"));
      }
    };

    vite.stdout.on("data", onData);
    vite.stderr.on("data", (buffer) => process.stderr.write(buffer.toString()));
    vite.on("exit", (code) => {
      if (!settled) {
        reject(new Error(`Vite exited before becoming ready (code ${code})`));
      } else if (!tornDown) {
        teardown();
        process.exit(code ?? 0);
      }
    });

    // Fallback if the URL banner is never matched.
    setTimeout(() => {
      if (!settled) {
        settled = true;
        console.warn("Could not detect the Vite URL; assuming http://localhost:5173/");
        resolve("http://localhost:5173/");
      }
    }, 30000);
  });

const launchBrowser = (devUrl) => {
  const browser = findBrowser();
  if (!browser) {
    console.warn(
      "No Chrome/Edge found. Set GLANCE3D_BROWSER to a Chromium browser, " +
        `or open it manually with --remote-debugging-port=${port} at ${devUrl}`,
    );
    return;
  }

  const browserArgs = [
    `--remote-debugging-port=${port}`,
    `--user-data-dir=${userDataDir}`,
    "--no-first-run",
    "--no-default-browser-check",
    "--new-window",
    devUrl,
  ];
  console.log(`Launching ${path.basename(browser)} with remote debugging on port ${port}`);
  const child = spawn(browser, browserArgs, {
    stdio: "ignore",
    detached: false,
  });
  child.on("error", (error) =>
    console.warn(`Failed to launch browser: ${error.message}`),
  );
  children.push(child);
};

const startCollector = (devUrl) => {
  const host = new URL(devUrl).hostname;
  const collector = spawn(
    process.execPath,
    [
      path.join(scriptsDir, "collect-browser-devtools-logs.mjs"),
      `--port=${port}`,
      `--url=${host}`,
    ],
    { cwd: webDir, stdio: "inherit" },
  );
  children.push(collector);
};

const main = async () => {
  const logDir = resolveLogDir();
  if (logDir) {
    console.log(`Web logs -> ${path.join(logDir, "g3d_*_web.log")}`);
  } else {
    console.warn("G3D_LOG_FILE disables file logging; collector will be a no-op.");
  }

  const devUrl = await startVite();
  console.log(`Vite ready at ${devUrl}`);
  launchBrowser(devUrl);
  startCollector(devUrl);
};

main().catch((error) => {
  console.error(error.message);
  teardown();
  process.exit(1);
});
