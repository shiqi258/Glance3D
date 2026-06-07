import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";

const __filename = fileURLToPath(import.meta.url);
const scriptsDir = path.dirname(__filename);
const webDir = path.resolve(scriptsDir, "..");
const configPath = path.join(webDir, "browser-log.config.json");

const config = JSON.parse(fs.readFileSync(configPath, "utf8"));
const logFile = path.resolve(
  webDir,
  config.logFile || "../../../logs/browser-console.jsonl",
);

fs.mkdirSync(path.dirname(logFile), { recursive: true });
fs.writeFileSync(logFile, "", "utf8");

console.log(`Cleared browser console log: ${logFile}`);
