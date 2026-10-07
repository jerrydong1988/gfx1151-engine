// Embedded, dependency-free dashboard. All endpoints are same-origin.
// /health is API self-state, not an engine liveness check. /memory occupies
// an inference slot and is only polled on the visible memory page when idle.
// Request dates are UTC. Charts use real per-request records, not synthetic data.
static const char kDashboardHtml[] = R"HTMLDOC(<!doctype html>
<html lang="zh-CN">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>gfx1151 控制台</title>
    <style>
      :root {
        color-scheme: dark;
        --text-xs: 0.75rem;
        --text-sm: 0.8125rem;
        --text-base: 0.875rem;
        --control-height: 40px;
        --hover: #22283a;
        --bg: #0b0e16;
        --sidebar: #0d101a;
        --panel: #151a27;
        --soft: #0e1320;
        --border: #30384d;
        --fg: #f1f4fc;
        --dim: #adb8ce;
        --accent: #d3fa69;
        --accent-bg: #2b351d;
        --ok: #b9ec87;
        --warn: #ffcb85;
        --err: #ffa9a6;
        --c-committed: #d3fa69;
        --c-rss: #b8a4ff;
        --secondary: #b8a4ff;
        --display-font: "Bahnschrift", "Segoe UI Variable", "Segoe UI", sans-serif;
      }
      * {
        box-sizing: border-box;
      }
      body {
        margin: 0;
        background: var(--bg);
        color: var(--fg);
        font:
          14px/1.6 "Segoe UI Variable",
          "Segoe UI",
          "Microsoft YaHei",
          sans-serif;
      }
      button,
      input,
      select,
      textarea {
        font: inherit;
      }
      button,
      a {
        touch-action: manipulation;
      }
      button {
        cursor: pointer;
      }
      button:disabled {
        opacity: 0.64;
        cursor: default;
      }
      a {
        color: var(--accent);
        text-decoration: none;
      }
      button,
      input,
      select,
      textarea {
        outline-offset: 4px;
      }
      button:focus-visible,
      a:focus-visible,
      input:focus-visible,
      select:focus-visible,
      textarea:focus-visible,
      summary:focus-visible {
        outline: 2px solid var(--accent);
      }
      [hidden] {
        display: none !important;
      }
      h1,
      h2,
      h3,
      p {
        margin: 0;
      }
      h1 {
        font-size: 36px;
        font-weight: 700;
        letter-spacing: -1px;
        line-height: 1.4;
      }
      h2 {
        font-size: 15px;
        font-weight: 600;
      }
      h3 {
        font-size: 14px;
      }
      small,
      .muted,
      .hint,
      .note {
        color: var(--dim);
      }
      .hint,
      .note {
        font-size: var(--text-sm);
      }
      .mono {
        font-family: Consolas, "Cascadia Mono", monospace;
        font-variant-numeric: tabular-nums;
      }
      .row,
      .between {
        display: flex;
        align-items: center;
        gap: 12px;
        flex-wrap: wrap;
      }
      .between {
        justify-content: space-between;
      }
      .btn {
        display: inline-flex;
        gap: 8px;
        align-items: center;
        justify-content: center;
        min-height: var(--control-height);
        padding: 7px 13px;
        border: 1px solid var(--border);
        border-radius: 8px;
        background: transparent;
        color: var(--fg);
      }
      .btn:hover:not(:disabled) {
        background: #242d42;
        border-color: #7789ac;
      }
      .btn.primary {
        background: var(--accent);
        border-color: var(--accent);
        color: #19220c;
        font-weight: 600;
      }
      .btn.primary:hover:not(:disabled) {
        background: #e2ff96;
      }
      .link-btn {
        border: 0;
        background: none;
        color: var(--accent);
        padding: 6px;
        font: inherit;
      }
      .icon {
        width: 18px;
        height: 18px;
        fill: none;
        stroke: currentColor;
        stroke-width: 1.7;
        stroke-linecap: round;
        stroke-linejoin: round;
        flex: none;
      }
      .chip {
        display: inline-flex;
        align-items: center;
        border: 1px solid var(--border);
        border-radius: 6px;
        padding: 3px 8px;
        font-size: var(--text-xs);
        white-space: nowrap;
        color: var(--dim);
      }
      .chip.ok {
        background: #243521;
        color: var(--ok);
        border-color: #425b34;
      }
      .chip.warn {
        background: #382d21;
        color: var(--warn);
        border-color: #65503a;
      }
      .chip.err {
        background: #3d252e;
        color: var(--err);
        border-color: #82505c;
      }
      .dot {
        display: inline-block;
        background: var(--ok);
        width: 7px;
        height: 7px;
        border-radius: 50%;
        margin-right: 7px;
      }
      .sidebar {
        position: fixed;
        inset: 0 auto 0 0;
        width: 214px;
        display: flex;
        flex-direction: column;
        padding: 30px 16px 20px;
        background: var(--sidebar);
        border-right: 1px solid var(--border);
        z-index: 5;
      }
      .brand {
        display: flex;
        gap: 11px;
        align-items: center;
        padding: 0 6px 30px;
      }
      .brand-mark {
        width: 38px;
        height: 38px;
        border: 1px solid #d3fa69;
        background: var(--accent);
        color: #19220c;
        border-radius: 9px;
        display: grid;
        place-items: center;
      }
      .brand strong {
        font-family: var(--display-font);
        font-size: 23px;
        font-weight: 600;
        letter-spacing: -0.6px;
        color: var(--fg);
      }
      .brand small {
        display: block;
        font-size: var(--text-xs);
        letter-spacing: 1px;
        white-space: nowrap;
      }
      .nav-caption {
        padding: 0 14px 12px;
        color: var(--dim);
        font-size: var(--text-xs);
      }
      .nav {
        display: flex;
        flex-direction: column;
        gap: 9px;
      }
      .nav a {
        display: flex;
        align-items: center;
        gap: 12px;
        padding: 11px 14px;
        color: var(--dim);
        border-radius: 8px;
        font-size: var(--text-base);
        white-space: nowrap;
      }
      .nav a:hover {
        background: #20273a;
        color: var(--fg);
      }
      .nav a[aria-current="page"] {
        color: #19220c;
        background: var(--accent);
        font-weight: 600;
      }
      .sidebar-bottom {
        margin-top: auto;
        padding: 17px 12px 0;
        border-top: 1px solid var(--border);
        font-size: var(--text-xs);
        color: var(--dim);
      }
      .sidebar-bottom .mono {
        display: block;
        margin-top: 8px;
        overflow-wrap: anywhere;
      }
      .shell {
        margin-left: 214px;
        min-width: 0;
      }
      .topbar {
        min-height: 68px;
        padding: 14px 32px;
        border-bottom: 1px solid var(--border);
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 14px;
      }
      .crumb {
        font-size: var(--text-sm);
        color: var(--dim);
      }
      .crumb b {
        font-weight: 500;
        color: var(--fg);
        margin-left: 10px;
      }
      .main {
        max-width: 1680px;
        margin: auto;
        padding: 32px 32px;
      }
      .section-title {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 16px;
        margin-bottom: 28px;
      }
      .section-title p {
        color: var(--dim);
        margin-top: 6px;
        font-size: var(--text-base);
      }
      .section-title .meta {
        font-size: var(--text-xs);
        color: var(--dim);
      }
      input,
      select,
      textarea {
        background: var(--soft);
        color: var(--fg);
        border: 1px solid var(--border);
        border-radius: 7px;
        padding: 8px 10px;
        min-width: 0;
      }
      select {
        min-height: var(--control-height);
      }
      textarea {
        width: 100%;
        resize: vertical;
      }
      input:disabled,
      select:disabled {
        opacity: 0.64;
      }
      .status-strip {
        border: 1px solid var(--border);
        background: var(--sidebar);
        border-radius: 8px;
        min-height: 46px;
        padding: 9px 16px;
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 16px;
        margin-bottom: 22px;
      }
      .status-strip .row {
        min-width: 0;
      }
      .status-strip .mono {
        overflow-wrap: anywhere;
        font-size: var(--text-base);
      }
      .status-strip small {
        font-size: var(--text-xs);
      }
      .sep {
        height: 17px;
        width: 1px;
        background: #3d465d;
      }
      .stats {
        display: grid;
        grid-template-columns: minmax(0, 1.4fr) repeat(2, minmax(0, 1fr));
        gap: 14px;
        margin: 0 0 24px;
      }
      .stat {
        background: var(--panel);
        border: 1px solid var(--border);
        border-radius: 14px;
        padding: 20px 22px;
        min-width: 0;
      }
      .stat.featured {
        grid-row: span 2;
        background: linear-gradient(130deg, #302951, #1c2038 75%);
        border-color: #685a95;
        display: flex;
        flex-direction: column;
        justify-content: center;
        padding: 28px;
        position: relative;
        overflow: hidden;
      }
      .stat.featured .label {
        color: #d2c6ff;
        position: relative;
      }
      .stat.featured .value {
        font-size: clamp(40px, 4.3vw, 64px);
        line-height: 1.1;
        margin: 24px 0 18px;
        position: relative;
      }
      .stat.featured .foot {
        color: #c0c7df;
        position: relative;
      }
      .stat.requests-stat {
        grid-column: 2 / -1;
        display: grid;
        grid-template-columns: 1fr auto;
        gap: 0 20px;
        align-items: center;
        padding-block: 14px;
        background: var(--soft);
      }
      .requests-stat .value {
        grid-column: 2;
        grid-row: 1 / 3;
        margin: 0;
      }
      .requests-stat .foot {
        grid-column: 1;
      }
      .metric-code {
        font: 12px/1.5 var(--display-font);
        letter-spacing: 1.4px;
        margin-bottom: 18px;
        color: #d2c6ff;
      }
      .signal-mark {
        position: absolute;
        right: -12px;
        top: -10px;
        width: 138px;
        height: 138px;
        opacity: 0.25;
        pointer-events: none;
      }
      .eyebrow {
        display: block;
        color: var(--accent);
        font: 12px/1.5 var(--display-font);
        letter-spacing: 1.8px;
        margin-bottom: 8px;
      }
      .stat .label {
        color: var(--dim);
        font-size: var(--text-sm);
        display: flex;
        justify-content: space-between;
        align-items: center;
      }
      .stat .value {
        font-family: var(--display-font);
        font-variant-numeric: tabular-nums;
        font-weight: 600;
        font-size: 36px;
        letter-spacing: -0.025em;
        margin: 9px 0;
        line-height: 1.25;
        overflow-wrap: anywhere;
      }
      .stat.featured .value {
        color: #d3fa69;
      }
      .stat .value small {
        font-size: var(--text-sm);
        font-weight: 400;
        letter-spacing: 0;
        margin-left: 6px;
      }
      .stat .foot {
        font-size: var(--text-xs);
        color: var(--dim);
      }
      .analytics,
      .bottom-grid {
        display: grid;
        grid-template-columns: minmax(0, 1fr) 320px;
        gap: 20px;
        margin-bottom: 20px;
      }
      .panel {
        background: var(--panel);
        border: 1px solid var(--border);
        border-radius: 14px;
        min-width: 0;
        overflow: hidden;
      }
      .panel-head {
        padding: 22px 22px 0;
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 12px;
        flex-wrap: wrap;
      }
      .panel-head p {
        color: var(--dim);
        font-size: var(--text-xs);
        margin-top: 3px;
      }
      .panel-body {
        padding: 20px 22px;
      }
      .chart-switch {
        display: flex;
        gap: 3px;
        background: var(--soft);
        border: 1px solid var(--border);
        padding: 3px;
        border-radius: 7px;
      }
      .chart-switch button {
        font-size: var(--text-xs);
        color: var(--dim);
        background: transparent;
        border: 0;
        border-radius: 5px;
        padding: 4px 10px;
      }
      .chart-switch button[aria-pressed="true"] {
        color: var(--accent);
        background: var(--accent-bg);
      }
      #throughput {
        display: block;
        width: 100%;
        height: 200px;
      }
      .chart-note {
        display: flex;
        justify-content: space-between;
        gap: 10px;
        color: var(--dim);
        font-size: var(--text-xs);
        margin-top: 12px;
      }
      .number {
        font-family: var(--display-font);
        font-variant-numeric: tabular-nums;
        font-size: 42px;
        line-height: 1.3;
      }
      .number small {
        font-family: "Segoe UI", sans-serif;
        font-size: var(--text-sm);
      }
      .capacity {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 10px;
        margin: 12px 0;
      }
      .capacity > div {
        padding: 0;
      }
      .capacity strong {
        font:
          25px var(--display-font);
        display: block;
        margin-top: 3px;
      }
      .capacity small {
        font-size: var(--text-xs);
      }
      .summary-line {
        display: flex;
        justify-content: space-between;
        align-items: baseline;
        gap: 14px;
        padding: 12px 0;
        border-bottom: 1px solid var(--border);
        font-size: var(--text-sm);
      }
      .summary-line > span {
        color: var(--dim);
      }
      .summary-line strong {
        font-weight: 500;
        text-align: right;
        overflow-wrap: anywhere;
      }
      .summary-line:last-child {
        border-bottom: 0;
      }
      .acceptance {
        padding-top: 17px;
        font-size: var(--text-sm);
      }
      .bar {
        height: 7px;
        border-radius: 5px;
        background: #30384d;
        overflow: hidden;
        margin: 10px 0;
      }
      .bar > i {
        display: block;
        height: 100%;
        background: var(--accent);
      }
      .table-wrap {
        overflow: auto;
        max-width: 100%;
      }
      table {
        border-collapse: collapse;
        width: 100%;
        font-size: var(--text-sm);
      }
      th {
        font-weight: 400;
        color: var(--dim);
        text-align: left;
        white-space: nowrap;
        font-size: var(--text-xs);
      }
      td,
      th {
        padding: 10px 16px;
        border-bottom: 1px solid var(--border);
      }
      tr:last-child td {
        border-bottom: 0;
      }
      td small {
        font-size: var(--text-xs);
      }
      .requests {
        min-width: 680px;
        margin-top: 14px;
      }
      .requests tbody tr:hover {
        background: #20273a;
      }
      .requests .r-id {
        display: block;
        color: var(--dim);
        font-size: var(--text-xs);
        margin-top: 3px;
      }
      .requests .detail-btn {
        padding: 4px 10px;
        min-height: var(--control-height);
        min-width: var(--control-height);
        border: 0;
        background: transparent;
        color: var(--dim);
        border-radius: 5px;
      }
      .requests .detail-btn:hover {
        color: var(--accent);
        background: var(--accent-bg);
      }
      .table-footer {
        border-top: 1px solid var(--border);
        padding: 12px 20px;
        color: var(--dim);
        font-size: var(--text-xs);
        display: flex;
        justify-content: space-between;
        gap: 10px;
        flex-wrap: wrap;
      }
      .empty {
        padding: 34px 20px;
        text-align: center;
        color: var(--dim);
        font-size: var(--text-base);
      }
      .page-footer {
        display: flex;
        justify-content: space-between;
        color: var(--dim);
        font-size: var(--text-xs);
        gap: 10px;
        flex-wrap: wrap;
        margin-top: 24px;
      }
      .errmsg {
        display: none;
        color: var(--err);
        background: #39232e;
        border: 1px solid #89505d;
        padding: 11px 15px;
        border-radius: 7px;
        font-size: var(--text-sm);
        overflow-wrap: anywhere;
        margin: 12px 0;
      }
      .errmsg.show {
        display: block;
      }
      .stale {
        opacity: 0.78;
      }
      .notice {
        padding: 12px 16px;
        border-radius: 8px;
        border: 1px solid var(--border);
        color: var(--dim);
        font-size: var(--text-sm);
        margin-bottom: 20px;
        background: var(--soft);
      }
      .notice strong {
        color: var(--fg);
        font-weight: 500;
      }
      .grid {
        display: grid;
        grid-template-columns: repeat(3, minmax(0, 1fr));
        gap: 14px;
      }
      .cell {
        background: var(--soft);
        border: 1px solid var(--border);
        border-radius: 8px;
        padding: 14px;
      }
      .cell .k,
      .cell .s {
        font-size: var(--text-xs);
        color: var(--dim);
      }
      .cell .v {
        font-size: 22px;
        line-height: 1.5;
      }
      .secondary-grid {
        display: grid;
        grid-template-columns: minmax(0, 1fr) 300px;
        gap: 20px;
      }
      .help p {
        margin-top: 17px;
        color: var(--dim);
        font-size: var(--text-sm);
      }
      .help strong {
        color: var(--fg);
        font-weight: 500;
      }
      .ovr {
        min-width: 690px;
      }
      .ovr td {
        vertical-align: middle;
      }
      .ovr td.note {
        max-width: 310px;
        min-width: 210px;
        font-size: var(--text-xs);
      }
      .ovr input,
      .ovr select {
        width: 130px;
      }
      .ovr .on td:first-child {
        color: var(--warn);
      }
      .ovr .def td:first-child {
        color: var(--accent);
      }
      .save-bar {
        padding: 18px 22px;
        border-top: 1px solid var(--border);
        background: var(--panel);
        display: flex;
        align-items: center;
        gap: 10px;
        flex-wrap: wrap;
      }
      .save-bar small {
        flex-basis: 100%;
      }
      #ovr-status,
      #chat-status {
        font-size: var(--text-sm);
        color: var(--dim);
      }
      #ovr-meta {
        overflow-wrap: anywhere;
      }
      .cparams {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 14px;
      }
      .cparams label {
        display: flex;
        flex-direction: column;
        gap: 5px;
        color: var(--dim);
        font-size: var(--text-xs);
      }
      .cparams input,
      .cparams select {
        width: 100%;
      }
      #c-log {
        display: flex;
        flex-direction: column;
        gap: 15px;
        max-height: 65vh;
        overflow: auto;
        scrollbar-gutter: stable;
      }
      .msg {
        background: var(--soft);
        border: 1px solid var(--border);
        padding: 16px;
        border-radius: 9px;
        min-width: 0;
      }
      .msg.user {
        border-color: #554774;
        background: #211f36;
      }
      .msg .role {
        font-size: var(--text-xs);
        color: var(--accent);
        margin-bottom: 6px;
      }
      .msg .body,
      .msg .think {
        white-space: pre-wrap;
        overflow-wrap: anywhere;
      }
      .msg .think {
        color: var(--dim);
        border-left: 1px solid #655584;
        padding-left: 12px;
        margin: 10px 0;
        font-size: var(--text-sm);
      }
      .msg .stat {
        font-size: var(--text-xs);
        color: var(--dim);
        margin-top: 10px;
      }
      .msg .ovr-note {
        font-size: var(--text-xs);
        color: var(--warn);
      }
      .chat-empty {
        min-height: 260px;
        display: flex;
        align-items: center;
        justify-content: center;
        flex-direction: column;
        text-align: center;
        gap: 10px;
        color: var(--dim);
      }
      .chat-empty .icon {
        width: 32px;
        height: 32px;
        color: var(--accent);
        margin-bottom: 8px;
      }
      .chat-empty p {
        font-size: var(--text-sm);
        max-width: 350px;
      }
      .chat-compose {
        padding-top: 20px;
      }
      .chat-compose textarea {
        min-height: 106px;
      }
      details {
        margin-top: 14px;
      }
      summary {
        cursor: pointer;
        color: var(--dim);
        font-size: var(--text-sm);
      }
      pre {
        font-size: var(--text-sm);
        background: var(--soft);
        padding: 16px;
        border: 1px solid var(--border);
        border-radius: 8px;
        overflow: auto;
        white-space: pre-wrap;
        overflow-wrap: anywhere;
        max-height: 420px;
      }
      #c-system {
        margin-top: 7px;
      }
      .hero {
        display: flex;
        align-items: baseline;
        gap: 20px;
        flex-wrap: wrap;
        margin: 0 0 22px;
      }
      .hero .num {
        font:
          48px var(--display-font);
        color: var(--accent);
      }
      .hero .lab {
        font-size: var(--text-sm);
        color: var(--dim);
      }
      .barlab,
      .legend {
        display: flex;
        gap: 15px;
        justify-content: space-between;
        font-size: var(--text-xs);
        color: var(--dim);
      }
      .legend {
        justify-content: flex-start;
        flex-wrap: wrap;
      }
      .legend i {
        display: inline-block;
        width: 8px;
        height: 8px;
        margin-right: 5px;
        border-radius: 3px;
      }
      #spark {
        width: 100%;
        height: 180px;
        display: block;
        margin: 22px 0 12px;
      }
      .memory-grid {
        margin-top: 24px;
      }
      .drawer {
        position: fixed;
        inset: 0 0 0 auto;
        width: min(480px, 100%);
        max-width: 100%;
        max-height: 100%;
        height: 100%;
        margin: 0;
        border: 0;
        border-left: 1px solid var(--border);
        padding: 28px;
        background: var(--panel);
        color: var(--fg);
        overflow: auto;
      }
      .drawer::backdrop {
        background: #0008;
      }
      .drawer h2 {
        font-size: 25px;
        margin: 23px 0 10px;
      }
      .drawer .summary-line {
        padding: 17px 0;
      }
      #toast {
        position: fixed;
        bottom: 24px;
        left: 50%;
        transform: translateX(-50%);
        max-width: 90vw;
        padding: 11px 20px;
        background: #293620;
        box-shadow: 0 8px 30px #0006;
        border-radius: 8px;
        z-index: 20;
        font-size: var(--text-base);
        overflow-wrap: anywhere;
      }
      .skip {
        position: fixed;
        top: -80px;
        left: 12px;
        padding: 10px;
        background: var(--panel);
        z-index: 99;
      }
      .skip:focus {
        top: 8px;
      }
      .analytics > *,
      .bottom-grid > *,
      .secondary-grid > * {
        min-width: 0;
      }
      @media (min-width: 1600px) {
        .analytics,
        .bottom-grid {
          grid-template-columns: minmax(0, 1fr) 360px;
        }
        .stat .value {
          font-size: 36px;
        }
      }
      @media (max-width: 1150px) {
        .sidebar {
          width: 74px;
          padding: 24px 10px;
        }
        .brand {
          padding: 0 8px 30px;
        }
        .brand > span,
        .nav-caption,
        .nav a span,
        .sidebar-bottom {
          display: none;
        }
        .nav a {
          justify-content: center;
          padding: 13px 0;
        }
        .shell {
          margin-left: 74px;
        }
        .main {
          padding: 24px;
        }
        .topbar {
          padding: 14px 24px;
        }
        .stats {
          gap: 12px;
        }
        .stat {
          padding: 16px;
        }
        .stat .value {
          font-size: 27px;
        }
        .analytics,
        .bottom-grid {
          grid-template-columns: minmax(0, 1fr) 290px;
        }
        .secondary-grid {
          grid-template-columns: minmax(0, 1fr) 270px;
        }
      }
      @media (max-width: 850px) {
        .analytics,
        .bottom-grid,
        .secondary-grid {
          grid-template-columns: 1fr;
        }
        .grid {
          grid-template-columns: repeat(2, minmax(0, 1fr));
        }
        .cparams {
          grid-template-columns: repeat(3, minmax(0, 1fr));
        }
        .status-strip {
          flex-wrap: wrap;
        }
        .section-title .meta {
          display: block;
          text-align: right;
        }
        .stats {
          grid-template-columns: repeat(2, minmax(0, 1fr));
        }
        .stat.featured {
          grid-column: 1 / -1;
          grid-row: auto;
          padding: 24px;
        }
        .stat.featured .value { font-size: 48px; margin: 16px 0; }
        .stat.requests-stat { grid-column: 1 / -1; }
        .metric-code { margin-bottom: 12px; }
      }
      @media (max-width: 600px) {
        .sidebar {
          position: static;
          width: 100%;
          padding: 0;
          display: block;
          border-right: 0;
          border-bottom: 1px solid var(--border);
        }
        .brand {
          display: none;
        }
        .nav {
          flex-direction: row;
          gap: 0;
          overflow: auto;
          padding: 0 8px;
        }
        .nav a {
          padding: 13px 12px;
          font-size: var(--text-sm);
          border-radius: 0;
          min-height: 46px;
        }
        .nav a span {
          display: block;
        }
        .nav .icon {
          display: none;
        }
        .shell {
          margin-left: 0;
        }
        .topbar {
          padding: 9px 16px;
          min-height: 60px;
        }
        .topbar .btn-label {
          display: none;
        }
        .main {
          padding: 23px 18px;
        }
        .section-title {
          align-items: flex-start;
          gap: 10px;
          margin-bottom: 20px;
        }
        .section-title h1 {
          font-size: 30px;
        }
        .eyebrow { font-size: 11px; letter-spacing: 1.2px; }
        .section-title { display: block; }
        .stat.featured { padding: 22px; }
        .stat.featured .value { font-size: 42px; }
        .stat.requests-stat { padding: 16px; }
        .section-title p {
          font-size: var(--text-sm);
          max-width: none;
        }
        .section-title > .row {
          margin-top: 16px;
          flex-direction: row;
          align-items: center;
          justify-content: space-between;
          gap: 6px;
        }
        .section-title select {
          max-width: 125px;
          font-size: var(--text-sm);
        }
        .section-title .meta {
          font-size: var(--text-xs);
          max-width: 125px;
        }
        .status-strip {
          padding: 12px;
        }
        .status-strip .sep {
          display: none;
        }
        .stats {
          gap: 10px;
        }
        .stat {
          padding: 15px 12px;
        }
        .stat .value {
          font-size: 27px;
        }
        .stat .value small {
          font-size: var(--text-xs);
        }
        .stat .label {
          font-size: var(--text-xs);
        }
        .stat .label .icon {
          display: none;
        }
        .stat .foot {
          font-size: var(--text-xs);
        }
        .panel-head {
          padding: 18px 17px 0;
        }
        .panel-body {
          padding: 18px 17px;
        }
        .request-head > div:first-child {
          flex-basis: 100%;
        }
        .table-footer {
          padding: 12px 17px;
        }
        .grid {
          gap: 10px;
        }
        .cell {
          padding: 12px;
        }
        .cell .v {
          font-size: 18px;
        }
        .hero .num {
          font-size: 32px;
        }
        .cparams {
          grid-template-columns: 1fr 1fr;
        }
        .save-bar {
          padding: 16px;
        }
        .page-footer {
          font-size: var(--text-xs);
        }
        .drawer {
          padding: 22px;
        }
        .ovr-key {
          width: 100%;
        }
        .chart-note {
          font-size: var(--text-xs);
        }
        .crumb {
          font-size: var(--text-xs);
        }
        .crumb b {
          margin-left: 6px;
        }
      }
      @media (prefers-reduced-motion: no-preference) {
        button,
        a {
          transition:
            background 0.15s,
            color 0.15s;
        }
        .drawer[open] {
          animation: slide 0.18s ease-out;
        }
        @keyframes slide {
          from {
            transform: translateX(20px);
            opacity: 0.5;
          }
          to {
            transform: none;
            opacity: 1;
          }
        }
      }

      ::selection {
        background: #58652e;
        color: #fff;
      }
      input,
      textarea {
        caret-color: var(--accent);
      }
      input::placeholder,
      textarea::placeholder {
        color: var(--dim);
        opacity: 1;
      }
      a {
        text-underline-offset: 0.2em;
      }
      .nav a {
        position: relative;
        min-height: 44px;
        transition: none;
      }
      .nav a:hover:not([aria-current]) {
        background: var(--hover);
      }
      .nav a[aria-current] {
        box-shadow: none;
      }
      .nav a[aria-current] span {
        font-weight: 600;
      }
      .nav a:hover[aria-current] {
        color: #19220c;
      }
      .stat .value {
        font-variant-numeric: tabular-nums;
        white-space: nowrap;
        overflow-wrap: normal;
      }
      .stat .foot {
        line-height: 1.6;
      }
      .analytics,
      .bottom-grid,
      .secondary-grid {
        align-items: start;
      }
      #trend-panel {
        min-height: 296px;
      }
      #capacity-panel .panel-body {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 0 16px;
      }
      #capacity-panel .number,
      #capacity-panel .capacity,
      #capacity-panel .hint {
        grid-column: 1 / -1;
      }
      #capacity-panel .summary-line {
        display: grid;
        gap: 3px;
        border: 0;
        padding: 8px 0;
      }
      #capacity-panel .summary-line strong {
        text-align: left;
      }
      .capacity > div + div {
        border-left: 1px solid var(--border);
        padding-left: 16px;
      }
      #s-finish {
        max-width: 36ch;
      }
      .requests {
        font-size: var(--text-base);
      }
      .requests td {
        white-space: nowrap;
      }
      .requests .detail-btn {
        display: inline-flex;
        align-items: center;
        justify-content: center;
      }
      .requests .detail-btn .icon {
        width: 16px;
        height: 16px;
      }
      .chart-switch button {
        min-height: 32px;
      }
      .chart-switch button:hover:not([aria-pressed="true"]) {
        background: var(--hover);
        color: var(--fg);
      }
      #chart-view {
        max-width: 120px;
        font-size: var(--text-xs);
      }
      .chart-controls {
        display: flex;
        gap: 10px;
        align-items: center;
        flex-wrap: wrap;
      }
      .chart-readout {
        min-height: 24px;
        font-size: var(--text-xs);
        font-variant-numeric: tabular-nums;
        color: var(--fg);
        margin-top: 6px;
      }
      .chart-filters {
        display: flex;
        flex-wrap: wrap;
        gap: 12px 20px;
        margin-bottom: 12px;
      }
      .chart-filters label {
        display: grid;
        gap: 5px;
        font-size: var(--text-xs);
        color: var(--dim);
        flex: 1 1 170px;
      }
      .chart-filters select {
        width: 100%;
        min-height: 44px;
      }
      .chart-context {
        font-size: var(--text-xs);
        color: var(--dim);
        line-height: 1.6;
        margin: 0 0 12px;
      }
      #throughput {
        outline-offset: 4px;
      }
      #throughput:focus-visible {
        outline: 2px solid var(--accent);
      }
      .ovr td:first-child {
        font-family: inherit;
        font-size: var(--text-base);
      }
      .ovr td:first-child .param-key {
        display: block;
        font-family: Consolas, monospace;
        color: var(--dim);
        font-size: var(--text-xs);
        margin-top: 3px;
      }
      .ovr tr.on {
        background: #ffcb8506;
      }
      .help { border-top: 2px solid var(--secondary); }
      .help h2, .msg .role { color: var(--secondary); }
      .notice {
        border-left: 2px solid var(--secondary);
        background: #171a2a;
      }
      .chat-empty h2 { font-size: 23px; letter-spacing: -0.4px; }
      .chat-empty > [data-icon] {
        display: inline-grid;
        place-items: center;
        width: 52px;
        height: 52px;
        border: 1px solid #685a95;
        border-radius: 14px;
        background: #25223c;
        color: var(--secondary);
        margin-bottom: 16px;
      }
      #capacity-panel { border-top: 2px solid var(--secondary); }
      #capabilities { background: var(--soft); }
      .requests th { background: var(--soft); }
      #memory .hero { border-bottom: 1px solid var(--border); padding-bottom: 24px; }
      #ovr-err:focus-visible { outline: 2px solid var(--accent); outline-offset: 4px; }
      #settings .secondary-grid > section {
        overflow: visible;
      }
      #settings .save-bar {
        position: sticky;
        bottom: 0;
        z-index: 2;
        border-radius: 0 0 10px 10px;
      }
      #settings .save-bar.is-tall {
        position: static;
      }
      .field-error {
        display: block;
        color: var(--err);
        font-size: var(--text-xs);
        margin-top: 6px;
        overflow-wrap: anywhere;
      }
      [aria-invalid="true"] {
        border-color: var(--err);
      }
      #ovr-key-field {
        min-width: 0;
        width: 100%;
      }
      #ovr-key-field label {
        display: block;
        font-size: var(--text-sm);
        margin-bottom: 6px;
      }
      #ovr-key {
        width: min(100%, 340px);
      }
      #ovr-err {
        flex-basis: 100%;
        margin: 0;
      }
      #ovr-save {
        min-width: 140px;
      }
      .save-bar.is-dirty {
        border-top-color: var(--warn);
      }
      .save-bar.is-dirty #ovr-status {
        color: var(--warn);
      }
      .drawer .between {
        justify-content: flex-end;
      }
      #detail-title {
        margin-top: 12px;
      }
      .empty {
        white-space: normal !important;
        max-width: 60ch;
      }
      .empty strong {
        display: block;
        color: var(--fg);
        margin-bottom: 5px;
        font-weight: 500;
      }
      @media (min-width: 1151px) {
        .help {
          position: sticky;
          top: 20px;
        }
      }
      @media (max-width: 850px) {
        .help .panel-body {
          columns: 2;
          column-gap: 24px;
        }
        .help h2 {
          column-span: all;
        }
        .help p {
          break-inside: avoid;
        }
      }
      @media (max-width: 600px) {
        :root {
          --control-height: 44px;
        }
        .main {
          padding: 20px 16px;
        }
        .sidebar {
          position: sticky;
          top: 0;
          z-index: 5;
        }
        .nav {
          padding: 0 6px;
          gap: 0;
          justify-content: space-between;
          overflow: visible;
        }
        .nav a {
          padding: 12px 5px;
          font-size: var(--text-xs);
          min-height: 48px;
          justify-content: center;
          flex: 1;
        }
        .nav [data-icon] {
          display: none;
        }
        .nav a[aria-current],
        .nav a:hover[aria-current] {
          box-shadow: inset 0 -2px var(--accent);
          background: transparent;
          color: var(--accent);
        }
        .topbar {
          padding: 10px 16px;
        }
        .status-strip small {
          flex-basis: 100%;
        }
        .status-strip {
          gap: 6px;
        }
        .stat .value {
          font-size: 25px;
          display: flex;
          flex-wrap: wrap;
          align-items: baseline;
          column-gap: 3px;
        }
        .stat .value small {
          margin-left: 0;
        }
        .stat .foot {
          font-size: var(--text-xs);
        }
        .chart-switch button {
          min-height: 40px;
        }
        .chart-controls {
          width: 100%;
          justify-content: space-between;
        }
        #chart-view {
          min-height: 44px;
        }
        .chart-note {
          flex-wrap: wrap;
          gap: 4px;
        }
        .requests,
        .ovr {
          min-width: 0;
        }
        .requests thead,
        .ovr thead {
          position: absolute;
          width: 1px;
          height: 1px;
          clip-path: inset(50%);
          overflow: hidden;
        }
        .requests tbody,
        .ovr tbody {
          display: block;
        }
        .requests tr {
          display: grid;
          grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
          gap: 12px 16px;
          padding: 16px;
          border-bottom: 1px solid var(--border);
        }
        .requests tr:last-child {
          border-bottom: 0;
        }
        .requests td {
          display: block;
          border: 0;
          padding: 0;
          white-space: normal;
          overflow-wrap: anywhere;
        }
        .requests td:nth-child(2),
        .requests td:last-child {
          text-align: right;
        }
        .requests td[data-label]::before {
          content: attr(data-label);
          display: block;
          color: var(--dim);
          font:
            var(--text-xs)/1.5 "Segoe UI",
            sans-serif;
          margin-bottom: 4px;
        }
        .requests td.empty {
          grid-column: 1 / -1;
          padding: 12px 0;
          text-align: left;
        }
        .requests .detail-btn {
          border: 1px solid var(--border);
          min-width: 72px;
        }
        .ovr tr {
          display: grid;
          grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
          gap: 10px 12px;
          padding: 16px;
          border-bottom: 1px solid var(--border);
        }
        .ovr td {
          display: block;
          padding: 0;
          border: 0;
          min-width: 0;
        }
        .ovr td:first-child,
        .ovr td.note {
          grid-column: 1 / -1;
        }
        .ovr td.note {
          min-width: 0;
          max-width: none;
        }
        .ovr td[data-label]::before {
          content: attr(data-label);
          display: block;
          color: var(--dim);
          font-size: var(--text-xs);
          margin-bottom: 5px;
        }
        .ovr input,
        .ovr select {
          width: 100%;
          min-height: 44px;
        }
        .save-bar {
          gap: 8px;
          padding: 12px 16px;
        }
        .save-bar .btn {
          flex: 1 1 auto;
          font-size: var(--text-xs);
        }
        .save-bar #ovr-key {
          width: 100%;
        }
        .help .panel-body {
          columns: auto;
        }
        .summary-line {
          font-size: var(--text-sm);
        }
        .table-footer,
        .page-footer {
          font-size: var(--text-xs);
        }
        .drawer {
          padding: 18px;
        }
      }
    </style>
  </head>
  <body>
    <a class="skip" href="#content">跳转到内容</a>
    <aside class="sidebar">
      <a class="brand" href="#overview"
        ><div class="brand-mark" data-icon="chip"></div>
        <span><strong>gfx1151</strong><small>ENGINE CONSOLE</small></span></a
      >
      <div class="nav-caption">工作空间</div>
      <nav class="nav" aria-label="控制台导航">
        <a href="#overview" data-page="overview" title="运行总览"
          ><i data-icon="grid"></i><span>运行总览</span></a
        >
        <a href="#requests" data-page="requests" title="请求记录"
          ><i data-icon="list"></i><span>请求记录</span></a
        >
        <a href="#settings" data-page="settings" title="参数设置"
          ><i data-icon="tune"></i><span>参数设置</span></a
        >
        <a href="#chat" data-page="chat" title="对话测试"
          ><i data-icon="chat"></i><span>对话测试</span></a
        >
        <a href="#memory" data-page="memory" title="内存资源"
          ><i data-icon="chip"></i><span>内存资源</span></a
        >
      </nav>
      <div class="sidebar-bottom">
        <span id="conn" class="chip">连接中…</span
        ><span id="target" class="mono"></span>
        <p style="margin-top: 12px">本地推理 · 同源访问</p>
      </div>
    </aside>
    <div class="shell">
      <header class="topbar">
        <div class="crumb">
          控制台 <span aria-hidden="true">/</span
          ><b id="page-label">运行总览</b>
        </div>
        <div class="row">
          <button class="btn" id="pause" aria-pressed="false">
            <i data-icon="pause"></i><span>暂停刷新</span></button
          ><button
            class="btn"
            id="copy-api"
            title="复制 API 地址"
            aria-label="复制 API 地址"
          >
            <i data-icon="copy"></i><span class="btn-label">复制 API 地址</span>
          </button>
        </div>
      </header>
      <main class="main" id="content" tabindex="-1">
        <div class="section-title">
          <div>
            <span class="eyebrow" id="page-code" aria-hidden="true">01 / OVERVIEW</span>
            <h1 id="page-title">运行总览</h1>
            <p id="page-description">查看服务状态、推理表现与最近请求。</p>
          </div>
          <div class="row">
            <select id="reqs-range" aria-label="统计时间范围">
              <option value="1">今天（UTC）</option>
              <option value="7">近 7 天（UTC）</option>
              <option value="0">全部历史</option></select
            ><span class="meta" id="refresh-meta">等待数据</span>
          </div>
        </div>
        <div id="health-err" class="errmsg" role="status"></div>
        <div id="reqs-err" class="errmsg" role="status"></div>
        <section data-view="overview" id="overview">
          <div class="status-strip" id="health-data">
            <div class="row">
              <span class="chip" id="h-status">连接中…</span
              ><span class="sep"></span
              ><strong class="mono" id="h-model">等待模型信息</strong>
            </div>
            <small id="health-meta">状态来自 API 进程</small>
          </div>
          <div class="stats" id="reqs-grid">
            <article class="stat featured">
              <svg class="signal-mark" viewBox="0 0 140 140" fill="none" aria-hidden="true">
                <path d="M15 104 66 16h22l-51 88M48 124 99 36h22l-51 88M82 140 133 52h22l-51 88" stroke="#b8a4ff" stroke-width="2" />
              </svg>
              <div class="metric-code" aria-hidden="true">INFERENCE / PREFILL</div>
              <div class="label">
                Prefill · 输入处理<i data-icon="pulse"></i>
              </div>
              <div class="value">
                <span id="s-prefill">—</span><small>tok/s</small>
              </div>
              <div class="foot" id="s-prefill-note">不含缓存命中 token</div>
            </article>
            <article class="stat">
              <div class="label">
                Decode · 输出生成<i data-icon="arrow"></i>
              </div>
              <div class="value">
                <span id="s-decode">—</span><small>tok/s</small>
              </div>
              <div class="foot" id="s-output">等待输出统计</div>
            </article>
            <article class="stat">
              <div class="label">首字延迟 · P50<i data-icon="clock"></i></div>
              <div class="value" id="s-ttft">—</div>
              <div class="foot" id="s-p95">P95 · 等待流式记录</div>
            </article>
            <article class="stat requests-stat">
              <div class="label">请求总数<i data-icon="list"></i></div>
              <div class="value">
                <span id="s-requests">—</span><small>次</small>
              </div>
              <div class="foot" id="s-finish">当前统计范围</div>
            </article>
          </div>
          <div class="analytics">
            <section class="panel" id="trend-panel">
              <div class="panel-head">
                <div>
                  <h2>吞吐表现</h2>
                  <p>每点代表一次请求 · 可按输入规模和缓存状态比较</p>
                </div>
                <div class="chart-controls">
                  <select id="chart-view" aria-label="吞吐图横轴">
                    <option value="request">按请求顺序</option>
                    <option value="time">按实际时间</option>
                  </select>
                  <div
                    class="chart-switch"
                    role="group"
                    aria-label="吞吐图指标"
                  >
                    <button data-chart="prefill" aria-pressed="true">
                      Prefill</button
                    ><button data-chart="decode" aria-pressed="false">
                      Decode
                    </button>
                  </div>
                </div>
              </div>
              <div class="panel-body">
                <div class="chart-filters">
                  <label for="chart-size">实际新增 token
                    <select id="chart-size">
                      <option value="all">全部规模</option>
                      <option value="tiny">1–128</option>
                      <option value="small">129–512</option>
                      <option value="medium">513–2,048</option>
                      <option value="large">2,049–8,192</option>
                      <option value="bulk">超过 8,192</option>
                    </select>
                  </label>
                  <label for="chart-cache">前缀缓存
                    <select id="chart-cache">
                      <option value="all">全部状态</option>
                      <option value="miss">未命中 · 完整预填充</option>
                      <option value="partial">部分命中 · 增量预填充</option>
                      <option value="full">全部命中 · 无新增输入</option>
                    </select>
                  </label>
                </div>
                <p class="chart-context" id="chart-context">
                  实际新增 = 输入减去缓存命中。短输入的 tok/s 较低，不一定出首字更慢。
                </p>
                <canvas
                  id="throughput"
                  role="img"
                  tabindex="0"
                  aria-describedby="chart-context chart-readout chart-note"
                  aria-label="请求吞吐趋势"
                ></canvas>
                <div
                  class="chart-readout"
                  id="chart-readout"
                  aria-live="polite"
                >
                  将鼠标移到采样点，或聚焦图表后按方向键查看。
                </div>
                <div class="chart-note">
                  <span id="chart-note">等待请求记录</span
                  ><span id="chart-axis-label">tok/s · 请求顺序</span>
                </div>
              </div>
            </section>
            <section class="panel" id="capacity-panel">
              <div class="panel-head">
                <h2>并发与队列</h2>
                <span class="chip" id="queue-status">等待数据</span>
              </div>
              <div class="panel-body">
                <div class="number">
                  <span id="h-inflight">—</span>
                  <small>/ <span id="h-slots">—</span> 并发容量</small>
                </div>
                <div class="capacity">
                  <div>
                    <small>等待队列</small><strong id="h-queued">—</strong>
                  </div>
                  <div>
                    <small>持续忙碌</small><strong id="h-busyfor">—</strong>
                  </div>
                </div>
                <div class="summary-line">
                  <span>输入 token</span
                  ><strong class="mono" id="s-input">—</strong>
                </div>
                <div class="summary-line">
                  <span>缓存命中</span
                  ><strong class="mono" id="s-cache">—</strong>
                </div>
                <p class="hint" style="margin-top: 10px">
                  API 自报状态；不代表引擎存活探测。
                </p>
              </div>
            </section>
          </div>
          <div class="bottom-grid">
            <div id="recent-table"></div>
            <section class="panel" id="capabilities">
              <div class="panel-head">
                <h2>服务能力</h2>
                <span class="chip">只读概览</span>
              </div>
              <div class="panel-body">
                <div class="summary-line">
                  <span>上下文上限 / 路</span
                  ><strong class="mono" id="h-ctx">—</strong>
                </div>
                <div class="summary-line">
                  <span>工具调用</span><strong id="h-tools">—</strong>
                </div>
                <div class="summary-line">
                  <span>图像输入上限</span><strong id="h-vision">—</strong>
                </div>
                <div class="summary-line">
                  <span>参数覆盖</span><a href="#settings" id="h-ovr">—</a>
                </div>
                <div id="reqs-acc" class="acceptance"></div>
              </div>
            </section>
          </div>
        </section>
        <section data-view="requests" id="requests" hidden>
          <div class="notice">
            统计卡片汇总所选日期的全部记录。此列表与趋势图在服务器最近
            <strong>100 条</strong
            >记录内按日期筛选；结束原因筛选只影响列表。时间均为 UTC。
          </div>
          <div id="all-table"></div>
        </section>
        <section data-view="settings" id="settings" hidden>
          <div class="notice">
            <strong>服务端参数覆盖</strong> · 保存后立即影响后续请求，适用于
            Chat Completions、Responses 和
            Completions。思考参数仅适用于对话模板。这里不修改引擎的计算精度配置。
          </div>
          <div class="secondary-grid">
            <section class="panel">
              <div class="panel-head">
                <h2>采样与思考</h2>
                <span class="chip" id="ovr-count">等待配置</span>
              </div>
              <div class="table-wrap" style="margin-top: 16px">
                <table class="ovr">
                  <thead>
                    <tr>
                      <th>参数</th>
                      <th>覆盖模式</th>
                      <th>值</th>
                      <th>说明</th>
                    </tr>
                  </thead>
                  <tbody id="ovr-body"></tbody>
                </table>
              </div>
              <div class="save-bar">
                <div id="ovr-key-field" hidden>
                  <label for="ovr-key">管理密钥</label>
                  <input
                  id="ovr-key"
                  type="password"
                  autocomplete="off"
                  placeholder="GDEC_API_ADMIN_KEY"
                  aria-describedby="ovr-key-error"
                  />
                  <span id="ovr-key-error" class="field-error" hidden></span>
                </div>
                <button id="ovr-save" class="btn primary" disabled>
                  保存并立即生效</button
                ><button id="ovr-reload" class="btn">重新读取</button
                ><button id="ovr-clear" class="btn" disabled>跟随客户端</button
                ><small id="ovr-status" role="status">尚未读取</small>
                <div class="errmsg" id="ovr-err" role="alert" tabindex="-1"></div>
              </div>
              <div class="panel-body" style="padding-top: 0">
                <div class="hint mono" id="ovr-meta"></div>
              </div>
            </section>
            <aside class="panel help">
              <div class="panel-body">
                <h2>覆盖规则</h2>
                <p><strong>跟随客户端</strong><br />不干预请求中的参数。</p>
                <p><strong>默认值</strong><br />仅在客户端没有传值时补齐。</p>
                <p>
                  <strong>强制覆盖</strong><br />替换客户端传入的值。max_tokens
                  的强制模式是上限，较小的客户端值仍保留。
                </p>
                <p>
                  响应头 X-Gdec-Overrides
                  会列出被改写的字段；对话测试中可直接查看。
                </p>
                <p>管理密钥只保留在当前页面，不写入浏览器存储。</p>
              </div>
            </aside>
          </div>
        </section>
        <section data-view="chat" id="chat" hidden>
          <div class="secondary-grid">
            <section class="panel">
              <div class="panel-head">
                <h2>对话测试</h2>
                <span class="chip">流式输出</span>
              </div>
              <div class="panel-body">
                <div class="chat-empty" id="chat-empty">
                  <i data-icon="chat"></i>
                  <h2>验证一次真实的使用体验</h2>
                  <p>
                    思考过程、回答正文和耗时分别展示。请求使用当前模型，并受服务端参数覆盖规则影响。
                  </p>
                </div>
                <div id="c-log" aria-label="对话记录"></div>
                <div class="chat-compose">
                  <label class="hint" for="c-input">测试消息</label
                  ><textarea
                    id="c-input"
                    rows="3"
                    placeholder="输入消息，Ctrl / ⌘ + Enter 发送"
                  ></textarea>
                  <div class="row" style="margin-top: 12px">
                    <button id="c-send" class="btn primary">发送消息</button
                    ><button id="c-stop" class="btn" disabled>停止</button
                    ><button id="c-clear" class="btn">清空对话</button
                    ><span id="chat-status" role="status"></span>
                  </div>
                  <div class="errmsg" id="chat-err" role="alert"></div>
                </div>
              </div>
            </section>
            <aside class="panel">
              <div class="panel-head"><h2>本次请求参数</h2></div>
              <div class="panel-body">
                <p class="hint" style="margin-bottom: 16px">
                  留空表示不传，使用服务端默认值。并发已满时请求会排队。
                </p>
                <div class="cparams">
                  <label
                    >思考开关<select id="c-think">
                      <option value="">不传</option>
                      <option value="true">开</option>
                      <option value="false">关</option>
                    </select></label
                  ><label
                    >思考强度<select id="c-effort">
                      <option value="">不传</option>
                      <option>low</option>
                      <option>medium</option>
                      <option>xhigh</option>
                    </select></label
                  >
                  <label
                    >temperature<input
                      id="c-temp"
                      type="number"
                      min="0"
                      max="2"
                      step="0.05"
                      placeholder="不传" /></label
                  ><label
                    >top_p<input
                      id="c-topp"
                      type="number"
                      min="0"
                      max="1"
                      step="0.01"
                      placeholder="不传" /></label
                  ><label
                    >top_k<input
                      id="c-topk"
                      type="number"
                      min="0"
                      max="2147483647"
                      step="1"
                      placeholder="不传" /></label
                  ><label
                    >min_p<input
                      id="c-minp"
                      type="number"
                      min="0"
                      max="1"
                      step="0.01"
                      placeholder="不传" /></label
                  ><label
                    >presence_penalty<input
                      id="c-pres"
                      type="number"
                      min="-2"
                      max="2"
                      step="0.1"
                      placeholder="不传" /></label
                  ><label
                    >max_tokens<input
                      id="c-max"
                      type="number"
                      min="1"
                      max="2147483647"
                      step="1"
                      placeholder="不传" /></label
                  ><label
                    >seed<input
                      id="c-seed"
                      type="number"
                      min="0"
                      max="2147483647"
                      step="1"
                      placeholder="随机"
                  /></label>
                </div>
                <details>
                  <summary>System 提示词</summary>
                  <textarea
                    id="c-system"
                    rows="5"
                    aria-label="System 提示词"
                    placeholder="可选的系统提示词"
                  ></textarea>
                </details>
                <p class="hint" style="margin-top: 17px">
                  首 token 耗时在浏览器端测量，包含排队和网络时间。
                </p>
              </div>
            </aside>
          </div>
        </section>
        <section data-view="memory" id="memory" hidden>
          <div class="notice">
            此页仅在 API 空闲且页面可见时每 10
            秒尝试采样。查询会使用引擎槽位；推理忙碌时跳过。<strong
              >记账值不等于物理驻留，UMA 共享内存不能视为独立显存。</strong
            >
          </div>
          <section class="panel">
            <div class="panel-head">
              <h2>内存资源</h2>
              <span class="hint" id="mem-meta">尚未采样</span>
            </div>
            <div class="panel-body">
              <div id="mem-data">
                <div class="hero">
                  <span class="num mono" id="m-hero">—</span
                  ><span class="lab"
                    >GPU 可访问记账 · committed<br /><span
                      id="m-accounting"
                    ></span
                  ></span>
                </div>
                <div class="barlab">
                  <span>HIP 报告已用</span><span id="m-hip-pct"></span>
                </div>
                <div class="bar"><i id="m-hip-bar" style="width: 0%"></i></div>
                <div class="barlab">
                  <span id="m-hip-used">等待数据</span
                  ><span id="m-hip-total"></span>
                </div>
                <div class="grid memory-grid" id="mem-grid"></div>
                <canvas
                  id="spark"
                  role="img"
                  aria-label="内存采样趋势"
                ></canvas>
                <div class="legend">
                  <span
                    ><i style="background: var(--c-committed)"></i>GPU
                    committed</span
                  ><span><i style="background: var(--c-rss)"></i>进程 RSS</span
                  ><span>最近 60 次采样 · 按采样顺序</span>
                </div>
              </div>
              <div id="mem-err" class="errmsg" role="status"></div>
              <details>
                <summary>原始状态数据</summary>
                <pre id="raw-health">等待健康状态</pre>
                <pre id="raw-mem">尚未采样</pre>
              </details>
            </div>
          </section>
        </section>
        <footer class="page-footer">
          <span>gfx1151 engine · 本地控制台</span
          ><span id="poll-state">API 状态每 2 秒 · 请求统计每 10 秒</span>
        </footer>
      </main>
    </div>
    <dialog class="drawer" id="request-detail" aria-labelledby="detail-title">
      <div class="between">
        <button id="detail-close" class="btn" aria-label="关闭详情">
          关闭 <i data-icon="close"></i>
        </button>
      </div>
      <h2 id="detail-title">请求详情</h2>
      <div id="detail-body"></div>
    </dialog>
    <div id="toast" role="status" hidden></div>

    <script>
      "use strict";
      const $ = (id) => document.getElementById(id);
      const HEALTH_MS = 2000,
        REQ_MS = 10000,
        MEM_MS = 10000,
        TIMEOUT_MS = 5000,
        HIST_CAP = 60;
      let paused = false,
        page = "overview",
        health = null,
        healthAt = 0,
        reqAt = 0,
        memAt = 0;
      let records = [],
        summary = null,
        chartMetric = "prefill",
        reasonFilter = "all",
        toastTimer;
      let chartPoints = [],
        chartSelected = -1;
      const pending = { health: false, reqs: false, memory: false };
      const hist = { committed: [], rss: [] };
      const icons = {
        chip: '<rect x="5" y="5" width="14" height="14" rx="3"/><path d="M9 2v3m6-3v3M9 19v3m6-3v3M2 9h3m-3 6h3m14-6h3m-3 6h3"/><rect x="9" y="9" width="6" height="6" rx="1"/>',
        grid: '<rect x="3" y="3" width="7" height="7" rx="1.5"/><rect x="14" y="3" width="7" height="7" rx="1.5"/><rect x="3" y="14" width="7" height="7" rx="1.5"/><rect x="14" y="14" width="7" height="7" rx="1.5"/>',
        list: '<path d="M8 5h13M8 12h13M8 19h13M3 5h.01M3 12h.01M3 19h.01"/>',
        tune: '<path d="M4 6h7m4 0h5M4 12h2m4 0h10M4 18h10m4 0h2"/><circle cx="13" cy="6" r="2"/><circle cx="8" cy="12" r="2"/><circle cx="16" cy="18" r="2"/>',
        chat: '<path d="M21 11.5a8.5 8.5 0 0 1-8.5 8.5H4l-2 2V11.5a9.5 9.5 0 0 1 19 0Z"/><path d="M7 10h8m-8 4h5"/>',
        pulse: '<path d="M2 12h4l3-8 5 16 3-8h5"/>',
        arrow: '<path d="M5 16 19 6M8 6h11v11"/>',
        clock: '<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>',
        pause: '<path d="M9 5v14M15 5v14"/>',
        play: '<path d="m8 4 12 8-12 8Z"/>',
        close: '<path d="m6 6 12 12M18 6 6 18"/>',
        chevron: '<path d="m9 5 7 7-7 7"/>',
        copy: '<rect x="8" y="8" width="12" height="12" rx="2"/><path d="M16 8V4a1 1 0 0 0-1-1H4a1 1 0 0 0-1 1v11a1 1 0 0 0 1 1h4"/>',
      };
      document.querySelectorAll("[data-icon]").forEach((el) => {
        el.innerHTML =
          '<svg class="icon" viewBox="0 0 24 24" aria-hidden="true">' +
          icons[el.dataset.icon] +
          "</svg>";
      });
      function icon(name) {
        const el = document.createElementNS(
          "http://www.w3.org/2000/svg",
          "svg",
        );
        el.setAttribute("class", "icon");
        el.setAttribute("viewBox", "0 0 24 24");
        el.setAttribute("aria-hidden", "true");
        el.innerHTML = icons[name];
        return el;
      }
      function txt(id, value) {
        $(id).textContent = value;
      }
      function node(tag, text, cls) {
        const n = document.createElement(tag);
        if (text != null) n.textContent = text;
        if (cls) n.className = cls;
        return n;
      }
      function finite(x) {
        return typeof x === "number" && Number.isFinite(x);
      }
      function fmtTok(x) {
        return finite(x) ? x.toLocaleString("en-US") : "—";
      }
      function fmtRate(x) {
        return finite(x)
          ? x.toLocaleString("en-US", {
              minimumFractionDigits: 1,
              maximumFractionDigits: 1,
            })
          : "—";
      }
      function fmtPct(x) {
        return finite(x) ? (x * 100).toFixed(1) + "%" : "—";
      }
      function fmtMs(x) {
        return finite(x)
          ? x >= 1000
            ? (x / 1000).toFixed(2) + " s"
            : Math.round(x) + " ms"
          : "—";
      }
      function fmtBytes(n) {
        if (!finite(n)) return "—";
        const g = 1073741824,
          m = 1048576,
          k = 1024;
        return Math.abs(n) >= g
          ? (n / g).toFixed(2) + " GiB"
          : Math.abs(n) >= m
            ? (n / m).toFixed(1) + " MiB"
            : Math.abs(n) >= k
              ? (n / k).toFixed(1) + " KiB"
              : n + " B";
      }
      function hhmmss(at = Date.now()) {
        return new Date(at).toLocaleTimeString("zh-CN", { hour12: false });
      }
      function utc(at) {
        return finite(at)
          ? new Date(at).toISOString().replace("T", " ").slice(0, 19)
          : "—";
      }
      function setChip(el, cls, text) {
        el.className = "chip " + cls;
        el.textContent = text;
      }
      function errText(j, status) {
        return j?.error?.message || j?.error?.type || "HTTP " + status;
      }
      async function grab(url, opts) {
        const c = new AbortController(),
          t = setTimeout(() => c.abort(), TIMEOUT_MS);
        try {
          const r = await fetch(url, {
            cache: "no-store",
            ...opts,
            signal: c.signal,
          });
          let j;
          try {
            j = await r.json();
          } catch {
            throw new Error("服务返回了无效 JSON（HTTP " + r.status + "）");
          }
          if (!r.ok) {
            const e = new Error(errText(j, r.status));
            e.status = r.status;
            throw e;
          }
          if (!j || typeof j !== "object") throw new Error("服务返回了空数据");
          return j;
        } catch (e) {
          if (e.name === "AbortError") throw new Error("请求超时");
          throw e;
        } finally {
          clearTimeout(t);
        }
      }
      function showErr(id, e) {
        const el = $(id);
        el.textContent = hhmmss() + " · " + e;
        el.classList.add("show");
      }
      function hideErr(id) {
        $(id).classList.remove("show");
      }
      function toast(message) {
        txt("toast", message);
        $("toast").hidden = false;
        clearTimeout(toastTimer);
        toastTimer = setTimeout(() => ($("toast").hidden = true), 4200);
      }
      const titles = {
        overview: ["运行总览", "查看服务状态、推理表现与最近请求。"],
        requests: ["请求记录", "从单次请求定位吞吐、延迟与结束原因。"],
        settings: ["参数设置", "明确客户端参数与服务端覆盖的关系。"],
        chat: ["对话测试", "在当前模型上验证真实的流式回复。"],
        memory: ["内存资源", "分别查看记账、驻留与 HIP 内存口径。"],
      };
      function activatePage() {
        const candidate = location.hash.slice(1);
        page = titles[candidate] ? candidate : "overview";
        document
          .querySelectorAll("[data-view]")
          .forEach((el) => (el.hidden = el.dataset.view !== page));
        document.querySelectorAll("[data-page]").forEach((el) => {
          if (el.dataset.page === page) el.setAttribute("aria-current", "page");
          else el.removeAttribute("aria-current");
        });
        const pageCodes = { overview: "01 / OVERVIEW", requests: "02 / REQUESTS", settings: "03 / PARAMETERS", chat: "04 / PLAYGROUND", memory: "05 / MEMORY" };
        txt("page-code", pageCodes[page]);
        txt("page-label", titles[page][0]);
        txt("page-title", titles[page][0]);
        txt("page-description", titles[page][1]);
        document.title = titles[page][0] + " · gfx1151 控制台";
        $("reqs-range").hidden = !["overview", "requests"].includes(page);
        $("reqs-err").hidden = !["overview", "requests"].includes(page);
        updateRefreshMeta();
        if (page === "overview" || page === "requests") {
          tickReqs();
          requestAnimationFrame(drawThroughput);
        }
        if (page === "settings" && !ovrLoaded) loadOvr();
        if (page === "memory") {
          tickHealth().then(tickMem);
          requestAnimationFrame(drawSpark);
        }
        requestAnimationFrame(updateSettingsViewport);
      }
      function updateRefreshMeta() {
        const at = ["overview", "requests"].includes(page)
          ? reqAt
          : page === "memory"
            ? memAt
            : healthAt;
        txt(
          "refresh-meta",
          (paused ? "已暂停 · " : document.hidden ? "后台暂停 · " : "") +
            (at ? "更新于 " + hhmmss(at) : "等待数据"),
        );
        txt(
          "poll-state",
          paused
            ? "自动刷新已暂停 · 显示最后一次数据"
            : document.hidden
              ? "页面在后台 · 自动刷新暂停"
              : "API 状态每 2 秒 · 请求统计每 10 秒",
        );
      }
      function pollingAllowed() {
        return !paused && !document.hidden;
      }
      async function tickHealth() {
        if (!pollingAllowed() || pending.health) return;
        pending.health = true;
        try {
          health = await grab("/health");
          healthAt = Date.now();
          renderHealth(health);
          hideErr("health-err");
          ["health-data", "capacity-panel", "capabilities"].forEach((id) =>
            $(id).classList.remove("stale"),
          );
          setChip($("conn"), "ok", "API 已连接");
        } catch (e) {
          healthAt = 0;
          ["health-data", "capacity-panel", "capabilities"].forEach((id) =>
            $(id).classList.add("stale"),
          );
          setChip($("h-status"), "err", "API 无法连接");
          setChip($("conn"), "err", "无法连接");
          showErr(
            "health-err",
            e.message + " · 保留的数据可能已过期，将自动重试",
          );
        } finally {
          pending.health = false;
          updateRefreshMeta();
        }
      }
      function renderHealth(j) {
        setChip(
          $("h-status"),
          j.status !== "ok" ? "err" : j.busy ? "warn" : "ok",
          j.status !== "ok"
            ? "API " + j.status
            : j.busy
              ? "API 忙碌"
              : "API 已连接",
        );
        txt("h-model", j.model ?? "—");
        txt("h-ctx", fmtTok(j.slot_ctx ?? j.context) + " token");
        txt("h-inflight", fmtTok(j.in_flight));
        txt("h-slots", fmtTok(j.slots));
        txt("h-queued", fmtTok(j.queued));
        txt(
          "h-busyfor",
          finite(j.busy_for_s) ? j.busy_for_s.toFixed(1) + " s" : "—",
        );
        setChip(
          $("queue-status"),
          j.queued > 0 ? "warn" : "ok",
          j.queued > 0 ? "有请求排队" : "暂无排队",
        );
        txt("health-meta", "API 状态 · " + hhmmss());
        txt(
          "h-tools",
          j.tool_calls?.parsed
            ? j.tool_calls.streaming
              ? "支持流式调用"
              : "支持"
            : "未报告支持",
        );
        txt(
          "h-vision",
          j.vision?.accepted
            ? fmtTok(j.vision.max_images) + " 张"
            : "未报告支持",
        );
        const count = Object.keys(j.server_overrides || {}).length;
        txt("h-ovr", count ? count + " 项生效 →" : "跟随客户端 →");
        txt("raw-health", JSON.stringify(j, null, 2));
      }
      const FINISH_ZH = {
        stop: "自然结束",
        length: "长度上限",
        cancel: "取消 / 断开",
        error: "错误",
        other: "其他",
      };
      function finZh(x) {
        return FINISH_ZH[x] || String(x ?? "其他");
      }
      function reasonChip(x) {
        const e = node("span", finZh(x));
        setChip(
          e,
          x === "stop"
            ? "ok"
            : x === "length" || x === "cancel"
              ? "warn"
              : x === "error"
                ? "err"
                : "",
          finZh(x),
        );
        return e;
      }
      function rangeBounds() {
        const value = $("reqs-range").value;
        if (value === "0") return { from: 0, to: Infinity, q: "" };
        const today = new Date().toISOString().slice(0, 10);
        const to = Date.parse(today) + 86400000;
        const from = to - Number(value) * 86400000;
        return {
          from,
          to,
          q:
            "?from=" +
            new Date(from).toISOString().slice(0, 10) +
            "&to=" +
            today,
        };
      }
      function scopedRecords() {
        const b = rangeBounds();
        return records
          .filter((r) => r.ts_ms >= b.from && r.ts_ms < b.to)
          .sort((a, b) => b.ts_ms - a.ts_ms || b.req_seq - a.req_seq);
      }
      function prefillRate(r) {
        const n = r.prompt_tokens - r.cached_tokens;
        return n > 0 && r.prefill_ms > 0 ? (n * 1000) / r.prefill_ms : null;
      }
      function decodeRate(r) {
        return r.output_tokens > 0 && r.decode_ms > 0
          ? (r.output_tokens * 1000) / r.decode_ms
          : null;
      }
      function chartMatches(r) {
        const n = r.prompt_tokens - r.cached_tokens,
          size = $("chart-size").value,
          cache = $("chart-cache").value,
          bounds = { tiny: [1, 128], small: [129, 512], medium: [513, 2048],
            large: [2049, 8192], bulk: [8193, Infinity] };
        return (
          (size === "all" || (n >= bounds[size][0] && n <= bounds[size][1])) &&
          (cache === "all" ||
            (cache === "miss" && r.cached_tokens === 0) ||
            (cache === "partial" && r.cached_tokens > 0 && n > 0) ||
            (cache === "full" && r.prompt_tokens > 0 && n === 0))
        );
      }
      let queuedManualRefresh = false;
      async function tickReqs(manual = false) {
        if (manual) queuedManualRefresh = true;
        if (
          (!queuedManualRefresh && !pollingAllowed()) ||
          pending.reqs ||
          !["overview", "requests"].includes(page)
        )
          return;
        pending.reqs = true;
        queuedManualRefresh = false;
        const q = rangeBounds().q;
        try {
          const [s, t] = await Promise.all([
            grab("/reqstat/summary" + q),
            grab("/reqstat/tail?n=100"),
          ]);
          if (q !== rangeBounds().q) return;
          if (!Array.isArray(t.records)) throw new Error("请求记录格式无效");
          summary = s;
          records = t.records;
          reqAt = Date.now();
          renderReqSummary(s);
          renderTables();
          drawThroughput();
          hideErr("reqs-err");
          [
            "reqs-grid",
            "recent-table",
            "all-table",
            "trend-panel",
            "reqs-acc",
            "s-input",
            "s-cache",
          ].forEach((id) => $(id).classList.remove("stale"));
          if (s.bad_crc || t.bad_crc)
            showErr("reqs-err", "部分损坏记录已跳过，统计可能不完整");
        } catch (e) {
          [
            "reqs-grid",
            "recent-table",
            "all-table",
            "trend-panel",
            "reqs-acc",
            "s-input",
            "s-cache",
          ].forEach((id) => $(id).classList.add("stale"));
          showErr("reqs-err", e.message + " · 统计未更新，保留上次数据");
        } finally {
          pending.reqs = false;
          updateRefreshMeta();
          if (q !== rangeBounds().q || queuedManualRefresh)
            tickReqs(manual || queuedManualRefresh);
        }
      }
      function renderReqSummary(j) {
        txt("s-requests", fmtTok(j.requests));
        txt("s-prefill", fmtRate(j.prefill_tok_per_s));
        txt("s-decode", fmtRate(j.decode_tok_per_s));
        txt("s-input", fmtTok(j.input_tokens));
        txt("s-cache", fmtPct(j.cache_hit_rate));
        txt(
          "s-finish",
          Object.entries(j.finish || {})
            .filter(([, n]) => n > 0)
            .map(([k, n]) => finZh(k) + " " + fmtTok(n))
            .join(" · ") || "暂无请求记录",
        );
        txt(
          "s-prefill-note",
          "处理 " + (j.prefill_seconds || 0).toFixed(1) + " s · 已剔除缓存命中",
        );
        txt(
          "s-output",
          "输出 " +
            fmtTok(j.output_tokens) +
            " token · " +
            (j.decode_seconds || 0).toFixed(1) +
            " s",
        );
        const ttft = j.ttft?.p50_ms;
        const tt = $("s-ttft");
        tt.replaceChildren();
        if (finite(ttft)) {
          const ms = ttft < 1000;
          tt.append(
            node("span", ms ? Math.round(ttft) : (ttft / 1000).toFixed(2)),
            node("small", ms ? "ms" : "s"),
          );
        } else tt.textContent = "—";
        txt(
          "s-p95",
          j.ttft
            ? "P95 " +
                fmtMs(j.ttft.p95_ms) +
                " · " +
                fmtTok(j.ttft.count) +
                " 条流式记录"
            : "暂无流式延迟记录",
        );
        const acc = $("reqs-acc");
        acc.replaceChildren();
        const entries = Object.entries(j.drafters || {});
        if (!entries.length) {
          acc.append(node("p", "暂无推测解码统计", "hint"));
          return;
        }
        for (const [name, d] of entries) {
          const line = node("div", null, "between");
          line.append(
            node(
              "span",
              name.toUpperCase() + (d.proposed > 0 ? " 接受率" : " 解码"),
            ),
            node(
              "strong",
              d.proposed > 0
                ? fmtPct(d.acceptance)
                : fmtRate(d.decode_tok_per_s) + " tok/s",
              "mono",
            ),
          );
          acc.append(line);
          if (d.proposed > 0) {
            const bar = node("div", null, "bar"),
              fill = node("i");
            fill.style.width =
              Math.max(0, Math.min(100, (d.acceptance || 0) * 100)) + "%";
            bar.append(fill);
            acc.append(bar);
          }
          acc.append(
            node(
              "p",
              (d.proposed > 0
                ? fmtTok(d.accepted) +
                  " / " +
                  fmtTok(d.proposed) +
                  " 草稿 token · "
                : "") +
                fmtTok(d.requests) +
                " 次请求",
              "hint",
            ),
          );
        }
      }
      function createTable(all) {
        const panel = node("section", null, "panel"),
          head = node("div", null, "panel-head request-head"),
          title = node("div");
        title.append(
          node("h2", all ? "请求记录" : "最近请求"),
          node(
            "p",
            all
              ? "最近 100 条内筛选 · 可展开查看完整指标"
              : "先看结果，再展开耗时与缓存详情",
          ),
        );
        head.append(title);
        const toolbar = node("div", null, "row");
        if (all) {
          const select = node("select");
          select.id = "reason-filter";
          select.setAttribute("aria-label", "筛选结束原因");
          for (const [v, label] of [
            ["all", "所有结果"],
            ...Object.entries(FINISH_ZH),
          ]) {
            const o = node("option", label);
            o.value = v;
            select.append(o);
          }
          select.value = reasonFilter;
          select.onchange = () => {
            reasonFilter = select.value;
            renderTables();
          };
          toolbar.append(select);
        } else {
          const link = node("a", "查看全部 →", "link-btn");
          link.href = "#requests";
          toolbar.append(link);
        }
        head.append(toolbar);
        panel.append(head);
        const wrap = node("div", null, "table-wrap"),
          table = node("table", null, "requests");
        const thead = node("thead"),
          hr = node("tr");
        for (const x of [
          "时间 / 类型（UTC）",
          "结束原因",
          "输入 → 输出",
          "首字延迟",
          "生成速度",
          "详情",
        ])
          hr.append(node("th", x));
        thead.append(hr);
        table.append(thead);
        const tb = node("tbody");
        const filtered = scopedRecords().filter(
            (r) => !all || reasonFilter === "all" || r.finish === reasonFilter,
          ),
          list = all ? filtered : filtered.slice(0, 5);
        if (!list.length) {
          const tr = node("tr"),
            td = node(
              "td",
              reqAt
                ? reasonFilter !== "all" && all
                  ? "当前没有此类结果，可切换“所有结果”查看。"
                  : "此日期范围没有请求。可调整日期，或到“对话测试”发送一次请求。"
                : "等待请求记录…",
              "empty",
            );
          td.colSpan = 6;
          tr.append(td);
          tb.append(tr);
        }
        for (const r of list) {
          const tr = node("tr"),
            time = node("td");
          time.append(
            node("span", utc(r.ts_ms).slice(5), "mono"),
            node(
              "span",
              String(r.drafter || "serial").toUpperCase() +
                (r.vision ? " · 图像" : ""),
              "r-id",
            ),
          );
          const reason = node("td");
          reason.append(reasonChip(r.finish));
          tr.append(
            time,
            reason,
            node(
              "td",
              fmtTok(r.prompt_tokens) + " → " + fmtTok(r.output_tokens),
              "mono",
            ),
            node("td", r.ttft_ms > 0 ? fmtMs(r.ttft_ms) : "—", "mono"),
            node(
              "td",
              fmtRate(decodeRate(r)) + (decodeRate(r) != null ? " tok/s" : ""),
              "mono",
            ),
          );
          const td = node("td"),
            btn = node("button", null, "detail-btn");
          btn.append(icon("chevron"));
          btn.setAttribute("aria-label", "查看请求 " + r.req_seq + " 详情");
          btn.dataset.recordKey = r.ts_ms + "-" + r.req_seq;
          btn.onclick = () => showDetail(r);
          td.append(btn);
          ["输入 → 输出", "首字延迟", "生成速度"].forEach(
            (label, i) => (tr.children[i + 2].dataset.label = label),
          );
          tr.append(td);
          tb.append(tr);
        }
        table.append(tb);
        wrap.append(table);
        panel.append(wrap);
        const foot = node("div", null, "table-footer");
        foot.append(
          node(
            "span",
            "显示 " +
              list.length +
              " 条 · 范围内最近记录 " +
              filtered.length +
              " 条",
          ),
          node("span", "长度上限 ≠ 执行失败"),
        );
        panel.append(foot);
        return panel;
      }
      function renderTables() {
        if ($("request-detail").open) return;
        const active = document.activeElement,
          filterFocused = active?.id === "reason-filter",
          recordKey = active?.dataset.recordKey;
        for (const [id, all] of [
          ["recent-table", false],
          ["all-table", true],
        ]) {
          const left = $(id).querySelector(".table-wrap")?.scrollLeft || 0;
          $(id).replaceChildren(createTable(all));
          $(id).querySelector(".table-wrap").scrollLeft = left;
        }
        if (filterFocused) $("reason-filter")?.focus();
        if (recordKey) {
          const container =
            page === "overview" ? $("recent-table") : $("all-table");
          Array.from(container.querySelectorAll("[data-record-key]"))
            .find((b) => b.dataset.recordKey === recordKey)
            ?.focus();
        }
      }
      function detailLine(label, value) {
        const line = node("div", null, "summary-line");
        line.append(node("span", label), node("strong", value, "mono"));
        return line;
      }
      function showDetail(r) {
        $("request-detail").dataset.returnKey =
          document.activeElement?.dataset.recordKey || "";
        $("request-detail").dataset.returnView = page;
        const body = $("detail-body");
        body.replaceChildren(reasonChip(r.finish));
        const rows = [
          ["时间（UTC）", utc(r.ts_ms)],
          ["请求序号 / 起草方式", r.req_seq + " / " + r.drafter],
          [
            "输入 / 缓存命中",
            fmtTok(r.prompt_tokens) + " / " + fmtTok(r.cached_tokens),
          ],
          ["输出 token", fmtTok(r.output_tokens)],
          ["首字延迟", r.ttft_ms > 0 ? fmtMs(r.ttft_ms) : "未记录"],
          [
            "Prefill",
            prefillRate(r) == null
              ? r.prompt_tokens > 0 && r.prompt_tokens === r.cached_tokens
                ? "全命中缓存"
                : "未记录"
              : fmtRate(prefillRate(r)) + " tok/s",
          ],
          ["Prefill 耗时", fmtMs(r.prefill_ms)],
          ["Decode", fmtRate(decodeRate(r)) + " tok/s"],
          ["Decode 耗时", fmtMs(r.decode_ms)],
          ["草稿接受率", fmtPct(r.acceptance)],
          [
            "草稿接受 / 提议",
            fmtTok(
              finite(r.commit) && finite(r.rounds)
                ? Math.max(0, r.commit - r.rounds)
                : null,
            ) +
              " / " +
              fmtTok(r.proposed),
          ],
          ["总提交 / 轮次", fmtTok(r.commit) + " / " + fmtTok(r.rounds)],
          ["图像请求", r.vision ? "是" : "否"],
        ];
        rows.forEach(([l, v]) => body.append(detailLine(l, v)));
        body.append(
          node("p", "记录包含运行指标，不包含提示词和回答正文。", "hint"),
        );
        $("request-detail").showModal();
      }
      function drawThroughput() {
        const cv = $("throughput");
        if (!cv.clientWidth) return;
        const dpr = devicePixelRatio || 1,
          W = cv.clientWidth,
          H = cv.clientHeight;
        cv.width = W * dpr;
        cv.height = H * dpr;
        const g = cv.getContext("2d");
        g.scale(dpr, dpr);
        const matched = scopedRecords().filter(chartMatches),
          measurable = matched.filter((r) => finite(
            chartMetric === "prefill" ? prefillRate(r) : decodeRate(r))),
          data = measurable
          .slice(0, 50)
          .reverse()
          .map((r) => ({
            t: r.ts_ms,
            v: chartMetric === "prefill" ? prefillRate(r) : decodeRate(r),
            r,
          }));
        const filtered = $("chart-size").value !== "all" || $("chart-cache").value !== "all",
          timeScale = $("chart-view").value === "time";
        txt("chart-context", chartMetric === "prefill"
          ? "实际新增 = 输入减去缓存命中。短输入的 tok/s 较低，不一定出首字更慢。"
          : "当前仍按新增输入和前缀缓存筛选；Decode = 输出 token ÷ 生成耗时。");
        txt("chart-axis-label", timeScale ? "tok/s · 时间 UTC" : "tok/s · 请求等距，非耗时");
        g.font = "12px Consolas, monospace";
        g.fillStyle = "#adb8ce";
        if (!data.length) {
          g.textAlign = "center";
          g.fillText(
            filtered ? "当前筛选无有效采样" : "暂无有效吞吐采样",
            W / 2,
            H / 2,
          );
          chartPoints = [];
          chartSelected = -1;
          const fullCache = chartMetric === "prefill" && matched.some((r) =>
            r.prompt_tokens > 0 && r.prompt_tokens === r.cached_tokens);
          txt("chart-readout", fullCache
            ? "全部命中缓存时没有新增输入，Prefill 速度不适用；可切换 Decode 查看。"
            : filtered ? "当前日期与最近记录中没有可计算的匹配请求，请调整筛选。"
            : "没有记录到可计算的 token 数与耗时；完成请求后可查看。");
          txt("chart-note", "范围：最近 " + records.length + " 条记录内的所选日期 · 匹配 " + matched.length + " 次");
          cv.setAttribute("aria-label", $("chart-readout").textContent);
          return;
        }
        const pad = { l: 48, r: 12, t: 13, b: 27 },
          max = Math.max(1, ...data.map((p) => p.v)) * 1.12,
          minT = data[0].t,
          maxT = data[data.length - 1].t;
        const x = (t, i) =>
            !timeScale
              ? pad.l +
                (W - pad.l - pad.r) *
                  (data.length === 1 ? 0.5 : i / (data.length - 1))
              : maxT === minT
                ? pad.l + (W - pad.l - pad.r) / 2
                : pad.l + ((t - minT) / (maxT - minT)) * (W - pad.l - pad.r),
          y = (v) => H - pad.b - (v / max) * (H - pad.t - pad.b);
        for (let i = 0; i <= 3; i++) {
          const value = (max * i) / 3,
            py = y(value);
          g.strokeStyle = "#30384d";
          g.beginPath();
          g.moveTo(pad.l, py);
          g.lineTo(W - pad.r, py);
          g.stroke();
          g.fillStyle = "#adb8ce";
          g.textAlign = "right";
          g.fillText(
            value >= 1000 ? (value / 1000).toFixed(1) + "k" : Math.round(value),
            pad.l - 8,
            py + 4,
          );
        }
        g.strokeStyle = "#d3fa69";
        g.lineWidth = 2.5;
        g.beginPath();
        data.forEach((p, i) =>
          i ? g.lineTo(x(p.t, i), y(p.v)) : g.moveTo(x(p.t, i), y(p.v)),
        );
        g.stroke();
        chartPoints = data.map((p, i) => ({ ...p, x: x(p.t, i), y: y(p.v) }));
        for (const p of chartPoints) {
          g.fillStyle = "#d3fa69";
          g.beginPath();
          g.arc(p.x, p.y, 3, 0, Math.PI * 2);
          g.fill();
        }
        g.fillStyle = "#adb8ce";
        g.textAlign = "left";
        g.fillText(timeScale ? utc(minT).slice(11) : "第 1 次", pad.l, H - 4);
        g.textAlign = "right";
        if (data.length > 1)
          g.fillText(
            timeScale ? utc(maxT).slice(11) : "第 " + data.length + " 次",
            W - pad.r,
            H - 4,
          );
        if (chartSelected >= data.length) chartSelected = data.length - 1;
        if (chartSelected >= 0) {
          const p = chartPoints[chartSelected];
          g.strokeStyle = "#adb8ce";
          g.setLineDash([3, 4]);
          g.beginPath();
          g.moveTo(p.x, pad.t);
          g.lineTo(p.x, H - pad.b);
          g.stroke();
          g.setLineDash([]);
          g.fillStyle = "#f1f4fc";
          g.beginPath();
          g.arc(p.x, p.y, 5, 0, Math.PI * 2);
          g.fill();
          txt(
            "chart-readout",
            utc(p.t) +
              " UTC · " +
              (chartMetric === "prefill" ? "Prefill " : "Decode ") +
              fmtRate(p.v) + " tok/s · 新增 " + fmtTok(p.r.prompt_tokens - p.r.cached_tokens) +
              " token · 缓存 " + (p.r.prompt_tokens > 0 ? (p.r.cached_tokens * 100 / p.r.prompt_tokens).toFixed(1) : "0.0") +
              "% · Prefill " + fmtMs(p.r.prefill_ms) + " · 首字 " + fmtMs(p.r.ttft_ms),
          );
        } else
          txt("chart-readout", "将鼠标移到采样点，或聚焦图表后按方向键查看。");
        txt("chart-note", "最近 " + records.length + " 条记录内筛选 · 显示最近 " + data.length +
          " / " + measurable.length + " 次有效采样 · 未做平滑");
        cv.setAttribute(
          "aria-label",
          (chartMetric === "prefill" ? "Prefill" : "Decode") +
            " 最近 " +
            data.length +
            " 次请求，每秒 token 范围 " +
            fmtRate(Math.min(...data.map((p) => p.v))) +
            " 至 " +
            fmtRate(Math.max(...data.map((p) => p.v))) +
            "；详细值见请求记录",
        );
      }
      for (const id of ["chart-view", "chart-size", "chart-cache"]) $(id).onchange = () => {
        chartSelected = -1;
        drawThroughput();
      };
      $("throughput").addEventListener("pointermove", (e) => {
        if (!chartPoints.length) return;
        const x = e.clientX - $("throughput").getBoundingClientRect().left;
        const next = chartPoints.reduce(
          (best, p, i) =>
            Math.abs(p.x - x) < Math.abs(chartPoints[best].x - x) ? i : best,
          0,
        );
        if (next !== chartSelected) {
          chartSelected = next;
          drawThroughput();
        }
      });
      $("throughput").addEventListener("pointerleave", () => {
        if (document.activeElement !== $("throughput")) {
          chartSelected = -1;
          drawThroughput();
        }
      });
      $("throughput").addEventListener("keydown", (e) => {
        if (
          !["ArrowLeft", "ArrowRight", "Home", "End"].includes(e.key) ||
          !chartPoints.length
        )
          return;
        e.preventDefault();
        chartSelected =
          e.key === "Home"
            ? 0
            : e.key === "End"
              ? chartPoints.length - 1
              : Math.max(
                  0,
                  Math.min(
                    chartPoints.length - 1,
                    chartSelected + (e.key === "ArrowRight" ? 1 : -1),
                  ),
                );
        drawThroughput();
      });
      async function tickMem() {
        if (!pollingAllowed() || pending.memory || page !== "memory") return;
        if (
          !healthAt ||
          Date.now() - healthAt > 6000 ||
          health.busy ||
          health.in_flight > 0 ||
          health.queued > 0 ||
          chatAbort
        ) {
          txt("mem-meta", "等待 API 空闲后采样");
          $("mem-data").classList.add("stale");
          return;
        }
        pending.memory = true;
        try {
          const j = await grab("/memory");
          renderMem(j);
          memAt = Date.now();
          $("mem-data").classList.remove("stale");
          hideErr("mem-err");
          txt("mem-meta", "采样于 " + hhmmss(memAt));
        } catch (e) {
          $("mem-data").classList.add("stale");
          showErr("mem-err", e.message + " · 保留上次采样，将自动重试");
        } finally {
          pending.memory = false;
          updateRefreshMeta();
        }
      }
      const OVR_FIELDS = [
        {
          k: "enable_thinking",
          label: "思考开关",
          type: "bool",
          def: true,
          note: "关 = 模板关闭思考，不输出 reasoning_content；客户端不传时为开",
        },
        {
          k: "reasoning_effort",
          label: "思考强度",
          type: "effort",
          def: "xhigh",
          note: "low / medium / xhigh（客户端传 high→xhigh，minimal→low）；仅思考开启时有效",
        },
        {
          k: "preserve_thinking",
          label: "保留历史思考",
          type: "bool",
          def: true,
          note: "多轮时历史 assistant 的思考是否放回提示词；不传时保留",
        },
        {
          k: "temperature",
          label: "temperature",
          type: "num",
          min: 0,
          max: 2,
          def: 1.0,
          note: "0 = 贪心；客户端不传时为 1.0。强制 0 时会去掉请求里的 logprobs",
        },
        {
          k: "top_p",
          label: "top_p",
          type: "num",
          min: 0,
          max: 1,
          def: 0.95,
          note: "客户端不传时为 0.95",
        },
        {
          k: "top_k",
          label: "top_k",
          type: "int",
          min: 0,
          max: 2147483647,
          step: 1,
          def: 20,
          note: "0 = 不限；客户端不传时为 20",
        },
        {
          k: "min_p",
          label: "min_p",
          type: "num",
          min: 0,
          max: 1,
          def: 0,
          note: "客户端不传时为 0",
        },
        {
          k: "presence_penalty",
          label: "presence_penalty",
          type: "num",
          min: -2,
          max: 2,
          def: 0,
          note: "-2 ~ 2",
        },
        {
          k: "frequency_penalty",
          label: "frequency_penalty",
          type: "num",
          min: -2,
          max: 2,
          def: 0,
          note: "-2 ~ 2",
        },
        {
          k: "max_tokens",
          label: "max_tokens",
          type: "int",
          min: 1,
          max: 2147483647,
          step: 1,
          def: 32768,
          note: "默认值 = 客户端没给输出上限时用它；强制 = 输出上限，客户端给得更大时压到此值",
        },
      ];

      function ovrValueInput(f) {
        if (f.type === "bool") {
          const s = document.createElement("select");
          s.innerHTML =
            '<option value="true">开 (true)</option><option value="false">关 (false)</option>';
          return s;
        }
        if (f.type === "effort") {
          const s = document.createElement("select");
          s.innerHTML =
            "<option>low</option><option>medium</option><option>xhigh</option>";
          return s;
        }
        const i = document.createElement("input");
        i.type = "number";
        i.min = f.min;
        i.max = f.max;
        i.step = f.type === "int" ? "1" : "any";
        return i;
      }

      function buildOvrTable() {
        const tb = $("ovr-body");
        tb.innerHTML = "";
        for (const f of OVR_FIELDS) {
          const tr = document.createElement("tr");
          tr.id = "ovr-row-" + f.k;
          const name = document.createElement("td");
          name.textContent = f.label;
          if (f.label !== f.k) name.append(node("span", f.k, "param-key"));
          if (f.label !== f.k) name.title = f.k;
          const modeTd = document.createElement("td");
          modeTd.dataset.label = "覆盖模式";
          const mode = document.createElement("select");
          mode.innerHTML =
            '<option value="client">跟随客户端</option><option value="default">默认值</option>' +
            '<option value="force">强制</option>';
          mode.setAttribute("aria-label", f.label + "覆盖模式");
          if (f.k === "max_tokens") mode.options[2].textContent = "强制上限";
          modeTd.appendChild(mode);
          const valTd = document.createElement("td");
          valTd.dataset.label = "参数值";
          const val = ovrValueInput(f);
          val.id = "ovr-value-" + f.k;
          val.setAttribute("aria-label", f.label + "值");
          valTd.appendChild(val);
          const error = node("span", "", "field-error");
          error.id = "error-" + f.k;
          error.hidden = true;
          valTd.appendChild(error);
          const note = document.createElement("td");
          note.className = "note";
          note.textContent = f.note;
          note.id = "note-" + f.k;
          val.setAttribute("aria-describedby", note.id);
          tr.append(name, modeTd, valTd, note);
          tb.appendChild(tr);
          f.modeEl = mode;
          f.valEl = val;
          f.row = tr;
          f.errorEl = error;
          mode.addEventListener("change", () => {
            syncRow(f);
            revalidateOvrField(f);
            markDirty();
          });
          val.addEventListener("input", () => {
            revalidateOvrField(f);
            markDirty();
          });
          val.addEventListener("change", markDirty);
        }
      }

      function setVal(f, v) {
        f.valEl.value = String(v);
      }
      function getVal(f) {
        const raw = f.valEl.value;
        if (f.type === "bool") return raw === "true";
        if (f.type === "effort") return raw;
        if (raw.trim() === "") throw new Error(f.k + " 需要填写数值");
        const n = Number(raw);
        if (!isFinite(n)) throw new Error(f.k + " 不是有效数字");
        if (f.type === "int" && !Number.isInteger(n))
          throw new Error(f.k + " 必须是整数");
        if (n < f.min || n > f.max)
          throw new Error(f.k + " 超出范围 " + f.min + " ~ " + f.max);
        return n;
      }
      function syncRow(f) {
        const m = f.modeEl.value;
        f.valEl.disabled = m === "client";
        f.row.className = m === "force" ? "on" : m === "default" ? "def" : "";
      }

      let ovrDirty = false,
        ovrLoaded = false,
        ovrPending = false;
      function setOvrFieldError(f, message = "") {
        f.errorEl.textContent = message;
        f.errorEl.hidden = !message;
        if (message) f.valEl.setAttribute("aria-invalid", "true");
        else f.valEl.removeAttribute("aria-invalid");
        f.valEl.setAttribute(
          "aria-describedby",
          "note-" + f.k + (message ? " " + f.errorEl.id : ""),
        );
      }
      function refreshOvrErrors() {
        const errors = OVR_FIELDS.filter((f) => !f.errorEl.hidden);
        if (errors.length) {
          $("ovr-err").dataset.kind = "validation";
          showErr("ovr-err", errors[0].errorEl.textContent +
            (errors.length > 1 ? "；共 " + errors.length + " 项需要修正" : ""));
        } else if ($("ovr-err").dataset.kind === "validation") {
          hideErr("ovr-err");
          delete $("ovr-err").dataset.kind;
        }
        return errors;
      }
      function revalidateOvrField(f) {
        if (f.errorEl.hidden) return;
        let message = "";
        try { if (f.modeEl.value !== "client") getVal(f); }
        catch (e) { message = e.message; }
        setOvrFieldError(f, message);
        refreshOvrErrors();
      }
      function clearOvrErrors() {
        for (const f of OVR_FIELDS) setOvrFieldError(f);
        $("ovr-key").removeAttribute("aria-invalid");
        $("ovr-key-error").hidden = true;
        $("ovr-key-error").textContent = "";
        hideErr("ovr-err");
        delete $("ovr-err").dataset.kind;
      }
      function restoreOvrFocus(origin, target = origin) {
        if (page === "settings" && target &&
            (document.activeElement === document.body || document.activeElement === origin)) {
          target.focus();
          requestAnimationFrame(updateSettingsViewport);
        }
      }
      function updateSettingsViewport() {
        const bar = $("ovr-save").closest(".save-bar");
        const sidebar = document.querySelector(".sidebar");
        const top = getComputedStyle(sidebar).position === "sticky"
          ? sidebar.getBoundingClientRect().height : 0;
        // A tall toolbar must scroll normally instead of covering the editor.
        bar.classList.toggle("is-tall", bar.offsetHeight > (innerHeight - top) / 2);
        const sticky = page === "settings" && getComputedStyle(bar).position === "sticky";
        document.documentElement.style.scrollPaddingTop = top + 12 + "px";
        document.documentElement.style.scrollPaddingBottom = (sticky ? bar.offsetHeight + 12 : 12) + "px";
        const active = document.activeElement;
        if (page !== "settings" || !$("settings").contains(active)) return;
        const box = (active.closest("td") || active).getBoundingClientRect();
        const bottom = sticky && !bar.contains(active)
          ? Math.min(innerHeight, bar.getBoundingClientRect().top) - 12
          : innerHeight - 12;
        if (box.top < top + 12 || box.bottom > bottom) {
          const available = bottom - top - 12;
          const y = top + 12 + Math.max(0, (available - box.height) / 2);
          window.scrollBy({ top: box.top - y, behavior: "instant" });
        }
      }
      function lockOvr(b) {
        ovrPending = b;
        $("ovr-save").textContent = b ? "正在处理…" : "保存并立即生效";
        $("ovr-body").setAttribute("aria-busy", String(b));
        for (const f of OVR_FIELDS) {
          f.modeEl.disabled = b || !ovrLoaded;
          f.valEl.disabled = b || !ovrLoaded || f.modeEl.value === "client";
        }
        $("ovr-save").disabled = b || !ovrLoaded;
        $("ovr-clear").disabled = b || !ovrLoaded;
        $("ovr-reload").disabled = b;
      }
      function markDirty() {
        ovrDirty = true;
        $("ovr-save").closest(".save-bar").classList.add("is-dirty");
        $("ovr-status").textContent = "有未保存的修改";
      }

      function fillOvr(state) {
        if (
          !state.overrides ||
          typeof state.overrides !== "object" ||
          Array.isArray(state.overrides)
        )
          throw new Error("参数覆盖响应格式无效");
        const t = state.overrides;
        ovrLoaded = true;
        txt(
          "ovr-count",
          Object.keys(t).length
            ? Object.keys(t).length + " 项覆盖生效"
            : "跟随客户端",
        );
        for (const f of OVR_FIELDS) {
          const e = t[f.k];
          f.modeEl.value = e ? e.mode : "client";
          setVal(f, e ? e.value : f.def);
          syncRow(f);
        }
        $("ovr-key-field").hidden = !state.admin_key_required;
        $("ovr-meta").textContent = state.persist_path
          ? "持久化文件：" + state.persist_path
          : "仅内存（未持久化）";
        ovrDirty = false;
        $("ovr-save").closest(".save-bar").classList.remove("is-dirty");
      }

      async function loadOvr() {
        if (ovrPending) return;
        const origin = document.activeElement;
        let focusTarget = origin;
        lockOvr(true);
        try {
          fillOvr(await grab("/admin/overrides"));
          clearOvrErrors();
          $("ovr-status").textContent = "已读取 " + hhmmss();
        } catch (e) {
          showErr("ovr-err", "读取失败：" + e.message);
          focusTarget = $("ovr-err");
        } finally {
          lockOvr(false);
          restoreOvrFocus(origin, focusTarget);
        }
      }

      function collectOvr() {
        const t = {};
        clearOvrErrors();
        for (const f of OVR_FIELDS) {
          const m = f.modeEl.value;
          if (m === "client") continue;
          try { t[f.k] = { mode: m, value: getVal(f) }; }
          catch (e) { setOvrFieldError(f, e.message); }
        }
        const errors = refreshOvrErrors();
        if (errors.length) {
          errors[0].valEl.focus();
          requestAnimationFrame(updateSettingsViewport);
          return null;
        }
        return t;
      }

      async function saveOvr(table) {
        if (ovrPending || !ovrLoaded) return;
        const origin = document.activeElement;
        let focusTarget = origin;
        clearOvrErrors();
        lockOvr(true);
        const headers = { "Content-Type": "application/json" };
        const key = $("ovr-key").value;
        if (key) headers["X-Admin-Key"] = key;
        try {
          const j = await grab("/admin/overrides", {
            method: "POST",
            headers,
            body: JSON.stringify(table),
          });
          fillOvr(j);
          hideErr("ovr-err");
          $("ovr-status").textContent =
            (j.saved ? "已保存并生效 " : "已生效，但未能写入文件 ") + hhmmss();
          if (!j.saved) {
            showErr(
              "ovr-err",
              "持久化失败：" + (j.save_error || "?") + "（重启后会丢失）",
            );
            focusTarget = $("ovr-err");
          }
          tickHealth();
        } catch (e) {
          if (e.status === 401) {
            $("ovr-key-field").hidden = false;
            $("ovr-key").setAttribute("aria-invalid", "true");
            $("ovr-key-error").textContent = "请检查管理密钥后重新保存。";
            $("ovr-key-error").hidden = false;
            showErr("ovr-err", "保存失败：" + e.message);
            focusTarget = $("ovr-key");
          } else {
            showErr("ovr-err", "保存未确认：" + e.message + "；请重新读取确认服务端实际值");
            focusTarget = $("ovr-err");
          }
        } finally {
          lockOvr(false);
          restoreOvrFocus(origin, focusTarget);
        }
      }

      $("ovr-save").addEventListener("click", () => {
        const t = collectOvr();
        if (t) saveOvr(t);
      });
      $("ovr-reload").addEventListener("click", () => {
        if (ovrDirty && !confirm("丢弃未保存的修改？")) return;
        loadOvr();
      });
      $("ovr-clear").addEventListener("click", () => {
        if (!confirm("清空服务端覆盖表，所有参数改为跟随客户端？")) return;
        saveOvr({});
      });

      function memCell(k, v, sub) {
        return (
          '<div class="cell"><div class="k">' +
          k +
          '</div><div class="v mono">' +
          v +
          '</div><div class="s">' +
          (sub || "") +
          "</div></div>"
        );
      }

      function renderMem(j) {
        const f = (n) => (n == null ? null : Number(n));
        const committed = f(j.gpu_accessible_committed_bytes);
        $("m-hero").textContent = fmtBytes(committed);
        $("m-hero").title =
          "gpu_accessible_committed_bytes = " +
          (j.gpu_accessible_committed_bytes ?? "-") +
          " 字节";
        $("m-accounting").textContent = j.accounting || "";

        const total = f(j.hip_total_bytes),
          free = f(j.hip_free_bytes);
        if (total != null && free != null) {
          const used = total - free,
            pct = total ? (used / total) * 100 : 0;
          $("m-hip-bar").style.width =
            Math.min(100, Math.max(0, pct)).toFixed(1) + "%";
          $("m-hip-pct").textContent = pct.toFixed(1) + "%";
          $("m-hip-used").textContent = "已用 " + fmtBytes(used);
          $("m-hip-total").textContent = "共 " + fmtBytes(total);
        } else {
          $("m-hip-bar").style.width = "0%";
          ["m-hip-pct", "m-hip-used", "m-hip-total"].forEach((id) =>
            txt(id, "未报告"),
          );
        }

        const peak = (c, p) => "峰值 " + fmtBytes(f(p));
        $("mem-grid").innerHTML =
          memCell(
            "设备分配 (HIP device)",
            fmtBytes(f(j.device_current_bytes)),
            peak(0, j.device_peak_bytes),
          ) +
          memCell(
            "注册 mmap（专家权重）",
            fmtBytes(f(j.registered_mmap_bytes)),
            "mmap + hipHostRegister",
          ) +
          memCell(
            "Pinned host",
            fmtBytes(f(j.pinned_host_current_bytes)),
            peak(0, j.pinned_host_peak_bytes),
          ) +
          memCell(
            "启动以来 HIP 增量",
            fmtBytes(f(j.hip_used_since_engine_start_bytes)),
            "引擎自身驱动",
          ) +
          memCell(
            "进程 RSS",
            fmtBytes(f(j.process_rss_bytes)),
            "含可回收 mmap 页缓存",
          ) +
          memCell("进程已锁定", fmtBytes(f(j.process_locked_bytes)), "");
        $("raw-mem").textContent = JSON.stringify(j, null, 1);

        hist.committed.push(committed);
        hist.rss.push(f(j.process_rss_bytes));
        if (hist.committed.length > HIST_CAP) {
          hist.committed.shift();
          hist.rss.shift();
        }
        drawSpark();
      }

      function drawSpark() {
        const cv = $("spark");
        if (!cv.clientWidth) return;
        const dpr = window.devicePixelRatio || 1;
        const W = cv.clientWidth * dpr,
          H = cv.clientHeight * dpr;
        cv.width = W;
        cv.height = H;
        const g = cv.getContext("2d");
        g.clearRect(0, 0, W, H);
        const series = [
          { data: hist.committed, color: "#d3fa69" },
          { data: hist.rss, color: "#b8a4ff" },
        ];
        let max = 0;
        for (const s of series)
          for (const v of s.data) if (v != null && v > max) max = v;
        if (max <= 0) return;
        const pad = 6 * dpr,
          n = HIST_CAP;
        const x = (i) => pad + (W - 2 * pad) * (i / (n - 1));
        const y = (v) => H - pad - (H - 2 * pad) * (v / max);
        g.strokeStyle = "#30384d";
        g.lineWidth = 1;
        g.strokeRect(pad, pad, W - 2 * pad, H - 2 * pad);
        for (const s of series) {
          g.strokeStyle = s.color;
          g.lineWidth = 1.5 * dpr;
          g.beginPath();
          let started = false;
          s.data.forEach((v, i) => {
            if (v == null) {
              started = false;
              return;
            }
            if (!started) {
              g.moveTo(x(i), y(v));
              started = true;
            } else g.lineTo(x(i), y(v));
          });
          g.stroke();
        }
        g.fillStyle = "#adb8ce";
        g.font = 10 * dpr + "px Consolas, monospace";
        g.fillText(fmtBytes(max), pad + 4 * dpr, pad + 12 * dpr);
      }

      // Chat state survives navigation; only complete turns enter the next request.
      let convo = [],
        chatAbort = null;
      function msgBox(role) {
        $("chat-empty").hidden = true;
        const box = node("div", null, "msg " + role);
        box.append(node("div", role === "user" ? "你" : "模型", "role"));
        $("c-log").append(box);
        return box;
      }
      function addPart(box, cls, text) {
        const part = node("div", text || "", cls);
        box.append(part);
        return part;
      }
      function buildChatBody(text) {
        const messages = [],
          system = $("c-system").value.trim();
        if (system) messages.push({ role: "system", content: system });
        const body = {
          messages: messages.concat(convo, { role: "user", content: text }),
          stream: true,
          stream_options: { include_usage: true },
        };
        if ($("c-think").value)
          body.enable_thinking = $("c-think").value === "true";
        if ($("c-effort").value) body.reasoning_effort = $("c-effort").value;
        const nums = {
          temperature: "c-temp",
          top_p: "c-topp",
          top_k: "c-topk",
          min_p: "c-minp",
          presence_penalty: "c-pres",
          max_tokens: "c-max",
          seed: "c-seed",
        };
        for (const [k, id] of Object.entries(nums)) {
          const el = $(id),
            v = el.value.trim();
          if (el.validity.badInput || (v && !el.checkValidity()))
            throw new Error(k + " 取值无效，请检查范围与步长");
          if (v) body[k] = Number(v);
        }
        return body;
      }
      function setBusy(b) {
        $("c-send").disabled = b;
        $("c-stop").disabled = !b;
        $("c-clear").disabled = b;
      }
      async function sendChat() {
        const text = $("c-input").value;
        if (!text.trim() || chatAbort) return;
        let body;
        try {
          body = buildChatBody(text);
        } catch (e) {
          showErr("chat-err", e.message);
          return;
        }
        hideErr("chat-err");
        addPart(msgBox("user"), "body", text);
        $("c-input").value = "";
        const box = msgBox("assistant"),
          note = addPart(box, "ovr-note", ""),
          think = addPart(box, "think", ""),
          ans = addPart(box, "body", ""),
          stat = addPart(box, "stat", "等待中…");
        think.hidden = true;
        let reasoning = "",
          content = "",
          usage = null,
          finish = null,
          terminal = false,
          success = false,
          reader = null;
        const t0 = performance.now();
        let tFirst = 0;
        chatAbort = new AbortController();
        setBusy(true);
        txt("chat-status", "生成中…");
        function frame(raw) {
          const payload = raw
            .split(/\r?\n/)
            .filter((l) => l.startsWith("data:"))
            .map((l) => l.slice(5).trimStart())
            .join("\n")
            .trim();
          if (!payload) return;
          if (payload === "[DONE]") {
            terminal = true;
            return;
          }
          let j;
          try {
            j = JSON.parse(payload);
          } catch {
            throw new Error("流式响应包含无效 JSON");
          }
          if (j.error) throw new Error(j.error.message || "流式响应错误");
          if (j.usage) usage = j.usage;
          const ch = j.choices?.[0];
          if (!ch) return;
          if (ch.finish_reason) finish = ch.finish_reason;
          const d = ch.delta || {};
          const nearEnd =
            $("c-log").scrollHeight -
              $("c-log").scrollTop -
              $("c-log").clientHeight <
            80;
          if (d.reasoning_content) {
            if (!tFirst) tFirst = performance.now();
            reasoning += d.reasoning_content;
            think.textContent = reasoning;
            think.hidden = false;
          }
          if (d.content) {
            if (!tFirst) tFirst = performance.now();
            content += d.content;
            ans.textContent = content;
          }
          if (tFirst)
            stat.textContent =
              "生成中… " + ((performance.now() - t0) / 1000).toFixed(1) + " s";
          if (nearEnd) $("c-log").scrollTop = $("c-log").scrollHeight;
        }
        try {
          const response = await fetch("/v1/chat/completions", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(body),
            signal: chatAbort.signal,
          });
          const ov = response.headers.get("X-Gdec-Overrides");
          note.textContent = ov ? "服务端覆盖：" + ov : "";
          if (!response.ok) {
            let j;
            try {
              j = await response.json();
            } catch {}
            throw new Error(errText(j, response.status));
          }
          reader = response.body.getReader();
          const dec = new TextDecoder();
          let buf = "";
          function consume() {
            let match;
            while ((match = /\r?\n\r?\n/.exec(buf))) {
              const raw = buf.slice(0, match.index);
              buf = buf.slice(match.index + match[0].length);
              frame(raw);
              if (terminal) break;
            }
          }
          while (!terminal) {
            const { value, done } = await reader.read();
            if (done) {
              buf += dec.decode();
              consume();
              if (buf.trim() && !terminal) frame(buf);
              break;
            }
            buf += dec.decode(value, { stream: true });
            consume();
          }
          if (!terminal && !finish)
            throw new Error(
              "响应流提前结束，未收到结束标记；此轮未加入后续对话上下文",
            );
          success = true;
          txt("chat-status", "完成 " + hhmmss());
        } catch (e) {
          if (e.name === "AbortError") {
            finish = "已停止";
            txt("chat-status", "已停止");
          } else {
            finish = "未完成";
            showErr("chat-err", e.message);
            txt("chat-status", "出错 · 可重试");
          }
        } finally {
          if (reader) {
            try {
              await reader.cancel();
            } catch {}
            reader.releaseLock();
          }
          chatAbort = null;
          setBusy(false);
          tickHealth();
          tickReqs();
        }
        const parts = ["结束：" + (FINISH_ZH[finish] || finish || "完成")];
        if (tFirst) parts.push("浏览器首 token " + fmtMs(tFirst - t0));
        parts.push(
          "总耗时 " + ((performance.now() - t0) / 1000).toFixed(2) + " s",
        );
        if (usage)
          parts.push(
            "输入 " +
              fmtTok(usage.prompt_tokens) +
              " / 输出 " +
              fmtTok(usage.completion_tokens) +
              " token",
          );
        if (!success) parts.push("此轮未加入后续上下文");
        stat.textContent = parts.join(" · ");
        if (success && (content || reasoning)) {
          convo.push({ role: "user", content: text });
          const msg = { role: "assistant", content };
          if (reasoning) msg.reasoning_content = reasoning;
          convo.push(msg);
        }
        if (!success && !$("c-input").value) $("c-input").value = text;
      }
      $("c-send").onclick = sendChat;
      $("c-stop").onclick = () => chatAbort?.abort();
      $("c-clear").onclick = () => {
        convo = [];
        $("c-log").replaceChildren();
        $("chat-empty").hidden = false;
        hideErr("chat-err");
        txt("chat-status", "");
      };
      $("c-input").addEventListener("keydown", (e) => {
        if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) {
          e.preventDefault();
          sendChat();
        }
      });

      document.querySelector(".skip").addEventListener("click", (e) => {
        e.preventDefault();
        $("content").focus();
        $("content").scrollIntoView({ block: "start" });
      });
      $("settings").addEventListener("focusin", () => requestAnimationFrame(updateSettingsViewport));
      const settingsObserver = new ResizeObserver(() => requestAnimationFrame(updateSettingsViewport));
      settingsObserver.observe($("ovr-save").closest(".save-bar"));
      settingsObserver.observe(document.querySelector(".sidebar"));
      window.addEventListener("hashchange", () => {
        activatePage();
        document
          .querySelector('[data-page="' + page + '"]')
          .scrollIntoView({ block: "nearest", inline: "nearest" });
        $("content").focus({ preventScroll: true });
      });
      window.addEventListener("resize", () => {
        requestAnimationFrame(updateSettingsViewport);
        drawThroughput();
        if (page === "memory") drawSpark();
      });
      document.querySelectorAll("[data-chart]").forEach(
        (b) =>
          (b.onclick = () => {
            chartMetric = b.dataset.chart;
            document
              .querySelectorAll("[data-chart]")
              .forEach((x) => x.setAttribute("aria-pressed", String(x === b)));
            drawThroughput();
          }),
      );
      $("detail-close").onclick = () => $("request-detail").close();
      $("request-detail").addEventListener("close", () => {
        renderTables();
        const d = $("request-detail"),
          container =
            d.dataset.returnView === "overview"
              ? $("recent-table")
              : $("all-table");
        Array.from(container.querySelectorAll("[data-record-key]"))
          .find((b) => b.dataset.recordKey === d.dataset.returnKey)
          ?.focus({ preventScroll: true });
      });
      $("request-detail").addEventListener("click", (e) => {
        if (
          e.target === $("request-detail") &&
          e.clientX < $("request-detail").getBoundingClientRect().left
        )
          $("request-detail").close();
      });
      $("reqs-range").onchange = () => {
        renderTables();
        drawThroughput();
        $("reqs-grid").classList.add("stale");
        tickReqs(true);
      };
      $("pause").onclick = () => {
        paused = !paused;
        $("pause").setAttribute("aria-pressed", String(paused));
        $("pause")
          .querySelector("[data-icon]")
          .replaceChildren(icon(paused ? "play" : "pause"));
        $("pause").querySelector("span").textContent = paused
          ? "继续刷新"
          : "暂停刷新";
        updateRefreshMeta();
        if (!paused) {
          tickHealth().then(tickMem);
          tickReqs();
        }
      };
      $("copy-api").onclick = async () => {
        const url = location.origin + "/v1";
        try {
          await navigator.clipboard.writeText(url);
          toast("API 地址已复制");
        } catch {
          toast("API 地址：" + url);
        }
      };
      document.addEventListener("visibilitychange", () => {
        updateRefreshMeta();
        if (!document.hidden && !paused) {
          tickHealth().then(tickMem);
          tickReqs();
        }
      });
      window.addEventListener("beforeunload", (e) => {
        if (ovrDirty || chatAbort) {
          e.preventDefault();
          e.returnValue = "";
        }
      });
      txt("target", location.host);
      buildOvrTable();
      renderTables();
      activatePage();
      tickHealth();
      tickReqs();
      setInterval(tickHealth, HEALTH_MS);
      setInterval(tickReqs, REQ_MS);
      setInterval(tickMem, MEM_MS);
    </script>
  </body>
</html>
)HTMLDOC";
