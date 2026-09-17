#!/usr/bin/env python3
"""NewsBoard 行情桥 —— 用 yfinance 取数据，输出 JSON 到 stdout。

用法：
  yf_fetch.py quotes SYM [SYM...]              报价（批量、并发；带盘前/盘中/盘后时段判定）
  yf_fetch.py spark  SYM [SYM...]              迷你走势（5 天 / 1 小时收盘序列）
  yf_fetch.py history SYM RANGE INTERVAL       历史 K 线（range=1mo/3mo/6mo/1y/5y，interval=1d/1wk/1mo）

设计要点：
  * 报价用 fast_info（每次只发一个轻量请求），比 ticker.info 快一个数量级；
    公司名从 history 的 meta 里取（同一个响应里就带 longName），不额外发请求。
  * 用线程池并发，30 个标的实测数秒内完成（对比 yf.download 串行要 20+ 秒）。
  * 任何单个标的失败都不影响整体，只是该条不出现在结果里。
  * 输出保证是单个 JSON 对象/数组，日志一律走 stderr，避免污染 stdout。
"""
import json
import re
import sys
import time
import warnings
from concurrent.futures import ThreadPoolExecutor

warnings.filterwarnings("ignore")

try:
    import pandas as pd
    import yfinance as yf
except ImportError as e:  # pragma: no cover
    print(json.dumps({"error": f"yfinance/pandas not installed: {e}"}))
    sys.exit(1)

MAX_WORKERS = 8


def _emit(obj):
    sys.stdout.write(json.dumps(obj, ensure_ascii=False, default=str))
    sys.stdout.write("\n")
    sys.stdout.flush()


def _safe_float(v):
    try:
        if v is None:
            return None
        f = float(v)
        if f != f:  # NaN
            return None
        return f
    except Exception:
        return None


SESSION_LABEL = {"pre": "盘前", "regular": "盘中", "post": "盘后", "closed": "收盘"}


def _session_windows(t):
    """从 history_metadata 取交易所自己公布的三个时段窗口（带交易所时区，
    所以夏令时、半日市都自动正确，不需要我们硬编码开盘时间）。"""
    md = t.history_metadata or {}
    ctp = md.get("currentTradingPeriod") or {}
    out = {}
    for k in ("pre", "regular", "post"):
        w = ctp.get(k) or {}
        s, e = w.get("start"), w.get("end")
        if s is not None and e is not None and s < e:
            out[k] = (s, e)
    return md, out


def _last_bar(frame, win):
    """窗口内最后一根 K 线 → (收盘价, 该根的时间戳)；窗口为空返回 (None, None)"""
    if win is None or frame is None or len(frame) == 0:
        return None, None
    s, e = win
    m = frame.loc[(frame.index >= s) & (frame.index < e)]
    if not len(m):
        return None, None
    return float(m["Close"].iloc[-1]), m.index[-1].timestamp()


def quote_one(symbol: str):
    """返回单个标的报价，带「当前/最近时段」判定。

    时段判定 = 本机时钟的绝对时刻 × 交易所自己公布的时间窗（currentTradingPeriod，
    带交易所时区）→ 夏令时、半日市自动正确，本机 +08:00 也不影响美股判断。

    显示口径：
      · 有进行中的时段   → 显示该时段价格与涨跌幅，标签着色（正在交易）
      · 没有进行中的时段 → 显示**最近一个有数据的时段**（通常是刚结束的盘后），
                           标签同色但置灰（表示已收盘），另外用数据时间戳说明新鲜度
    """
    try:
        t = yf.Ticker(symbol)
        md, windows = _session_windows(t)

        # 含盘前盘后的 1 分钟线：一次请求拿到三个时段的价格
        h = t.history(period="1d", interval="1m", prepost=True)
        if h is None or len(h) == 0:
            return None

        # 现在落在哪个窗口里
        active_session = None
        now = None
        for k in ("pre", "regular", "post"):
            w = windows.get(k)
            if not w:
                continue
            if now is None:
                now = pd.Timestamp.now(tz=w[0].tz)
            if w[0] <= now < w[1]:
                active_session = k
                break

        pre, pre_ts = _last_bar(h, windows.get("pre"))
        reg, reg_ts = _last_bar(h, windows.get("regular"))
        post, post_ts = _last_bar(h, windows.get("post"))
        prev_close = md.get("previousClose") or md.get("chartPreviousClose")

        # ── 选价：进行中优先，否则取最近有数据的时段 ──
        price = ref = None
        data_ts = None
        label = "收盘"

        if active_session == "pre":
            price, ref, data_ts, label = pre, prev_close, pre_ts, "盘前"
        elif active_session == "regular":
            price, ref, data_ts, label = reg, prev_close, reg_ts, "盘中"
        elif active_session == "post":
            price, ref, data_ts, label = post, (reg if reg is not None else prev_close), post_ts, "盘后"
        elif post is not None:
            price, ref, data_ts, label = post, (reg if reg is not None else prev_close), post_ts, "盘后"
        elif reg is not None:
            price, ref, data_ts, label = reg, prev_close, reg_ts, "收盘"
        elif pre is not None:
            price, ref, data_ts, label = pre, prev_close, pre_ts, "盘前"

        if price is None and len(h):
            price, data_ts, label = float(h["Close"].iloc[-1]), h.index[-1].timestamp(), "收盘"

        if price is None:
            return None
        if ref is None:
            ref = prev_close

        change = change_pct = None
        if ref:
            change = price - ref
            change_pct = change / ref * 100.0

        # 当日正式时段的高/低/成交量（悬浮提示用）
        reg_frame = h
        if windows.get("regular"):
            s0, e0 = windows["regular"]
            m = h.loc[(h.index >= s0) & (h.index < e0)]
            if len(m):
                reg_frame = m

        return {
            "symbol": symbol,
            "name": md.get("longName") or md.get("shortName") or symbol,
            "session": (active_session or label_to_session(label)),
            "session_label": label,
            "session_active": active_session is not None,
            "price": price,
            "reference_close": ref,
            "change": change,
            "change_percent": change_pct,
            "previous_close": prev_close,
            "pre_price": pre,
            "regular_price": reg,
            "post_price": post,
            "day_high": float(reg_frame["High"].max()),
            "day_low": float(reg_frame["Low"].min()),
            "volume": float(reg_frame["Volume"].sum()),
            "data_time": data_ts,
            "exchange": md.get("fullExchangeName"),
            "timezone": md.get("exchangeTimezoneName"),
            "currency": md.get("currency"),
        }
    except Exception:
        return None


def label_to_session(label: str) -> str:
    return {"盘前": "pre", "盘中": "regular", "盘后": "post"}.get(label, "closed")


def _is_rate_limit(exc) -> bool:
    """判断异常是不是 Yahoo 限流。yfinance 新版本有专门的异常类，
    老版本只在消息文本里体现，所以两条路都要认。"""
    if "RateLimit" in type(exc).__name__:
        return True
    msg = str(exc).lower()
    return "too many requests" in msg or "rate limit" in msg or "429" in msg


def cmd_quotes(symbols):
    """一次性报价（自检/命令行用）：同样走批量接口。"""
    try:
        _emit(batch_quotes(symbols))
    except Exception as e:
        _emit({"error": f"{type(e).__name__}: {e}"})


def cmd_spark(symbols):
    """5 天 / 1 小时收盘序列，用于迷你走势图。"""
    out = {}

    def one(sym):
        try:
            hist = yf.Ticker(sym).history(period="5d", interval="1h", auto_adjust=True)
            closes = [round(float(c), 4) for c in hist["Close"].dropna().tolist()]
            return sym, closes
        except Exception:
            return sym, []

    if not symbols:
        _emit({})
        return
    with ThreadPoolExecutor(max_workers=min(MAX_WORKERS, len(symbols))) as ex:
        for sym, closes in ex.map(one, symbols):
            if closes:
                out[sym] = closes
    _emit(out)


def cmd_history(symbol, period, interval):
    try:
        hist = yf.Ticker(symbol).history(period=period, interval=interval, auto_adjust=True)
        bars = []
        for idx, row in hist.iterrows():
            bars.append({
                "timestamp": int(idx.timestamp()),
                "open": _safe_float(row.get("Open")),
                "high": _safe_float(row.get("High")),
                "low": _safe_float(row.get("Low")),
                "close": _safe_float(row.get("Close")),
                "volume": _safe_float(row.get("Volume")),
            })
        _emit(bars)
    except Exception as e:
        _emit({"error": str(e)})


# ══════════════════════════════════════════════════════════════════════════════
# 实时批量报价（Yahoo v7/finance/quote + cookie/crumb）
#
# 为什么用它：一次 HTTP 请求就能拿到全部标的，而且响应里直接带
#   marketState（PRE/REGULAR/POST/POSTPOST/CLOSED）、preMarketPrice、
#   postMarketPrice 及其涨跌幅 —— 正好对应「盘前/盘中/盘后」的需求。
# 对比逐标的拉 1 分钟线：请求数从 N 降到 1，44 个标的实测 ~0.3 秒，
# 所以可以做到 5 秒一轮的准实时刷新，而不触发 Yahoo 限流。
# ══════════════════════════════════════════════════════════════════════════════

QUOTE_CHUNK = 50
_STATE = {"session": None, "crumb": None}


def _new_session():
    from curl_cffi import requests as cr
    s = cr.Session(impersonate="chrome")
    try:
        s.get("https://fc.yahoo.com/", timeout=10)
    except Exception:
        pass
    return s


def _get_crumb(session):
    r = session.get("https://query1.finance.yahoo.com/v1/test/getcrumb", timeout=10)
    return r.text.strip()


def _state_label(state, post_present, pre_present):
    """Yahoo 的 marketState → (标签, 是否正在交易)"""
    if state == "PRE":
        return "盘前", True
    if state == "REGULAR":
        return "盘中", True
    if state == "POST":
        return "盘后", True
    if state == "POSTPOST":
        return "盘后", False          # 盘后已结束，但仍显示盘后价
    if state == "PREPRE":
        return "盘前", False
    return "收盘", False


def batch_quotes(symbols):
    """一次（或几次，按 50 个一批）请求拿到全部报价。返回本程序约定的字段。"""
    if not symbols:
        return []
    session = _STATE["session"]
    if session is None:
        session = _new_session()
        _STATE["session"] = session
        _STATE["crumb"] = _get_crumb(session)

    out = []
    for i in range(0, len(symbols), QUOTE_CHUNK):
        chunk = symbols[i:i + QUOTE_CHUNK]
        params = {"symbols": ",".join(chunk), "crumb": _STATE["crumb"]}
        r = session.get("https://query1.finance.yahoo.com/v7/finance/quote",
                        params=params, timeout=20)
        if r.status_code in (401, 403):        # crumb 过期 → 重取一次
            _STATE["crumb"] = _get_crumb(session)
            params["crumb"] = _STATE["crumb"]
            r = session.get("https://query1.finance.yahoo.com/v7/finance/quote",
                            params=params, timeout=20)
        try:
            rows = r.json().get("quoteResponse", {}).get("result", [])
        except Exception:
            continue

        for q in rows:
            sym = q.get("symbol")
            if not sym:
                continue
            state = q.get("marketState") or "CLOSED"
            prev = q.get("regularMarketPreviousClose") or q.get("previousClose")
            reg = q.get("regularMarketPrice")
            pre = q.get("preMarketPrice")
            post = q.get("postMarketPrice")
            label, active = _state_label(state, post is not None, pre is not None)

            # ── 主显示：一律用「盘中」（常规时段）──────────────
            # 无论现在是盘前、盘后还是夜盘，主行永远显示当日盘中价与盘中涨跌幅，
            # 排序也按它来 —— 否则一到盘后，屏幕上就只剩盘后的零点几个点，
            # 当天的真实涨跌反而被盖掉了。
            reg_pct = q.get("regularMarketChangePercent")
            reg_chg = q.get("regularMarketChange")
            if reg_chg is None and reg is not None and prev:
                reg_chg = reg - prev
            if reg_pct is None and reg is not None and prev:
                reg_pct = (reg - prev) / prev * 100.0

            if reg is None:                     # 极少数情况（例如停牌）退回可用价
                reg = pre if pre is not None else post

            # ── 副显示：盘前 / 盘后小字 ────────────────────────
            # 取「时间上离现在更近」的那个延长时段：
            #   · PRE / PREPRE           → 盘前
            #   · POST / POSTPOST / 收盘  → 盘后（盘后时间戳晚于盘中收盘）
            #   · REGULAR（正在盘中）     → 不显示（此时盘中就是全部信息，
            #                              响应里的 pre/post 是上一场的残留数据）
            pre_ts = q.get("preMarketTime")
            post_ts = q.get("postMarketTime")
            reg_ts = q.get("regularMarketTime")
            ext_session, ext_label, ext_price, ext_pct, ext_ts = "", "", None, None, None

            def _newer(a, b):
                return a is not None and (b is None or a > b)

            if state in ("PRE", "PREPRE") and pre is not None:
                ext_session, ext_label, ext_price = "pre", "盘前", pre
                ext_pct, ext_ts = q.get("preMarketChangePercent"), pre_ts
            elif state in ("POST", "POSTPOST") and post is not None:
                ext_session, ext_label, ext_price = "post", "盘后", post
                ext_pct, ext_ts = q.get("postMarketChangePercent"), post_ts
            elif state not in ("REGULAR",) and _newer(post_ts, reg_ts) and post is not None:
                ext_session, ext_label, ext_price = "post", "盘后", post
                ext_pct, ext_ts = q.get("postMarketChangePercent"), post_ts
            elif state not in ("REGULAR",) and _newer(pre_ts, reg_ts) and pre is not None:
                ext_session, ext_label, ext_price = "pre", "盘前", pre
                ext_pct, ext_ts = q.get("preMarketChangePercent"), pre_ts

            if reg is None:
                continue

            out.append({
                "symbol": sym,
                "name": q.get("longName") or q.get("shortName") or sym,
                # 时段（给状态栏用）与主显示解耦：主显示永远是盘中
                "session": state.lower(),
                "session_label": label,
                "session_active": active,
                "market_state": state,
                # ── 主：盘中 ──
                "price": float(reg),
                "regular_price": float(reg),
                "regular_change": reg_chg,
                "regular_change_percent": reg_pct,
                "regular_time": reg_ts,
                "previous_close": prev,
                # 兼容字段：change/change_percent 也等于盘中口径
                "change": reg_chg,
                "change_percent": reg_pct,
                # ── 副：盘前 / 盘后 ──
                "ext_session": ext_session,
                "ext_label": ext_label,
                "ext_price": ext_price,
                "ext_change_percent": ext_pct,
                "ext_time": ext_ts,
                "pre_price": pre,
                "post_price": post,
                "day_high": q.get("regularMarketDayHigh"),
                "day_low": q.get("regularMarketDayLow"),
                "volume": q.get("regularMarketVolume"),
                "data_time": reg_ts or ext_ts,
                "exchange": q.get("fullExchangeName"),
                "timezone": q.get("exchangeTimezoneName"),
                "currency": q.get("currency"),
                "data_source": "yahoo",
                "source_label": "Yahoo",
                "delayed": False,
            })

    # 期货实时补丁：Yahoo 的 CME/CBOT 期货永久滞后 10 分钟，用新浪实时价替换
    # （拿不到实时源时如实标记 delayed，让界面能说清“为什么不动”）
    try:
        apply_futures_realtime(out)
    except Exception:
        pass
    return out


# ══════════════════════════════════════════════════════════════════════════════
# 美股指数期货的「实时」补丁（新浪外盘期货接口）
#
# 为什么需要：Yahoo 对 CME / CBOT 的期货只提供**延迟 10 分钟**的数据 ——
#   ES=F 的 regularMarketTime 恒比现在早 10.0 分钟（v8/finance/chart 的 1 分钟线
#   同样滞后 10.0 分钟，所以不是端点问题，是交易所授权限制），而 marketState
#   却报 REGULAR，界面上看起来“在交易但数据不动”。
# 新浪的外盘期货接口是实时的（时间戳与本地时钟同秒），且**昨收字段与 Yahoo 完全一致**
#   （ES=F: 两边都是 7623.0），所以可以直接覆盖价格与涨跌幅。
# 安全阀：只有当新浪的昨收与 Yahoo 的昨收差异 < 0.3% 时才认为两者是同一个合约月份，
#   否则（例如原油的活跃月份不同）保留 Yahoo 数据并标记为“延迟”。
# ══════════════════════════════════════════════════════════════════════════════

SINA_FUTURES = {
    "ES=F": "hf_ES",    # 标普500 期货
    "NQ=F": "hf_NQ",    # 纳斯达克100 期货
    "YM=F": "hf_YM",    # 道琼斯 期货
    "GC=F": "hf_GC",    # 纽约黄金
    "SI=F": "hf_SI",    # 纽约白银
    "CL=F": "hf_CL",    # 纽约原油
    "NG=F": "hf_NG",    # 美国天然气
    "HG=F": "hf_HG",    # 美铜
}
SINA_URL = "https://hq.sinajs.cn/list="
SINA_HEADERS = {"Referer": "https://finance.sina.com.cn",
                "User-Agent": "Mozilla/5.0 (X11; Linux x86_64) Chrome/120 Safari/537.36"}


def _sina_fetch(codes, session=None):
    """取新浪外盘期货行情，返回 {code: {price, prev, open, high, low, ts, name}}"""
    if not codes:
        return {}
    out = {}
    try:
        sess = session or _new_session()
        r = sess.get(SINA_URL + ",".join(codes), headers=SINA_HEADERS, timeout=10)
        text = r.content.decode("gbk", errors="ignore")
    except Exception:
        return {}
    for line in text.split("\n"):
        m = re.match(r'var hq_str_(\w+)="(.*)";', line.strip())
        if not m:
            continue
        code, body = m.group(1), m.group(2)
        f = body.split(",")
        if len(f) < 14 or not f[0]:
            continue
        try:
            price, prev = float(f[0]), float(f[7])
        except Exception:
            continue
        # 时间字段是北京时间（与服务器同时区），转成 epoch
        ts = None
        try:
            ts = int(time.mktime(time.strptime(f[12] + " " + f[6], "%Y-%m-%d %H:%M:%S")))
        except Exception:
            ts = int(time.time())
        out[code] = {
            "price": price,
            "prev": prev,
            "open": float(f[8]) if f[8] else None,
            "high": float(f[4]) if f[4] else None,
            "low": float(f[5]) if f[5] else None,
            "ts": ts,
            "name": f[13],
        }
    return out


def apply_futures_realtime(rows, lag_threshold=60):
    """把延迟的期货报价换成新浪实时价；返回被替换的代码列表。"""
    now = time.time()
    targets = []
    for q in rows:
        code = SINA_FUTURES.get(q.get("symbol"))
        if not code:
            continue
        ts = q.get("regular_time")
        lag = (now - ts) if ts else 0
        if q.get("market_state") == "REGULAR" and lag > lag_threshold:
            targets.append((q, code))
    if not targets:
        return []

    realtime = _sina_fetch([c for _, c in targets])
    fixed = []
    for q, code in targets:
        r = realtime.get(code)
        if not r or not r["price"] or not r["prev"]:
            q["delayed"] = True          # 拿不到实时源：如实标记为延迟
            q["delay_note"] = "Yahoo 对 CME/CBOT 期货延迟 10 分钟"
            continue
        yahoo_prev = q.get("previous_close") or 0
        # 同一个合约月份的校验：昨收必须基本一致，否则宁可不换
        if yahoo_prev and abs(r["prev"] - yahoo_prev) / yahoo_prev > 0.003:
            q["delayed"] = True
            q["delay_note"] = "Yahoo 对 CME/CBOT 期货延迟 10 分钟（合约月份与实时源不一致，未替换）"
            continue
        new_pct = (r["price"] - r["prev"]) / r["prev"] * 100.0
        q["price"] = r["price"]
        q["regular_price"] = r["price"]
        q["regular_change"] = r["price"] - r["prev"]
        q["regular_change_percent"] = new_pct
        q["regular_time"] = r["ts"]
        q["data_time"] = r["ts"]
        q["change"] = q["regular_change"]
        q["change_percent"] = new_pct
        if r["high"]:
            q["day_high"] = r["high"]
        if r["low"]:
            q["day_low"] = r["low"]
        q["data_source"] = "sina"
        q["source_label"] = "新浪实时"
        q["delayed"] = False
        fixed.append(q["symbol"])
    return fixed


def run_daemon():
    """常驻模式：stdin 读 JSON 行请求，stdout 写 JSON 行结果。

    这样刷新不需要每次启动 Python 进程（省 1.5~2 秒），配合批量接口
    就能做到 5 秒一轮的准实时刷新。
      {"id":1,"cmd":"quotes","symbols":[...]}
      {"id":2,"cmd":"spark","symbols":[...]}
      {"id":3,"cmd":"history","symbol":"AAPL","range":"1mo","interval":"1d"}
    """
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except Exception:
            continue
        rid = req.get("id")
        cmd = req.get("cmd")
        try:
            if cmd == "quotes":
                data = batch_quotes(req.get("symbols") or [])
                _emit({"id": rid, "ok": True, "data": data})
            elif cmd == "spark":
                syms = req.get("symbols") or []
                out = {}
                def one(s):
                    try:
                        h = yf.Ticker(s).history(period="5d", interval="1h", auto_adjust=True)
                        return s, [round(float(c), 4) for c in h["Close"].dropna().tolist()]
                    except Exception:
                        return s, []
                with ThreadPoolExecutor(max_workers=min(8, max(1, len(syms)))) as ex:
                    for s, closes in ex.map(one, syms):
                        if closes:
                            out[s] = closes
                _emit({"id": rid, "ok": True, "data": out})
            elif cmd == "history":
                sym = req.get("symbol")
                per = req.get("range") or "6mo"
                ivl = req.get("interval") or "1d"
                h = yf.Ticker(sym).history(period=per, interval=ivl, auto_adjust=True)
                bars = [{"timestamp": int(i.timestamp()),
                         "open": _safe_float(r.get("Open")), "high": _safe_float(r.get("High")),
                         "low": _safe_float(r.get("Low")), "close": _safe_float(r.get("Close")),
                         "volume": _safe_float(r.get("Volume"))} for i, r in h.iterrows()]
                _emit({"id": rid, "ok": True, "data": bars})
            elif cmd == "ping":
                _emit({"id": rid, "ok": True, "data": {"pong": True}})
            else:
                _emit({"id": rid, "ok": False, "error": f"unknown cmd: {cmd}"})
        except Exception as e:
            _emit({"id": rid, "ok": False, "error": f"{type(e).__name__}: {e}"})
    return 0


def main(argv):
    if len(argv) < 2:
        _emit({"error": "usage: yf_fetch.py quotes|spark|history ..."})
        return 1
    cmd = argv[1]
    if cmd == "daemon":
        return run_daemon()
    if cmd == "quotes":
        cmd_quotes(argv[2:])
    elif cmd == "spark":
        cmd_spark(argv[2:])
    elif cmd == "history":
        if len(argv) < 3:
            _emit({"error": "usage: yf_fetch.py history SYMBOL RANGE INTERVAL"})
            return 1
        symbol = argv[2]
        period = argv[3] if len(argv) > 3 else "6mo"
        interval = argv[4] if len(argv) > 4 else "1d"
        cmd_history(symbol, period, interval)
    else:
        _emit({"error": f"unknown command: {cmd}"})
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
