// Boot an exported Godot web project in headless Chromium, capture console
// output and a screenshot, fail on page errors.
// Usage: node smoke.mjs <url> <out.png> [--chromium-flag ...]
// Prereq: npm i playwright && npx playwright install chromium
import { chromium } from "playwright";

const [, , url, outPng, ...flags] = process.argv;
if (!url || !outPng) {
  console.error("usage: node smoke.mjs <url> <out.png> [chromium flags...]");
  process.exit(2);
}

const browser = await chromium.launch({ args: flags });
const page = await browser.newPage();
const logs = [];
page.on("console", (m) => logs.push(`[${m.type()}] ${m.text()}`));
page.on("pageerror", (e) => logs.push(`[pageerror] ${e.message}`));

try {
  await page.goto(url, { waitUntil: "networkidle", timeout: 60000 });
  await page.waitForTimeout(8000); // engine boot; tighten later with a boot signal
  await page.screenshot({ path: outPng });
} finally {
  console.log(logs.join("\n"));
  await browser.close();
}

const bad = logs.filter(
  (l) => l.startsWith("[pageerror]") || l.startsWith("[error]")
);
if (bad.length) {
  console.error(`\nFAIL: ${bad.length} error(s) in console.`);
  process.exit(1);
}
console.log("\nPASS: no page errors.");
