// EspFS 儀表板前端：接收 /events（SSE），用原生 canvas 繪圖。不使用任何外部函式庫。
// 只會呼叫 /api/command 送 dump 或 erase；沒有任何馬達控制功能。
"use strict";

const CENTER = 1500;
const DEADBAND = 40;           // 韌體 kDeadband
const SWA_ON_ABOVE = 1500;     // 使用者規格：CH7 > 1500 視為開（韌體解鎖門檻為 kArmOnThreshold=1700）
const SPEED_LIMIT = 60;        // kSpeedLimitRpm
const OVERSPEED = 72;          // 60 × kOverspeedFactor 1.2
const RPM_SCALE = 80;
const WINDOW_MS = 60000;
const STALE_DRIVE_MS = 2000;
const STALE_LINE_S = 3;
const MAX_EVENTS = 500;
const MAX_RAW = 200;

const $ = (id) => document.getElementById(id);
const COLORS = { L: "#58a6ff", R: "#f0883e", ch2: "#3fb950", ch4: "#bc8cff", grid: "#2c333f", dim: "#8a93a3",
                 limit: "#8a93a3", over: "#f85149", dead: "rgba(138,147,163,0.22)", fg: "#e3e7ee" };

let state = null;
let samples = [];
let epoch = null;
let events = [];
let raw = [];
let dirty = true;

// ---------- 小工具 ----------
function fmtUptime(ms) {
  if (ms == null) return "—";
  const s = ms / 1000, h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = (s % 60).toFixed(1);
  return `${h}:${String(m).padStart(2, "0")}:${sec.padStart(4, "0")}（${ms} ms）`;
}
function fmtNum(v, digits = 1) { return v == null ? "—" : Number(v).toFixed(digits); }
function hex4(v) { return "0x" + (Math.round(v) >>> 0).toString(16).toUpperCase().padStart(4, "0"); }
function setText(id, text) { const el = $(id); if (el.textContent !== text) el.textContent = text; }

// 畫布寬度跟著版面走，高度固定；依 devicePixelRatio 調整解析度。
for (const id of ["chartRpm", "chartRc", "wheels", "stick"]) {
  const c = $(id); c.dataset.h = c.height; c.style.width = "100%"; c.style.height = c.height + "px";
}
function canvasCtx(id) {
  const c = $(id); const h = Number(c.dataset.h);
  const dpr = window.devicePixelRatio || 1, w = c.clientWidth;
  if (c.width !== Math.round(w * dpr) || c.height !== Math.round(h * dpr)) {
    c.width = Math.round(w * dpr); c.height = Math.round(h * dpr);
  }
  const ctx = c.getContext("2d"); ctx.setTransform(dpr, 0, 0, dpr, 0, 0); ctx.clearRect(0, 0, w, h);
  return { ctx, w, h };
}

// ---------- 資料接收 ----------
function onPayload(p) {
  state = p.state;
  if (state.chart_epoch !== epoch) { epoch = state.chart_epoch; samples = []; }
  for (const s of p.samples) {
    if (s.epoch !== epoch) continue;
    samples.push(s);
  }
  if (samples.length) {
    const cutoff = samples[samples.length - 1].ms - WINDOW_MS;
    let i = 0; while (i < samples.length && samples[i].ms < cutoff) i++;
    if (i) samples = samples.slice(i);
  }
  if (p.events.length) { events = events.concat(p.events).slice(-MAX_EVENTS); renderEvents(); }
  if (p.raw.length) { raw = raw.concat(p.raw).slice(-MAX_RAW); if ($("rawBox").open) renderRaw(); }
  dirty = true;
}

function connect() {
  const es = new EventSource("/events");
  es.onmessage = (ev) => { try { onPayload(JSON.parse(ev.data)); } catch (e) { console.error(e); } };
  es.onerror = () => {
    setText("conn", "儀表板程式已停止？");
    $("conn").className = "pill error";
  };
}

// ---------- 文字面板 ----------
function renderTop() {
  const c = state.connection;
  const labels = { connected: "已連線", disconnected: "已斷線", waiting: "等待接上", busy: "埠被占用",
                   error: "開啟失敗", replay: "重播中", replay_done: "重播完畢", starting: "啟動中" };
  setText("conn", labels[c.status] || c.status);
  $("conn").className = "pill " + c.status;
  setText("port", c.port || "—");
  setText("uptime", fmtUptime(state.uptime_ms));
  setText("boots", String(state.boot_count));
  setText("logfile", state.log_file ? state.log_file.split("/").pop() : "（未記錄）");
  $("logfile").title = state.log_file ? `Logging an output to ${state.log_file}` : "";
  setText("loglines", String(state.log_lines));
  $("dry").hidden = !state.dry;
  $("dumpbadge").hidden = !state.dumping;
  document.body.classList.toggle("dumping", state.dumping);

  const showBusy = c.status === "busy" || c.status === "error" || (c.status === "connected" && c.message);
  $("busy").hidden = !showBusy;
  if (showBusy) setText("busy", c.message);
  const quiet = c.status === "connected" && state.last_line_age_s != null && state.last_line_age_s > STALE_LINE_S;
  $("stale").hidden = !quiet;
  if (quiet) setText("stale", `已 ${state.last_line_age_s.toFixed(0)} 秒沒有收到任何資料`);

  $("latched").hidden = !state.latched;
  setText("latchedReason", state.latched_reason || "—");
  setText("latchedDetail", state.latched_detail || "—");
  $("probeHint").hidden = state.latched_reason !== "PROBE_FAIL";
  $("dumping").hidden = !state.dumping;
  setText("dumpLines", String(state.dump_lines));
  setText("dumpFile", state.dump_file ? state.dump_file.split("/").pop() : "（未記錄）");
}

function renderStatus() {
  setText("sysState", state.sys_state || "—");
  $("sysState").className = "state-big " + (state.sys_state || "");
  setText("reason", state.reason || "—");
  const rx = state.rx;
  setText("rxStatus", rx.status || "—");
  $("rxStatus").className = "tag " + (rx.status === "RECEIVING" ? "ok" : rx.status ? "bad" : "");
  setText("frameAge", rx.frame_age_ms == null ? "—" : String(rx.frame_age_ms));
  setText("unchanged", rx.values_unchanged_ms == null ? "—" : String(rx.values_unchanged_ms));
  setText("ch2", rx.ch2 == null ? "—" : String(rx.ch2));
  setText("ch4", rx.ch4 == null ? "—" : String(rx.ch4));
  setText("ch7", rx.ch7 == null ? "—" : String(rx.ch7));
  const on = rx.ch7 != null && rx.ch7 > SWA_ON_ABOVE;
  setText("swa", rx.ch7 == null ? "—" : on ? "開" : "關");
  $("swa").className = "tag " + (rx.ch7 == null ? "" : on ? "on" : "");
  $("btnDump").disabled = !state.commands_enabled;
  $("btnErase").disabled = !state.commands_enabled;
  setText("cmdReason", state.commands_enabled ? "可用（板子狀態 DISARMED／LATCHED）" : `不可用：${state.command_block_reason}`);
}

function driveCell(id, key, kind) {
  const el = $(id), d = state.drive[key];
  if (!d) { el.textContent = "—"; el.className = ""; return; }
  const stale = state.uptime_ms != null && state.uptime_ms - d.ms > STALE_DRIVE_MS;
  let text, cls = stale ? "stale" : "";
  if (kind === "err") { text = d.value ? hex4(d.value) : "0"; if (d.value) cls = "err"; }
  else if (kind === "hex") text = hex4(d.value);
  else text = fmtNum(d.value);
  el.textContent = text + (stale ? "（舊）" : "");
  el.className = cls;
}

function renderWheelTable() {
  driveCell("actL", "actual_velocity_L"); driveCell("actR", "actual_velocity_R");
  driveCell("torqueL", "actual_torque_L"); driveCell("torqueR", "actual_torque_R");
  driveCell("errL", "error_code_L", "err"); driveCell("errR", "error_code_R", "err");
  driveCell("msL", "motor_state_L", "hex"); driveCell("msR", "motor_state_R", "hex");
  setText("tgtL", fmtNum(state.control.tgtL)); setText("tgtR", fmtNum(state.control.tgtR));
}

function renderEvents() {
  const showDump = $("showDumpEvents").checked;
  const ul = $("events"); const frag = document.createDocumentFragment();
  for (let i = events.length - 1; i >= 0; i--) {
    const e = events[i];
    if (e.dump && !showDump) continue;
    const li = document.createElement("li");
    li.className = e.level + (e.dump ? " dump" : "");
    const t = document.createElement("span"); t.className = "t";
    t.textContent = `${e.host}  ${e.ms == null ? "" : (e.ms / 1000).toFixed(1) + "s"}`;
    li.appendChild(t);
    if (e.dump) { const d = document.createElement("span"); d.className = "dtag"; d.textContent = "傾倒"; li.appendChild(d); }
    li.appendChild(document.createTextNode(e.text));
    frag.appendChild(li);
  }
  ul.replaceChildren(frag);
}

function renderRaw() {
  const pre = $("raw"); const frag = document.createDocumentFragment();
  for (const r of raw) {
    const span = document.createElement("span");
    if (r.dump) span.className = "dump";
    span.textContent = r.text + "\n";
    frag.appendChild(span);
  }
  pre.replaceChildren(frag);
  pre.scrollTop = pre.scrollHeight;
}

// ---------- 畫布 ----------
function drawStick() {
  const { ctx, w, h } = canvasCtx("stick");
  const size = Math.min(w, h) - 20, x0 = (w - size) / 2, y0 = (h - size) / 2;
  const toX = (v) => x0 + ((v - 1000) / 1000) * size;
  const toY = (v) => y0 + size - ((v - 1000) / 1000) * size;
  ctx.fillStyle = "#0c0f13"; ctx.fillRect(x0, y0, size, size);
  ctx.fillStyle = COLORS.dead;
  ctx.fillRect(toX(CENTER - DEADBAND), y0, toX(CENTER + DEADBAND) - toX(CENTER - DEADBAND), size);
  ctx.fillRect(x0, toY(CENTER + DEADBAND), size, toY(CENTER - DEADBAND) - toY(CENTER + DEADBAND));
  ctx.strokeStyle = COLORS.dim; ctx.setLineDash([4, 4]); ctx.beginPath();
  ctx.moveTo(toX(CENTER), y0); ctx.lineTo(toX(CENTER), y0 + size);
  ctx.moveTo(x0, toY(CENTER)); ctx.lineTo(x0 + size, toY(CENTER)); ctx.stroke(); ctx.setLineDash([]);
  ctx.strokeStyle = COLORS.grid; ctx.strokeRect(x0, y0, size, size);
  ctx.fillStyle = COLORS.dim; ctx.font = "11px sans-serif";
  ctx.fillText("油門 CH2 ↑", x0 + 4, y0 + 12); ctx.fillText("轉向 CH4 →", x0 + size - 66, y0 + size - 4);
  const rx = state.rx;
  if (rx.ch2 == null) return;
  const sx = toX(rx.ch4 == null ? CENTER : Math.min(2000, Math.max(1000, rx.ch4)));
  const sy = toY(Math.min(2000, Math.max(1000, rx.ch2)));
  ctx.fillStyle = rx.status === "RECEIVING" ? COLORS.ch2 : COLORS.over;
  ctx.beginPath(); ctx.arc(sx, sy, 8, 0, Math.PI * 2); ctx.fill();
}

function drawWheels() {
  const { ctx, w, h } = canvasCtx("wheels");
  const top = 14, bottom = h - 22, mid = (top + bottom) / 2, half = (bottom - top) / 2;
  const toY = (v) => mid - (Math.max(-RPM_SCALE, Math.min(RPM_SCALE, v)) / RPM_SCALE) * half;
  const cols = [{ key: "L", x: w * 0.3 }, { key: "R", x: w * 0.7 }];
  const barW = Math.min(60, w * 0.18);
  ctx.font = "11px sans-serif"; ctx.textAlign = "right"; ctx.fillStyle = COLORS.dim;
  for (const v of [-80, -60, -40, -20, 0, 20, 40, 60, 80]) ctx.fillText(String(v), 26, toY(v) + 4);
  for (const { key, x } of cols) {
    const left = x - barW / 2;
    ctx.fillStyle = "#0c0f13"; ctx.fillRect(left, top, barW, bottom - top);
    ctx.strokeStyle = COLORS.grid; ctx.beginPath(); ctx.moveTo(left, mid); ctx.lineTo(left + barW, mid); ctx.stroke();
    ctx.strokeStyle = COLORS.limit; ctx.setLineDash([4, 3]); ctx.beginPath();
    for (const v of [SPEED_LIMIT, -SPEED_LIMIT]) { ctx.moveTo(left - 6, toY(v)); ctx.lineTo(left + barW + 6, toY(v)); }
    ctx.stroke(); ctx.setLineDash([]);
    ctx.strokeStyle = COLORS.over; ctx.lineWidth = 2; ctx.beginPath();
    for (const v of [OVERSPEED, -OVERSPEED]) { ctx.moveTo(left - 8, toY(v)); ctx.lineTo(left + barW + 8, toY(v)); }
    ctx.stroke(); ctx.lineWidth = 1;
    const act = state.drive["actual_velocity_" + key];
    if (act) {
      const v = act.value, a = Math.abs(v);
      const stale = state.uptime_ms != null && state.uptime_ms - act.ms > STALE_DRIVE_MS;
      ctx.fillStyle = stale ? COLORS.dim : a > OVERSPEED ? COLORS.over : a > SPEED_LIMIT ? "#d29922" : COLORS[key];
      const y = toY(v); ctx.fillRect(left + 6, Math.min(y, mid), barW - 12, Math.abs(mid - y));
    }
    const tgt = state.control["tgt" + key];
    if (tgt != null) {
      const y = toY(tgt); ctx.fillStyle = COLORS.fg; ctx.beginPath();
      ctx.moveTo(left + barW + 2, y); ctx.lineTo(left + barW + 14, y - 7); ctx.lineTo(left + barW + 14, y + 7); ctx.fill();
    }
    ctx.textAlign = "center"; ctx.fillStyle = COLORS.fg; ctx.font = "13px sans-serif";
    ctx.fillText(key === "L" ? "左 L" : "右 R", x, h - 6);
    ctx.font = "11px sans-serif"; ctx.textAlign = "right"; ctx.fillStyle = COLORS.dim;
  }
}

function drawChart(id, series, yMin, yMax, guides, band) {
  const { ctx, w, h } = canvasCtx(id);
  const padL = 40, padR = 8, padT = 8, padB = 18, pw = w - padL - padR, ph = h - padT - padB;
  const end = samples.length ? samples[samples.length - 1].ms : 0, start = end - WINDOW_MS;
  const toX = (ms) => padL + ((ms - start) / WINDOW_MS) * pw;
  const toY = (v) => padT + ph - ((Math.max(yMin, Math.min(yMax, v)) - yMin) / (yMax - yMin)) * ph;
  ctx.fillStyle = "#0c0f13"; ctx.fillRect(padL, padT, pw, ph);
  if (band) { ctx.fillStyle = COLORS.dead; ctx.fillRect(padL, toY(band[1]), pw, toY(band[0]) - toY(band[1])); }
  ctx.font = "11px sans-serif"; ctx.textAlign = "right";
  for (const g of guides) {
    ctx.strokeStyle = g.color; ctx.setLineDash(g.dash || []); ctx.beginPath();
    ctx.moveTo(padL, toY(g.v)); ctx.lineTo(padL + pw, toY(g.v)); ctx.stroke(); ctx.setLineDash([]);
    ctx.fillStyle = COLORS.dim; ctx.fillText(String(g.v), padL - 4, toY(g.v) + 4);
  }
  ctx.textAlign = "center"; ctx.fillStyle = COLORS.dim;
  for (let s = 0; s <= 60; s += 10) ctx.fillText(`-${60 - s}s`, padL + (s / 60) * pw, h - 4);
  ctx.save(); ctx.beginPath(); ctx.rect(padL, padT, pw, ph); ctx.clip();
  for (const sr of series) {
    ctx.strokeStyle = sr.color; ctx.lineWidth = sr.width || 1.5; ctx.setLineDash(sr.dash || []);
    ctx.beginPath(); let pen = false;
    for (const s of samples) {
      const v = s[sr.key];
      if (v == null) { pen = false; continue; }
      const x = toX(s.ms), y = toY(v);
      if (pen) ctx.lineTo(x, y); else { ctx.moveTo(x, y); pen = true; }
    }
    ctx.stroke();
  }
  ctx.restore(); ctx.setLineDash([]); ctx.lineWidth = 1;
}

function drawCharts() {
  const over = { color: COLORS.over }, lim = { color: COLORS.limit, dash: [4, 3] };
  drawChart("chartRpm", [
    { key: "tgtL", color: COLORS.L, dash: [6, 4] }, { key: "tgtR", color: COLORS.R, dash: [6, 4] },
    { key: "actL", color: COLORS.L, width: 2 }, { key: "actR", color: COLORS.R, width: 2 },
  ], -RPM_SCALE, RPM_SCALE, [
    { v: OVERSPEED, ...over }, { v: -OVERSPEED, ...over }, { v: SPEED_LIMIT, ...lim }, { v: -SPEED_LIMIT, ...lim },
    { v: 0, color: COLORS.grid },
  ]);
  drawChart("chartRc", [{ key: "ch2", color: COLORS.ch2, width: 2 }, { key: "ch4", color: COLORS.ch4, width: 2 }],
    1000, 2000, [{ v: 2000, color: COLORS.grid }, { v: CENTER, color: COLORS.dim, dash: [4, 4] }, { v: 1000, color: COLORS.grid }],
    [CENTER - DEADBAND, CENTER + DEADBAND]);
}

function frame() {
  if (dirty && state) {
    dirty = false;
    renderTop(); renderStatus(); renderWheelTable();
    drawStick(); drawWheels(); drawCharts();
  }
  requestAnimationFrame(frame);
}

// ---------- 指令（只有 dump／erase） ----------
async function sendCommand(cmd, confirmToken) {
  setText("cmdResult", `送出 ${cmd}…`);
  try {
    const res = await fetch("/api/command", {
      method: "POST",
      headers: { "Content-Type": "application/json", "X-EspFS-Dashboard": "1" },
      body: JSON.stringify(confirmToken ? { cmd, confirm: confirmToken } : { cmd }),
    });
    const body = await res.json();
    setText("cmdResult", `${cmd}：${body.message}`);
  } catch (e) {
    setText("cmdResult", `${cmd}：送出失敗 ${e}`);
  }
}
$("btnDump").addEventListener("click", () => sendCommand("dump"));
$("btnErase").addEventListener("click", () => {
  if (!confirm("確定要清除板上記錄（erase）？清除後無法復原。")) return;
  const typed = prompt("第二次確認：請輸入 ERASE（大寫）");
  if (typed !== "ERASE") { setText("cmdResult", "erase 已取消"); return; }
  sendCommand("erase", "ERASE");
});
$("showDumpEvents").addEventListener("change", renderEvents);
$("rawBox").addEventListener("toggle", () => { if ($("rawBox").open) renderRaw(); });
window.addEventListener("resize", () => { dirty = true; });

connect();
requestAnimationFrame(frame);
