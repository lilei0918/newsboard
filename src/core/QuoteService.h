#pragma once
// NewsBoard — 行情服务（yfinance 桥）
//
// 行情链路（实时优先）：
//   · 常驻 Python 守护进程（scripts/yf_fetch.py daemon），避免每次刷新都启动解释器
//   · 每轮刷新用 Yahoo 批量报价接口，**1 个 HTTP 请求拿全部标的**（实测 44 标的约 0.3 秒）
//   · 因此可以做到默认 5 秒一轮的准实时刷新，而总请求量只有 720 次/小时
//   · 响应里自带 marketState / preMarketPrice / postMarketPrice，直接对应盘前盘中盘后
//   · 迷你走势 5 分钟一次；历史 K 线按需（点开图表时）走同一个守护进程
//
// 防封要点：
//   · 批量：一次进程拿到整屏，不是一标的一次调用
//   · 节流：两次调用之间至少间隔 kMinGapMs；上一轮未结束不发下一轮
//   · 429 冷却：识别到限流立即进入冷却窗口（默认 120s），期间完全不发请求；
//     非限流的连续失败累计 kFailuresBeforeBackoff 次也退避 60s，
//     避免把临时故障打成封禁
//   · 失败不清空界面：保留上一轮数据，只在状态栏提示

#include "core/Models.h"

#include <QHash>
#include <QObject>
#include <QVector>

#include <functional>

class QProcess;
class QTimer;

namespace nb {

class QuoteService : public QObject {
    Q_OBJECT
  public:
    explicit QuoteService(QObject* parent = nullptr);

    void start();   // 启动定时刷新
    void stop();
    void refresh_now();                 // 立即刷新报价
    void set_refresh_seconds(int s);    // 改刷新频率（实时档 3~10 秒）
    void refresh_sparklines();        // 拉迷你走势（低频）

    /// 一次性自检：报价 + 迷你走势 + 历史 K 线，结果写 <data>/selftest.json。
    /// 入口：NB_SELFTEST=1 ./NewsBoard（会打印结果并自动退出）
    void run_selftest(const QString& out_path);

    void fetch_history(const QString& symbol, const QString& range, const QString& interval,
                       std::function<void(bool, QVector<Bar>)> cb);

    QVector<Quote> quotes() const { return quotes_; }
    Quote quote_for(const QString& symbol) const;
    bool busy() const { return quotes_in_flight_; }
    qint64 last_ok_ms() const { return last_ok_ms_; }
    QString last_error() const { return last_error_; }
    int seconds_to_next() const;

    /// 限流冷却状态（状态栏用）
    bool in_cooldown() const;
    int cooldown_seconds_left() const;
    void enter_cooldown(int seconds, const QString& reason);

  signals:
    void quotes_updated();
    void history_ready(const QString& symbol, QVector<Bar> bars);
    void status_changed();

  private:
    // ── 守护进程通信 ──
    void ensure_daemon();
    void send_request(const QString& cmd, const QJsonObject& extra,
                      std::function<void(bool, QJsonValue, QString)> cb);
    void on_daemon_output();
    void on_daemon_finished();

    // ── 一次性脚本（守护进程不可用时的兜底 / 自检）──
    void run_script(const QStringList& args, std::function<void(bool, QByteArray, QString)> cb);
    bool parse_quotes(const QJsonValue& data);   // false = 内容异常
    void parse_sparklines(const QJsonValue& data);
    void note_failure(const QString& error);   // 分类失败并决定是否退避
    void note_success();
    /// 状态流水账（追加到 <data>/quote_status.log），出问题时能查"行情为什么停了"
    void log_status(const QString& message);

    QTimer* timer_ = nullptr;
    QTimer* restart_timer_ = nullptr;   // 守护进程掉线后的重启
    QTimer* spark_timer_ = nullptr;
    QProcess* daemon_ = nullptr;      // 常驻守护进程
    QHash<int, std::function<void(bool, QJsonValue, QString)>> pending_;
    QByteArray daemon_buf_;
    int next_req_id_ = 1;
    bool quotes_in_flight_ = false;
    bool spark_busy_ = false;
    qint64 last_call_ms_ = 0;
    qint64 last_ok_ms_ = 0;
    qint64 cooldown_until_ms_ = 0;
    /// 自上次成功以来是否进过冷却 —— 用于在恢复时记一条日志
    /// （成功那一刻冷却已过期，不能在 note_success 里现查 in_cooldown()）
    bool had_cooldown_ = false;
    int consecutive_failures_ = 0;
    QString last_error_;
    QVector<Quote> quotes_;
    /// 迷你走势按标的缓存。报价与走势是两个独立进程，谁先回来不确定，
    /// 所以走势先到就先存这里，报价解析时再合并 —— 避免冷启动丢掉走势。
    QHash<QString, QVector<double>> spark_by_symbol_;
    QHash<QString, QVector<Bar>> history_cache_;
    static constexpr int kMinGapMs = 1500;              // 两次刷新之间的硬下限
    static constexpr int kRateLimitCooldownSec = 120;   // 429 → 冷却 2 分钟
    static constexpr int kFailureCooldownSec = 60;      // 连续失败 → 退避 1 分钟
    static constexpr int kFailuresBeforeBackoff = 3;
};

} // namespace nb
