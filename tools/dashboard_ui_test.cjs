/* Browser regression tests for the embedded dashboard. Requires Playwright.
 * NODE_PATH=/path/to/node_modules BROWSER_EXECUTABLE=/path/to/browser node tools/dashboard_ui_test.cjs
 * All API writes and failure simulations are confined to the local fixture server.
 */
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const http = require("node:http");
const { chromium } = require("playwright");
const root = path.resolve(__dirname, "..");
const source = fs.readFileSync(
  path.join(root, "src/api/dashboard_html.inc"),
  "utf8",
);
const html = source.split('R"HTMLDOC(')[1].split(')HTMLDOC"')[0];
const output = path.resolve(
  process.env.DASHBOARD_TEST_OUTPUT ||
    path.join(root, "logs/ui-implementation/qa"),
);
fs.mkdirSync(output, { recursive: true });
const today = Date.parse(new Date().toISOString().slice(0, 10));
let apiBusy = false,
  apiError = false,
  statsError = false,
  chatMode = "normal",
  saved = {},
  posts = [],
  memoryCalls = 0,
  reads = 0,
  authRequired = false,
  persistFails = false;
let healthRequests = 0,
  summaryRequests = 0,
  tailRequests = 0;
const health = () => ({
  status: "ok",
  model: "qwen3.8-flash-next",
  slots: 2,
  slot_ctx: 262144,
  context: 262144,
  in_flight: apiBusy ? 2 : 0,
  busy: apiBusy,
  busy_for_s: apiBusy ? 4.2 : 0,
  queued: apiBusy ? 1 : 0,
  server_overrides: saved,
  tool_calls: { parsed: true, streaming: true },
  vision: { accepted: true, max_images: 16 },
});
const summary = {
  requests: 16,
  input_tokens: 37482,
  cached_tokens: 8510,
  output_tokens: 81,
  cache_hit_rate: 0.227,
  prefill_seconds: 25.4,
  prefill_tok_per_s: 1141.8,
  decode_seconds: 2.7,
  decode_tok_per_s: 29.5,
  ttft: { count: 16, p50_ms: 506, p95_ms: 6630 },
  finish: { stop: 12, length: 1, error: 1, cancel: 1, other: 1 },
  drafters: {
    mtp: {
      requests: 16,
      accepted: 33,
      proposed: 68,
      acceptance: 33 / 68,
      decode_tok_per_s: 29.5,
    },
  },
};
const records = Array.from({ length: 14 }, (_, i) => ({
  ts_ms: today + 60000 + i * 8000,
  req_seq: i + 1,
  finish: ["stop", "length", "cancel", "error", "other"][i % 5],
  drafter: "mtp",
  vision: i === 3,
  prompt_tokens: 8018 + i,
  cached_tokens: 0,
  output_tokens: 4 + i,
  prefill_ms: 6000 + i * 50,
  decode_ms: 180 + i * 5,
  ttft_ms: 6162 + i * 10,
  proposed: 68,
  commit: 67,
  rounds: 34,
  acceptance: 33 / 68,
}));
records[0].ts_ms = today - 1000;
records[1].cached_tokens = records[1].prompt_tokens;
records[2].ttft_ms = 0;
const memory = {
  gpu_accessible_committed_bytes: 80 * 2 ** 30,
  accounting: "device + registered mmap + pinned",
  hip_total_bytes: 96 * 2 ** 30,
  hip_free_bytes: 24 * 2 ** 30,
  device_current_bytes: 20 * 2 ** 30,
  device_peak_bytes: 24 * 2 ** 30,
  registered_mmap_bytes: 58 * 2 ** 30,
  pinned_host_current_bytes: 2 * 2 ** 30,
  pinned_host_peak_bytes: 3 * 2 ** 30,
  process_rss_bytes: 84 * 2 ** 30,
};
const calls = [];
const server = http.createServer(async (req, res) => {
  const u = new URL(req.url, "http://localhost");
  calls.push(u.pathname);
  const json = (data, status = 200) => {
    res.writeHead(status, { "Content-Type": "application/json" });
    res.end(JSON.stringify(data));
  };
  if (u.pathname === "/" || u.pathname === "/dashboard") {
    res.writeHead(200, { "Content-Type": "text/html; charset=utf-8" });
    return res.end(html);
  }
  if (u.pathname === "/favicon.ico") {
    res.writeHead(204);
    return res.end();
  }
  if (u.pathname === "/health") {
    healthRequests++;
    return apiError
      ? json({ error: { message: "fixture disconnected" } }, 503)
      : json(health());
  }
  if (u.pathname === "/reqstat/summary") {
    summaryRequests++;
    return statsError
      ? json({ error: { message: "fixture statistics failure" } }, 500)
      : json(summary);
  }
  if (u.pathname === "/reqstat/tail") {
    tailRequests++;
    return json({ records });
  }
  if (u.pathname === "/memory") {
    memoryCalls++;
    return json(memory);
  }
  if (u.pathname === "/admin/overrides") {
    if (req.method === "GET") {
      reads++;
      return json({
        overrides: saved,
        persist_path: "data/test-overrides.json",
        admin_key_required: authRequired,
      });
    }
    let raw = "";
    for await (const part of req) raw += part;
    posts.push({ body: JSON.parse(raw), key: req.headers["x-admin-key"] });
    if (authRequired && req.headers["x-admin-key"] !== "fixture-key")
      return json({ error: { message: "需要管理密钥" } }, 401);
    saved = JSON.parse(raw);
    return json({
      overrides: saved,
      persist_path: "data/test-overrides.json",
      saved: !persistFails,
      save_error: persistFails ? "fixture disk full" : undefined,
      admin_key_required: authRequired,
    });
  }
  if (u.pathname === "/v1/chat/completions") {
    let raw = "";
    for await (const part of req) raw += part;
    const body = JSON.parse(raw);
    chatBodies.push(body);
    res.writeHead(200, {
      "Content-Type": "text/event-stream",
      "X-Gdec-Overrides": "temperature",
      "Cache-Control": "no-cache",
    });
    const event = (obj) => "data: " + JSON.stringify(obj) + "\r\n\r\n";
    if (chatMode === "slow") {
      res.write(event({ choices: [{ delta: { content: "正在生成" } }] }));
      const t = setInterval(
        () => res.write(event({ choices: [{ delta: { content: "。" } }] })),
        60,
      );
      res.on("close", () => clearInterval(t));
      return;
    }
    let stream =
      event({
        choices: [{ delta: { reasoning_content: "检查中文流式解码。" } }],
      }) +
      event({
        choices: [
          {
            delta: {
              content: '<img src=x onerror="window.injected=1">你好，世界。',
            },
          },
        ],
      });
    if (chatMode === "normal")
      stream +=
        event({
          choices: [{ delta: {}, finish_reason: "stop" }],
          usage: { prompt_tokens: 20, completion_tokens: 7 },
        }) + "data: [DONE]\r\n\r\n";
    else if (chatMode === "error")
      stream += event({ error: { message: "fixture stream failure" } });
    const bytes = Buffer.from(stream);
    // Deliberately cut CRLF framing and multibyte Chinese UTF-8 across network writes.
    let offset = 0;
    const t = setInterval(() => {
      if (offset >= bytes.length) {
        clearInterval(t);
        res.end();
      } else {
        res.write(bytes.subarray(offset, offset + 7));
        offset += 7;
      }
    }, 2);
    res.on("close", () => clearInterval(t));
    return;
  }
  json({ error: { message: "unknown fixture route" } }, 404);
});
const chatBodies = [],
  results = [],
  failures = [],
  errors = [],
  external = [];
let browser;
async function run(name, fn) {
  try {
    await fn();
    results.push(name);
    console.log("PASS " + name);
  } catch (error) {
    failures.push({ name, error: String(error) });
    console.error("FAIL " + name + ": " + error);
  }
}
async function main() {
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const base = "http://127.0.0.1:" + server.address().port;
  browser = await chromium.launch({
    headless: true,
    ...(process.env.BROWSER_EXECUTABLE
      ? { executablePath: process.env.BROWSER_EXECUTABLE }
      : {}),
  });
  const context = await browser.newContext({
    viewport: { width: 1600, height: 1080 },
    deviceScaleFactor: 1,
    timezoneId: "Asia/Shanghai",
  });
  const page = await context.newPage();
  page.on("pageerror", (e) => errors.push(e.message));
  page.on("request", (r) => {
    if (!r.url().startsWith(base)) external.push(r.url());
  });
  const go = async (name) => {
    await page.locator('[data-page="' + name + '"]').click();
    await page
      .locator('[data-view="' + name + '"]')
      .waitFor({ state: "visible" });
  };
  const text = (selector) => page.locator(selector).innerText();
  const waitText = (id, value) =>
    page.waitForFunction(
      ([id, value]) => document.getElementById(id).textContent.includes(value),
      [id, value],
    );
  await page.goto(base);
  await run(
    "real fixture data, accurate status and recent request scope",
    async () => {
      await waitText("s-requests", "16");
      assert.equal(await text("#s-prefill"), "1,141.8");
      assert.equal(await text("#s-cache"), "22.7%");
      assert.match(await text("#h-status"), /API 已连接/);
      assert.equal(await page.locator("#recent-table tbody tr").count(), 5);
      assert.equal(memoryCalls, 0);
      assert.equal(reads, 0);
      await page.screenshot({
        path: path.join(output, "overview-desktop.png"),
        fullPage: true,
      });
    },
  );
  await run(
    "UTC filters, all finish reasons, accessible details and focus return",
    async () => {
      await go("requests");
      assert.equal(await page.locator("#all-table tbody tr").count(), 13);
      await page.selectOption("#reason-filter", "error");
      assert.equal(await page.locator("#all-table tbody tr").count(), 3);
      const detail = page.locator("#all-table .detail-btn").first();
      await detail.click();
      await page.locator("#request-detail").waitFor({ state: "visible" });
      assert.match(await text("#detail-body"), /时间（UTC）/);
      await page.keyboard.press("Escape");
      await page.locator("#request-detail").waitFor({ state: "hidden" });
      // Query focus and the current row atomically: close/poll handlers may replace
      // the original button between a locator resolution and its evaluation.
      await page.waitForFunction(
        () =>
          document.activeElement ===
          document.querySelector("#all-table .detail-btn"),
        null,
        { timeout: 3000 },
      );
      await page.selectOption("#reason-filter", "all");
      await page.selectOption("#reqs-range", "0");
      await page.waitForFunction(
        () => document.querySelectorAll("#all-table tbody tr").length === 14,
      );
      await page.selectOption("#reqs-range", "1");
      await page.screenshot({
        path: path.join(output, "requests-desktop.png"),
        fullPage: true,
      });
    },
  );
  await run(
    "all ten override controls and unsaved changes survive navigation",
    async () => {
      await go("settings");
      await waitText("ovr-status", "已读取");
      assert.equal(await page.locator("#ovr-body tr").count(), 10);
      await page.locator("#ovr-row-temperature select").selectOption("force");
      await page.locator("#ovr-row-temperature input").fill("0.6");
      await go("overview");
      await go("settings");
      assert.equal(
        await page.locator("#ovr-row-temperature input").inputValue(),
        "0.6",
      );
      assert.equal(reads, 1);
      await page.screenshot({
        path: path.join(output, "settings-desktop.png"),
        fullPage: true,
      });
    },
  );
  await run(
    "override validation, authorization, persistence failure and recovery",
    async () => {
      await page.locator("#ovr-row-temperature input").fill("3");
      await page.click("#ovr-save");
      await waitText("ovr-err", "超出范围");
      assert.equal(posts.length, 0);
      await page.locator("#ovr-row-temperature input").fill("0.6");
      authRequired = true;
      await page.click("#ovr-save");
      await waitText("ovr-err", "需要管理密钥");
      await page.fill("#ovr-key", "fixture-key");
      persistFails = true;
      await page.click("#ovr-save");
      await waitText("ovr-err", "持久化失败");
      assert.deepEqual(saved, { temperature: { mode: "force", value: 0.6 } });
      persistFails = false;
      await page.click("#ovr-save");
      await waitText("ovr-status", "已保存并生效");
      assert.equal(
        await page.evaluate(() => localStorage.length + sessionStorage.length),
        0,
      );
      page.once("dialog", (d) => d.accept());
      await page.click("#ovr-clear");
      await waitText("ovr-count", "跟随客户端");
      assert.deepEqual(saved, {});
    },
  );
  await run(
    "streamed Chinese, CRLF frames, escaped output and multi-turn continuity",
    async () => {
      await go("chat");
      await page.selectOption("#c-think", "false");
      await page.fill("#c-max", "64");
      await page.fill("#c-input", "你好");
      await page.click("#c-send");
      await waitText("chat-status", "完成");
      assert.match(await text("#c-log"), /你好，世界/);
      assert.match(await text("#c-log"), /服务端覆盖：temperature/);
      assert.equal(await page.locator("#c-log img").count(), 0);
      await page.fill("#c-input", "继续");
      await page.click("#c-send");
      await waitText("chat-status", "完成");
      assert.equal(chatBodies.at(-1).messages.length, 3);
      assert.equal(chatBodies.at(-1).max_tokens, 64);
      await page.screenshot({
        path: path.join(output, "chat-desktop.png"),
        fullPage: true,
      });
    },
  );
  await run(
    "truncated and error streams preserve partial text but do not poison context",
    async () => {
      await page.click("#c-clear");
      chatMode = "truncated";
      await page.fill("#c-input", "不完整请求");
      await page.click("#c-send");
      await waitText("chat-err", "未收到结束标记");
      assert.equal(await page.locator("#c-input").inputValue(), "不完整请求");
      chatMode = "error";
      await page.click("#c-send");
      await waitText("chat-err", "fixture stream failure");
      assert.equal(chatBodies.at(-1).messages.length, 1);
      chatMode = "normal";
      await page.click("#c-send");
      await waitText("chat-status", "完成");
      assert.equal(chatBodies.at(-1).messages.length, 1);
    },
  );
  await run(
    "cancel survives navigation and a later request succeeds",
    async () => {
      await page.click("#c-clear");
      chatMode = "slow";
      await page.fill("#c-input", "取消测试");
      await page.click("#c-send");
      await waitText("c-log", "正在生成");
      await go("overview");
      await go("chat");
      await page.click("#c-stop");
      await waitText("chat-status", "已停止");
      chatMode = "normal";
      await page.click("#c-send");
      await waitText("chat-status", "完成");
      assert.equal(chatBodies.at(-1).messages.length, 1);
    },
  );
  await run("memory samples only on its visible idle page", async () => {
    assert.equal(memoryCalls, 0);
    apiBusy = true;
    await go("overview");
    await waitText("h-queued", "1");
    await go("memory");
    await waitText("mem-meta", "等待 API 空闲");
    assert.equal(memoryCalls, 0);
    apiBusy = false;
    await go("overview");
    await waitText("h-queued", "0");
    await go("memory");
    await waitText("m-hero", "80.00");
    assert.equal(memoryCalls, 1);
    await page.screenshot({
      path: path.join(output, "memory-desktop.png"),
      fullPage: true,
    });
    await go("overview");
  });
  await run(
    "pause blocks polling and resume refreshes, failures retain visibly stale data",
    async () => {
      await page.waitForTimeout(300);
      await page.click("#pause");
      const counts = [
        healthRequests,
        summaryRequests,
        tailRequests,
        memoryCalls,
      ];
      await page.waitForTimeout(10500);
      assert.deepEqual(
        [healthRequests, summaryRequests, tailRequests, memoryCalls],
        counts,
      );
      apiError = true;
      statsError = true;
      await page.click("#pause");
      await waitText("health-err", "fixture disconnected");
      await waitText("reqs-err", "fixture statistics failure");
      assert.equal(await text("#s-requests"), "16");
      assert.equal(
        await page
          .locator("#reqs-grid")
          .evaluate((el) => el.classList.contains("stale")),
        true,
      );
      apiError = false;
      statsError = false;
      await page.click("#pause");
      await page.click("#pause");
      await page.waitForFunction(
        () => !document.getElementById("reqs-err").classList.contains("show"),
      );
      assert.equal(
        await page
          .locator("#reqs-grid")
          .evaluate((el) => el.classList.contains("stale")),
        false,
      );
    },
  );
  await run(
    "chart modes and keyboard inspection expose individual measurements",
    async () => {
      await go("overview");
      assert.equal(await page.locator("#chart-view").inputValue(), "request");
      await page.locator("#throughput").focus();
      await page.keyboard.press("End");
      assert.match(await text("#chart-readout"), /UTC.*Prefill.*tok\/s/);
      await page.selectOption("#chart-view", "time");
      assert.match(await text("#chart-axis-label"), /时间 UTC/);
      await page.click('[data-chart="decode"]');
      await page.locator("#throughput").focus();
      await page.keyboard.press("Home");
      assert.match(await text("#chart-readout"), /Decode.*tok\/s/);
      await page.click('[data-chart="prefill"]');
      await page.selectOption("#chart-view", "request");
      await page.mouse.move(0, 0);
      await page.locator("#content").focus();
    },
  );
  await run(
    "explicit date selection still works when automatic refresh is paused",
    async () => {
      await page.click("#pause");
      const count = summaryRequests;
      await page.selectOption("#reqs-range", "0");
      await page.waitForFunction(
        () => !document.getElementById("reqs-grid").classList.contains("stale"),
      );
      assert.equal(summaryRequests, count + 1);
      assert.equal(
        await page.locator("#pause").getAttribute("aria-pressed"),
        "true",
      );
      await page.selectOption("#reqs-range", "1");
      await page.waitForFunction(
        () => !document.getElementById("reqs-grid").classList.contains("stale"),
      );
      await page.click("#pause");
    },
  );
  await run(
    "desktop, tablet, mobile and narrow mobile layouts have no page overflow",
    async () => {
      for (const width of [1920, 1280, 768, 390, 320]) {
        await page.setViewportSize({ width, height: 980 });
        for (const name of [
          "overview",
          "requests",
          "settings",
          "chat",
          "memory",
        ]) {
          await go(name);
          assert.equal(
            await page.evaluate(
              () => document.documentElement.scrollWidth <= innerWidth + 1,
            ),
            true,
            width + " " + name + " overflow",
          );
          assert.equal(await page.locator('[aria-current="page"]').count(), 1);
          if (width <= 390) {
            assert.equal(
              await page
                .locator(".nav a")
                .last()
                .evaluate(
                  (el) => el.getBoundingClientRect().right <= innerWidth,
                ),
              true,
              "all five mobile destinations visible",
            );
            if (name === "settings") {
              assert.equal(
                await page
                  .locator(".ovr")
                  .evaluate((el) => el.scrollWidth <= el.clientWidth + 1),
                true,
                "mobile form needs no horizontal scroll",
              );
              for (const control of await page
                .locator("#ovr-body input, #ovr-body select")
                .all()) {
                assert.equal(
                  await control.evaluate((el) => {
                    const r = el.getBoundingClientRect();
                    return (
                      r.left >= 0 && r.right <= innerWidth && r.height >= 44
                    );
                  }),
                  true,
                  "mobile parameter control is visible and touch sized",
                );
              }
            }
            if (name === "requests" || name === "overview") {
              const table = page.locator(
                name === "overview"
                  ? "#recent-table .requests"
                  : "#all-table .requests",
              );
              assert.equal(
                await table.evaluate(
                  (el) => el.scrollWidth <= el.clientWidth + 1,
                ),
                true,
                "mobile records need no horizontal scroll",
              );
            }
          }
          if (width === 390)
            await page.screenshot({
              path: path.join(output, name + "-mobile.png"),
              fullPage: true,
            });
        }
      }
    },
  );
  await run("no script errors or external dependencies", async () => {
    assert.deepEqual(errors, []);
    assert.deepEqual(external, []);
  });
  fs.writeFileSync(
    path.join(output, "results.json"),
    JSON.stringify(
      {
        passed: results,
        failures,
        errors,
        externalRequests: external,
        viewportWidths: [1920, 1600, 1280, 768, 390, 320],
        memoryCalls,
        writesOnlyToFixture: true,
      },
      null,
      2,
    ),
  );
  if (failures.length)
    throw new Error(failures.length + " browser regression groups failed");
}
main()
  .catch((e) => {
    console.error(e);
    process.exitCode = 1;
  })
  .finally(async () => {
    if (browser) await browser.close();
    server.closeAllConnections();
    server.close();
  });
