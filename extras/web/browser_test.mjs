#!/usr/bin/env node
// Optional UI smoke test. Requires Node 22+ and Chrome; no npm dependencies.
// Uses Chrome's public DevTools Protocol: https://chromedevtools.github.io/devtools-protocol/
// Starts its own server and temporary browser profile, never a personal session.
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { mkdir, mkdtemp, readFile, readlink, rm, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const directory = path.dirname(fileURLToPath(import.meta.url));
const artifacts = path.join(directory, ".build");
const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));
const children = [];
let profile;
let browser;

async function launch(command, args, pattern) {
  const child = spawn(command, args, {
    cwd: directory, stdio: ["ignore", "pipe", "pipe"], detached: process.platform !== "win32",
  });
  children.push(child);
  return new Promise((resolve, reject) => {
    let output = "";
    const timer = setTimeout(() => reject(new Error(`Startup timed out: ${command}\n${output}`)), 60000);
    const collect = (chunk) => {
      output = (output + chunk.toString()).slice(-12000);
      const match = output.match(pattern);
      if (match) {
        clearTimeout(timer);
        resolve(match[0]);
      }
    };
    child.stdout.on("data", collect);
    child.stderr.on("data", collect);
    child.once("error", (error) => { clearTimeout(timer); reject(error); });
    child.once("exit", (code) => {
      clearTimeout(timer);
      reject(new Error(`${command} exited (${code})\n${output}`));
    });
  });
}

async function stop(child) {
  if (child.exitCode !== null || child.signalCode !== null) return;
  const kill = (signal) => {
    try {
      if (process.platform === "win32") child.kill(signal);
      else process.kill(-child.pid, signal);
    } catch (error) {
      if (error.code !== "ESRCH") throw error;
    }
  };
  kill("SIGTERM");
  for (let attempt = 0; attempt < 40; attempt += 1) {
    if (child.exitCode !== null || child.signalCode !== null) return;
    await delay(50);
  }
  kill("SIGKILL");
}

class BrowserPage {
  constructor(socket) {
    this.socket = socket;
    this.sequence = 0;
    this.pending = new Map();
    this.errors = [];
    socket.addEventListener("message", (event) => {
      const message = JSON.parse(event.data);
      if (message.id) {
        const pending = this.pending.get(message.id);
        if (!pending) return;
        clearTimeout(pending.timer);
        this.pending.delete(message.id);
        if (message.error) pending.reject(new Error(JSON.stringify(message.error)));
        else pending.resolve(message.result);
      } else if (message.method === "Runtime.exceptionThrown") {
        this.errors.push(message.params.exceptionDetails);
      }
    });
  }

  static async connect(url) {
    const socket = new WebSocket(url);
    await new Promise((resolve, reject) => {
      socket.addEventListener("open", resolve, { once: true });
      socket.addEventListener("error", reject, { once: true });
    });
    return new BrowserPage(socket);
  }

  call(method, params = {}) {
    const id = ++this.sequence;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`Chrome command timed out: ${method}`));
      }, 10000);
      this.pending.set(id, { resolve, reject, timer });
      this.socket.send(JSON.stringify({ id, method, params }));
    });
  }

  async evaluate(expression) {
    const result = await this.call("Runtime.evaluate", {
      expression, returnByValue: true, awaitPromise: true,
    });
    if (result.exceptionDetails) throw new Error(JSON.stringify(result.exceptionDetails));
    return result.result.value;
  }

  async click(selector) {
    const point = await this.evaluate(`(async () => {
      const element = document.querySelector(${JSON.stringify(selector)});
      if (!element || element.disabled) throw new Error("Unavailable control: " + ${JSON.stringify(selector)});
      element.scrollIntoView({ block: "center", inline: "nearest" });
      await new Promise(requestAnimationFrame);
      const rect = element.getBoundingClientRect();
      if (!rect.width || !rect.height) throw new Error("Hidden control: " + ${JSON.stringify(selector)});
      return { x: rect.x + rect.width / 2, y: rect.y + rect.height / 2 };
    })()`);
    await this.call("Input.dispatchMouseEvent", { type: "mousePressed", ...point, button: "left", clickCount: 1 });
    await this.call("Input.dispatchMouseEvent", { type: "mouseReleased", ...point, button: "left", clickCount: 1 });
  }

  async fill(selector, value) {
    await this.evaluate(`(() => {
      const element = document.querySelector(${JSON.stringify(selector)});
      element.focus(); element.select();
    })()`);
    await this.call("Input.insertText", { text: value });
  }

  async select(selector, value) {
    await this.evaluate(`(() => {
      const element = document.querySelector(${JSON.stringify(selector)});
      element.value = ${JSON.stringify(value)};
      element.dispatchEvent(new Event("change", { bubbles: true }));
    })()`);
  }

  async screenshot(name, fullPage = false) {
    await this.evaluate("window.scrollTo(0, 0)");
    await this.evaluate("new Promise(requestAnimationFrame)");
    const options = { format: "png", fromSurface: true, captureBeyondViewport: fullPage };
    if (fullPage) {
      const { cssContentSize } = await this.call("Page.getLayoutMetrics");
      options.clip = { x: 0, y: 0, width: cssContentSize.width, height: cssContentSize.height, scale: 1 };
    }
    const { data } = await this.call("Page.captureScreenshot", options);
    const filename = path.join(artifacts, name);
    await writeFile(filename, Buffer.from(data, "base64"));
    return filename;
  }
}

async function waitFor(description, predicate) {
  const deadline = Date.now() + 10000;
  while (Date.now() < deadline) {
    const result = await predicate();
    if (result) return result;
    await delay(50);
  }
  throw new Error(`Timed out: ${description}`);
}

async function main() {
  assert.equal(typeof WebSocket, "function", "Use Node 22 or newer.");
  await mkdir(artifacts, { recursive: true });
  profile = await mkdtemp(path.join(artifacts, "browser-profile-"));
  const url = await launch(process.env.WEB_TEST_PYTHON || "python3", ["-u", "server.py", "--port", "0"], /http:\/\/127\.0\.0\.1:\d+/);
  const debuggerUrl = await launch(process.env.WEB_TEST_CHROME || "google-chrome", [
    "--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
    "--remote-debugging-port=0", "--remote-debugging-address=127.0.0.1",
    `--user-data-dir=${profile}`, "--window-size=1440,1050", "about:blank",
  ], /ws:\/\/127\.0\.0\.1:\d+\/devtools\/browser\/[a-zA-Z0-9-]+/);
  const debuggingOrigin = `http://${new URL(debuggerUrl).host}`;
  const targets = await (await fetch(`${debuggingOrigin}/json/list`)).json();
  const target = targets.find((entry) => entry.type === "page");
  assert.ok(target, "Chrome must expose an isolated page target.");
  browser = await BrowserPage.connect(target.webSocketDebuggerUrl);
  await browser.call("Page.enable");
  await browser.call("Runtime.enable");
  await browser.call("Emulation.setDeviceMetricsOverride", { width: 1440, height: 1050, deviceScaleFactor: 1, mobile: false });
  await browser.call("Emulation.setEmulatedMedia", { features: [{ name: "prefers-reduced-motion", value: "reduce" }] });
  await browser.call("Page.navigate", { url });
  const dom = (expression) => browser.evaluate(expression);
  const state = async () => {
    const response = await fetch(`${url}/api/state`);
    assert.equal(response.status, 200);
    return response.json();
  };
  const assertIndividualBook = async () => {
    const snapshot = await state();
    for (const [side, levels] of [["bid", snapshot.book.buys], ["ask", snapshot.book.sells]]) {
      const expected = levels.flatMap((level) => level.orders).map((order) => ({
        id: order.id, price: order.price, quantity: order.remaining_quantity,
      }));
      const actual = await dom(`Array.from(document.querySelectorAll('.book-order.${side}'), (cell) => ({
        id: cell.dataset.bookOrderId, price: cell.dataset.bookPrice,
        quantity: cell.querySelector('.order-quantity').textContent.replaceAll('.', ''),
      }))`);
      assert.deepEqual(actual, expected, `${side}: one row per active order in the engine's priority, without grouping.`);
    }
    assert.equal(await dom('document.querySelector("#book-column-headings").hidden'), true);
    assert.equal(await dom('document.querySelector("#book-content").dataset.mode'), "orders");
  };
  const ready = () => waitFor("connected interface", () => dom(
    'document.querySelector("#connection")?.dataset.state === "online" && !document.querySelector("#submit-order").disabled'
  ));
  const submit = async (type, side, quantity, price) => {
    await ready();
    const before = (await state()).metrics.submitted;
    await browser.click(`[data-order-type="${type}"]`);
    await browser.click(`[data-order-side="${side}"]`);
    if (price) await browser.fill("#order-price", price);
    await browser.fill("#order-quantity", quantity);
    await browser.click("#submit-order");
    await waitFor("submitted order", async () => (await state()).metrics.submitted === before + 1);
    await ready();
    return state();
  };
  await ready();
  assert.equal((await state()).orders.length, 0);
  assert.equal(await dom('document.querySelector(".app-header, .brand, .header-divider")'), null);
  assert.equal(await dom('document.querySelectorAll(".session-toolbar .session-actions button").length'), 2);
  assert.equal(await dom('document.querySelector("h1, .page-heading")'), null);
  const toolbarCentered = () => dom(`(() => {
    const toolbar = document.querySelector(".session-toolbar").getBoundingClientRect();
    const actions = document.querySelector(".session-actions").getBoundingClientRect();
    return Math.abs((toolbar.left + toolbar.right) / 2 - (actions.left + actions.right) / 2) < 1;
  })()`);
  assert.ok(await toolbarCentered(), "Session actions must be centered on desktop.");
  assert.equal(await dom('document.querySelector("#connection").closest("footer") !== null'), true);
  assert.equal(await dom('getComputedStyle(document.documentElement).colorScheme'), "light");
  assert.equal(await dom('getComputedStyle(document.querySelector("h2")).fontFamily === getComputedStyle(document.body).fontFamily'), true);
  assert.equal(await dom('getComputedStyle(document.querySelector(".order-entry")).borderTopWidth'), "0px");
  assert.equal(await dom('getComputedStyle(document.querySelector(".book-panel")).borderTopWidth'), "0px");
  assert.equal(await dom('getComputedStyle(document.querySelector(".book-panel")).backgroundColor'), "rgba(0, 0, 0, 0)");
  assert.equal(await dom('getComputedStyle(document.querySelector(".book-panel")).borderRadius'), "0px");
  assert.equal(await dom('document.querySelector(".empty-icon, .live-indicator")'), null);
  const palette = await dom(`Object.fromEntries(
    ["background", "panel", "surface", "text", "muted", "faint", "navy", "buy", "sell"].map(
      (name) => [name, getComputedStyle(document.documentElement).getPropertyValue("--" + name).trim()]
    )
  )`);
  const rgb = (hex) => [1, 3, 5].map((offset) => Number.parseInt(hex.slice(offset, offset + 2), 16));
  const luminance = (color) => rgb(color).map((value) => {
    const channel = value / 255;
    return channel <= 0.04045 ? channel / 12.92 : ((channel + 0.055) / 1.055) ** 2.4;
  }).reduce((sum, value, index) => sum + value * [0.2126, 0.7152, 0.0722][index], 0);
  const contrast = (foreground, background) => {
    const values = [luminance(foreground), luminance(background)].sort((a, b) => b - a);
    return (values[0] + 0.05) / (values[1] + 0.05);
  };
  for (const text of ["text", "muted", "faint", "navy", "buy", "sell"]) {
    for (const surface of ["background", "panel", "surface"]) {
      assert.ok(contrast(palette[text], palette[surface]) >= 4.5, `${text} must be readable on ${surface}.`);
    }
  }
  for (const side of ["buy", "sell"]) {
    assert.ok(contrast("#ffffff", palette[side]) >= 4.5, `${side} submit text must remain readable.`);
  }
  assert.ok(rgb(palette.buy)[2] > rgb(palette.buy)[0], "Buy must use blue, not green.");
  assert.ok(rgb(palette.sell)[0] > rgb(palette.sell)[2], "Sell must use red.");
  await browser.screenshot("matching-toolbar-empty.png");
  const beforeViewChange = await state();
  await browser.click('[data-book-mode="orders"]');
  await assertIndividualBook();
  assert.deepEqual(await state(), beforeViewChange, "Changing views must not change the engine or consume an ID.");

  await submit("limit", "sell", "150", "10,00");
  await submit("limit", "buy", "100", "9.00");
  await browser.click('[data-order-action="amend"][data-order-id="2"]');
  await browser.fill("#amend-price", "10,00");
  await browser.fill("#amend-quantity", "200");
  // A periodic state refresh must not overwrite a user's unfinished draft.
  await delay(1100);
  assert.equal(await dom('document.querySelector("#amend-quantity").value'), "200");
  await browser.click("#confirm-amend");
  await waitFor("atomic amendment", async () => (await state()).metrics.trade_count === 1);
  await waitFor("amend dialog closed", () => dom('!document.querySelector("#amend-dialog").open'));
  let current = await state();
  assert.equal(current.orders.find((order) => order.id === "2").remaining_quantity, "50");
  assert.equal(current.trades[0].quantity, "150");
  assert.equal(current.trades[0].price, "1000");
  await assertIndividualBook();
  assert.equal(await dom('document.querySelector(".book-order.bid .order-quote").textContent'), "50 @ 10,00");

  await browser.click('[data-order-action="cancel"][data-order-id="2"]');
  await browser.click('[data-close-dialog="cancel-dialog"]');
  assert.equal((await state()).orders.find((order) => order.id === "2").status, "active");
  await browser.click('[data-order-action="cancel"][data-order-id="2"]');
  await browser.click("#confirm-cancel");
  await waitFor("cancel dialog closed", () => dom('!document.querySelector("#cancel-dialog").open'));
  assert.equal((await state()).orders.find((order) => order.id === "2").status, "cancelled");
  await assertIndividualBook();
  current = await submit("market", "sell", "10");
  assert.equal(current.orders[0].status, "cancelled");
  assert.equal(current.metrics.trade_count, 1);

  await browser.fill("#order-quantity", "1e2");
  await browser.click("#submit-order");
  assert.equal(await dom('document.querySelector("#order-quantity").validity.valid'), false);
  assert.equal((await state()).metrics.submitted, 3);
  await submit("limit", "sell", "100", "12");
  current = await submit("peg", "sell", "50");
  assert.equal(current.orders[0].peg_reference, "offer");
  current = await submit("limit", "sell", "100", "11");
  assert.deepEqual(current.book.sells[0].orders.map((order) => order.id), ["5", "6"]);
  await assertIndividualBook();
  assert.deepEqual(await dom('Array.from(document.querySelectorAll(".book-order.ask"), (cell) => cell.dataset.bookOrderId)'), ["5", "6", "4"]);
  await browser.click('[data-book-price="1100"]');
  assert.equal(await dom('document.querySelector("#order-price").value'), "11,00");
  await browser.click('[data-order-action="amend"][data-order-id="5"]');
  assert.equal(await dom('document.querySelector("#amend-price-field").hidden'), true);
  await browser.fill("#amend-quantity", "40");
  await browser.click("#confirm-amend");
  await waitFor("peg quantity amendment", async () => (await state()).orders.find((order) => order.id === "5").remaining_quantity === "40");
  await waitFor("peg dialog closed", () => dom('!document.querySelector("#amend-dialog").open'));
  await assertIndividualBook();

  await browser.click("#status-tab");
  assert.equal(await dom('document.querySelector("#status-panel").hidden'), false);
  await browser.select("#event-filter", "trade");
  assert.equal(await dom('document.querySelectorAll("#events-content .event-row").length'), 1);
  await browser.select("#event-filter", "repriced");
  assert.equal(await dom('document.querySelectorAll("#events-content .event-row").length'), 1);
  await browser.select("#event-filter", "all");
  await browser.fill("#event-search", "no-matching-message-123");
  assert.equal(await dom('document.querySelectorAll("#events-content .event-row").length'), 0);
  await browser.fill("#event-search", "");

  const previousSession = (await state()).session_id;
  await browser.click("#reset-button");
  await browser.click('[data-close-dialog="reset-dialog"]');
  assert.equal((await state()).session_id, previousSession);
  await browser.click("#reset-button");
  await browser.click("#confirm-reset");
  await waitFor("new empty session", async () => (await state()).session_id !== previousSession);
  await waitFor("reset dialog closed", () => dom('!document.querySelector("#reset-dialog").open'));
  assert.equal((await state()).orders.length, 0);
  await browser.click("#desk-tab");
  await assertIndividualBook();
  await browser.click('[data-book-mode="levels"]');
  assert.equal(await dom('document.querySelector("#book-column-headings").hidden'), false);
  await browser.click("#demo-button");
  await waitFor("demo order book", async () => (await state()).metrics.submitted === 20);
  await ready();
  assert.equal(await dom('document.querySelector("#demo-button").disabled'), true);
  assert.equal(await dom('document.querySelectorAll("#book-content .book-row").length'), 8);
  await browser.click('[data-book-mode="orders"]');
  await assertIndividualBook();
  assert.equal(await dom('document.querySelectorAll(".book-order").length'), 18);
  assert.equal(await dom('document.querySelectorAll(\'.book-order.bid[data-book-price="10000"]\').length'), 2);
  await browser.screenshot("matching-book-orders-desktop.png");
  await browser.click('[data-book-mode="cumulative"]');
  const amounts = await dom('Array.from(document.querySelectorAll(".book-cell.bid .level-quantity"), (node) => Number(node.textContent.replaceAll(".", "")))');
  assert.ok(amounts.every((amount, index) => index === 0 || amount >= amounts[index - 1]));
  await browser.click('[data-order-side="buy"]');
  await browser.fill("#order-price", "100,05");
  await browser.fill("#order-quantity", "150");
  await browser.click("#dismiss-notification");
  assert.ok(await dom("document.documentElement.scrollWidth <= window.innerWidth + 1"), "Desktop must not overflow horizontally.");
  const desktop = await browser.screenshot("matching-desk-desktop.png");
  await browser.click("#status-tab");
  const statusScreenshot = await browser.screenshot("matching-desk-status.png");
  await browser.click("#desk-tab");
  for (const width of [320, 680, 768, 1024, 1920]) {
    await browser.call("Emulation.setDeviceMetricsOverride", { width, height: 1050, deviceScaleFactor: 1, mobile: false });
    assert.ok(await dom("document.documentElement.scrollWidth <= window.innerWidth + 1"), `Trading layout must fit at ${width}px.`);
    assert.ok(await toolbarCentered(), `Toolbar must remain centered at ${width}px.`);
    await browser.click('[data-book-mode="orders"]');
    await assertIndividualBook();
    assert.ok(await dom("document.documentElement.scrollWidth <= window.innerWidth + 1"), `Individual book must fit at ${width}px.`);
    await browser.click('[data-book-mode="cumulative"]');
    await browser.click("#status-tab");
    assert.ok(await dom("document.documentElement.scrollWidth <= window.innerWidth + 1"), `Status layout must fit at ${width}px.`);
    await browser.click("#desk-tab");
  }
  await browser.evaluate("window.scrollTo(0, 0)");
  await browser.screenshot("matching-desk-wide.png");
  await browser.call("Emulation.setDeviceMetricsOverride", { width: 390, height: 844, deviceScaleFactor: 1, mobile: false });
  await submit("limit", "buy", "25", "99.00");
  await browser.click("#dismiss-notification");
  assert.ok(await dom("document.documentElement.scrollWidth <= window.innerWidth + 1"), "Mobile must not overflow horizontally.");
  assert.ok(await toolbarCentered(), "Session actions must remain centered on mobile.");
  const mobile = await browser.screenshot("matching-desk-mobile.png", true);
  await browser.click('[data-book-mode="orders"]');
  await assertIndividualBook();
  await browser.screenshot("matching-book-orders-mobile.png", true);
  await browser.click('.book-order.bid');
  assert.equal(await dom('document.querySelector("#order-price").value'), "100,00");

  // The crossing-limit dialog is exercised against the real server, including
  // a concurrent tab changing the quote between preview and confirmation.
  const apiPost = async (endpoint, body) => {
    const response = await fetch(`${url}${endpoint}`, {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
    assert.equal(response.status, 200);
    return response.json();
  };
  const resetForConfirmation = await apiPost("/api/reset", {});
  await waitFor("reset reflected in UI", () => dom(`state.session_id === ${JSON.stringify(resetForConfirmation.session_id)}`));
  await browser.call("Emulation.setDeviceMetricsOverride", { width: 1440, height: 1050, deviceScaleFactor: 1, mobile: false });
  await submit("limit", "sell", "50", "10");
  const previewLimit = async (side, value, amount) => {
    await ready();
    await browser.click('[data-order-type="limit"]');
    await browser.click(`[data-order-side="${side}"]`);
    await browser.fill("#order-price", value);
    await browser.fill("#order-quantity", amount);
    await browser.click("#submit-order");
    await waitFor("crossing-limit confirmation", () => dom('document.querySelector("#limit-dialog").open'));
    await ready();
  };
  const beforePreview = await state();
  await previewLimit("buy", "11", "20");
  assert.deepEqual(await state(), beforePreview, "A preview must not mutate even revision or history.");
  assert.match(await dom('document.querySelector("#limit-description").textContent'), /11,00/);
  assert.match(await dom('document.querySelector("#limit-best-price").textContent'), /10,00/);
  assert.equal(await dom('document.activeElement.id'), "decline-limit");
  const crossingDesktop = await browser.screenshot("matching-limit-confirmation-desktop.png");
  await browser.click("#decline-limit");
  await waitFor("declined dialog closed", () => dom('!document.querySelector("#limit-dialog").open'));
  assert.deepEqual(await state(), beforePreview);

  await previewLimit("buy", "11", "20");
  await browser.call("Input.dispatchKeyEvent", { type: "keyDown", key: "Escape", code: "Escape", windowsVirtualKeyCode: 27 });
  await browser.call("Input.dispatchKeyEvent", { type: "keyUp", key: "Escape", code: "Escape", windowsVirtualKeyCode: 27 });
  await waitFor("Escape closes confirmation", () => dom('!document.querySelector("#limit-dialog").open'));
  assert.deepEqual(await state(), beforePreview);

  await previewLimit("buy", "11", "20");
  const concurrent = await apiPost("/api/command", { action: "limit", side: "sell", price: "9.50", quantity: "50" });
  await browser.click("#confirm-limit");
  await waitFor("updated quote requires consent again", () => dom('!document.querySelector("#limit-changed").hidden'));
  await ready();
  assert.match(await dom('document.querySelector("#limit-best-price").textContent'), /9,50/);
  assert.deepEqual(await state(), concurrent, "Stale consent must not execute.");
  await browser.call("Emulation.setDeviceMetricsOverride", { width: 390, height: 844, deviceScaleFactor: 1, mobile: false });
  assert.ok(await dom('(() => { const d = document.querySelector("#limit-dialog"); return d.scrollWidth <= d.clientWidth + 1; })()'), "Confirmation must fit on mobile.");
  const crossingMobile = await browser.screenshot("matching-limit-confirmation-mobile.png");
  await browser.click("#confirm-limit");
  await waitFor("confirmed crossing executed", async () => (await state()).metrics.submitted === 3);
  await waitFor("confirmed dialog closed", () => dom('!document.querySelector("#limit-dialog").open'));
  assert.equal((await state()).metrics.executed_quantity, "20");
  assert.equal((await state()).trades[0].price, "950");

  await submit("limit", "buy", "10", "9");
  await previewLimit("sell", "9", "5");
  assert.match(await dom('document.querySelector("#limit-best-price").textContent'), /compra \(bid\).*9,00/);
  await browser.click("#confirm-limit");
  await waitFor("sell crossing executed", async () => (await state()).metrics.submitted === 5);
  await waitFor("sell confirmation closed", () => dom('!document.querySelector("#limit-dialog").open'));
  assert.equal((await state()).trades[0].price, "900");
  await previewLimit("buy", "11", "20");
  const resetWithOpenDialog = await apiPost("/api/reset", {});
  await waitFor("session reset discards confirmation", () => dom(`state.session_id === ${JSON.stringify(resetWithOpenDialog.session_id)} && !document.querySelector("#limit-dialog").open`));
  assert.equal((await state()).metrics.submitted, 0);

  // The assignment's two same-price limits stay distinct until market orders
  // consume them in FIFO order. Only the unfilled remainder is displayed.
  await browser.click('[data-book-mode="orders"]');
  await submit("limit", "buy", "100", "10");
  await submit("limit", "sell", "100", "20");
  await submit("limit", "sell", "200", "20");
  await assertIndividualBook();
  assert.deepEqual(await dom('Array.from(document.querySelectorAll(".book-order.ask"), (cell) => cell.textContent)'), ["100 @ 20,00", "200 @ 20,00"]);
  await submit("market", "buy", "150");
  await assertIndividualBook();
  assert.equal(await dom('document.querySelector(".book-order.ask").dataset.bookOrderId'), "3");
  assert.equal(await dom('document.querySelector(".book-order.ask").textContent'), "150 @ 20,00");
  await submit("market", "buy", "200");
  await assertIndividualBook();
  assert.equal(await dom('document.querySelectorAll(".book-order.ask").length'), 0);
  await submit("market", "sell", "200");
  await assertIndividualBook();
  assert.equal(await dom('document.querySelector("#book-content .empty-state strong").textContent'), "Livro vazio");

  // Direct ID actions reuse the same dialogs, independently of table filters.
  const beforeInvalidId = await state();
  for (const value of ["", "0", "-1", "1e2", "#abc", "999999"]) {
    await browser.fill("#manage-order-id", value);
    await browser.click("#amend-by-id");
    assert.equal(await dom('document.querySelector("#manage-order-error").hidden'), false);
    assert.equal(await dom('document.querySelector("#amend-dialog").open'), false);
    assert.deepEqual(await state(), beforeInvalidId);
  }
  await browser.fill("#manage-order-id", "1");
  await browser.click("#cancel-by-id");
  assert.match(await dom('document.querySelector("#manage-order-error").textContent'), /não está aberta/);
  assert.equal(await dom('document.querySelector("#cancel-dialog").open'), false);

  current = await submit("limit", "buy", "100", "10");
  const managedId = current.book.buys[0].orders[0].id;
  await browser.select("#order-side-filter", "sell");
  assert.equal(await dom(`document.querySelector('[data-order-row="${managedId}"]')`), null);
  await browser.fill("#manage-order-id", `#00${managedId}`);
  const beforeIdDialog = await state();
  await browser.click("#amend-by-id");
  assert.equal(await dom('document.querySelector("#amend-title").textContent'), `Alterar ordem #${managedId}`);
  assert.equal(await dom('document.querySelector("#amend-price").value'), "10,00");
  assert.equal(await dom('document.querySelector("#amend-quantity").value'), "100");
  assert.deepEqual(await state(), beforeIdDialog, "ID lookup must not submit an amendment.");
  await browser.click('[data-close-dialog="amend-dialog"]');

  const amendById = async (newPrice, newQuantity) => {
    await ready();
    await browser.click("#amend-by-id");
    if (newPrice !== undefined) await browser.fill("#amend-price", newPrice);
    if (newQuantity !== undefined) await browser.fill("#amend-quantity", newQuantity);
    await browser.click("#confirm-amend");
    await waitFor("ID amendment completed", () => dom('!document.querySelector("#amend-dialog").open'));
    await ready();
    await assertIndividualBook();
    return (await state()).orders.find((order) => order.id === managedId);
  };
  let managedOrder = await amendById("9,50");
  assert.equal(managedOrder.price, "950");
  assert.equal(managedOrder.remaining_quantity, "100");
  managedOrder = await amendById(undefined, "80");
  assert.equal(managedOrder.price, "950");
  assert.equal(managedOrder.remaining_quantity, "80");
  managedOrder = await amendById("9,70", "125");
  assert.equal(managedOrder.price, "970");
  assert.equal(managedOrder.remaining_quantity, "125");

  const beforeIdCancel = await state();
  await browser.click("#cancel-by-id");
  assert.equal(await dom('document.querySelector("#cancel-title").textContent'), `Cancelar ordem #${managedId}?`);
  assert.deepEqual(await state(), beforeIdCancel, "ID lookup must not cancel without confirmation.");
  await browser.click('[data-close-dialog="cancel-dialog"]');
  assert.deepEqual(await state(), beforeIdCancel);
  await browser.click("#cancel-by-id");
  await browser.click("#confirm-cancel");
  await waitFor("ID cancellation completed", () => dom('!document.querySelector("#cancel-dialog").open'));
  assert.equal((await state()).orders.find((order) => order.id === managedId).status, "cancelled");
  await assertIndividualBook();
  await browser.click("#amend-by-id");
  assert.match(await dom('document.querySelector("#manage-order-error").textContent'), /cancelada/);

  await submit("limit", "buy", "100", "10");
  current = await submit("peg", "buy", "50");
  const pegId = current.book.buys[0].orders.find((order) => order.type === "peg").id;
  await browser.fill("#manage-order-id", pegId);
  await browser.click("#amend-by-id");
  assert.equal(await dom('document.querySelector("#amend-price-field").hidden'), true);
  await browser.fill("#amend-quantity", "40");
  await browser.click("#confirm-amend");
  await waitFor("ID pegged amendment completed", () => dom('!document.querySelector("#amend-dialog").open'));
  assert.equal((await state()).orders.find((order) => order.id === pegId).remaining_quantity, "40");

  // A reset must discard the typed ID and close its dialog: IDs are reused.
  await browser.click("#amend-by-id");
  const resetWithIdDialog = await apiPost("/api/reset", {});
  await waitFor("ID dialog discarded on reset", () => dom(`state.session_id === ${JSON.stringify(resetWithIdDialog.session_id)} && !document.querySelector("#amend-dialog").open`));
  assert.equal(await dom('document.querySelector("#manage-order-id").value'), "");
  assert.equal(await dom('document.querySelector("#manage-order-error").hidden'), true);

  if (process.platform === "linux") {
    // Only terminate the bridge owned by our newly spawned test server.
    const serverPid = children[0].pid;
    const childIds = (await readFile(`/proc/${serverPid}/task/${serverPid}/children`, "utf8")).trim().split(/\s+/);
    assert.equal(childIds.length, 1, "The test server should own exactly one bridge process.");
    const bridgePid = Number(childIds[0]);
    assert.equal(await readlink(`/proc/${bridgePid}/exe`), path.join(artifacts, "bridge"));
    const beforeFailure = await state();
    process.kill(bridgePid, "SIGTERM");
    await waitFor("offline engine indicator", () => dom('document.querySelector("#connection").dataset.state === "offline"'));
    assert.equal(await dom('document.querySelector("#submit-order").disabled'), true);
    assert.equal(await dom('document.querySelector("#amend-by-id").disabled'), true);
    assert.equal(await dom('document.querySelector("#cancel-by-id").disabled'), true);
    assert.equal(await dom('document.querySelector("#recover-button").hidden'), false);
    const failedResponse = await fetch(`${url}/api/state`);
    assert.equal(failedResponse.status, 503);
    assert.deepEqual((await failedResponse.json()).book, beforeFailure.book);
    await browser.click("#recover-button");
    assert.equal(await dom('document.querySelector("#reset-dialog").open'), true);
    await browser.click("#confirm-reset");
    await ready();
    const recovered = await state();
    assert.equal(recovered.orders.length, 0);
    assert.notEqual(recovered.session_id, beforeFailure.session_id);
  }
  assert.deepEqual(browser.errors, [], "The browser must not report uncaught JavaScript exceptions.");
  console.log("Browser OK: order entry, matching, amendments, cancellation, pegged, activity filters, reset confirmation, demo and mobile.");
  console.log("Presentation OK: light palette, text contrast, blue/red sides and layouts from 320px to 1920px.");
  console.log("Individual book OK: no grouping, remaining quantities, cancellation, pegged priority, reset and price selection.");
  console.log("ID actions OK: invalid/missing/closed IDs, filtered orders, price/quantity/both amendments, pegged, cancel confirmation and session reset.");
  console.log("Crossing limits: buy/sell, decline, Escape, stale quote, reset, desktop and mobile OK.");
  console.log(process.platform === "linux" ? "Engine failure and recovery through the UI: OK." : "Engine failure UI test skipped: child-process discovery requires Linux /proc.");
  console.log(JSON.stringify({ desktop, status: statusScreenshot, mobile, crossingDesktop, crossingMobile }, null, 2));
}

try {
  await main();
} catch (error) {
  console.error(error);
  process.exitCode = 1;
} finally {
  if (browser) browser.socket.close();
  for (const child of children.reverse()) await stop(child);
  // This is only the mkdtemp profile created by this test, never a personal one.
  if (profile) await rm(profile, { recursive: true, force: true });
}
