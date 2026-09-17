#include "core/QuoteService.h"

#include "core/Config.h"
#include "core/PyEnv.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTextStream>
#include <QTimer>

namespace nb {

QuoteService::QuoteService(QObject* parent) : QObject(parent) {}

void QuoteService::start() {
    if (!timer_) {
        timer_ = new QTimer(this);
        connect(timer_, &QTimer::timeout, this, [this]() { refresh_now(); });
    }
    if (!spark_timer_) {
        spark_timer_ = new QTimer(this);
        spark_timer_->setInterval(5 * 60 * 1000);  // 迷你走势 5 分钟一次
        connect(spark_timer_, &QTimer::timeout, this, [this]() { refresh_sparklines(); });
    }
    if (!restart_timer_) {
        restart_timer_ = new QTimer(this);
        restart_timer_->setSingleShot(true);
        connect(restart_timer_, &QTimer::timeout, this, [this]() { ensure_daemon(); });
    }

    // 常驻守护进程：省掉每次刷新启动解释器的 1.5~2 秒，才有资格谈“实时”
    ensure_daemon();

    timer_->start(Config::instance().quote_refresh_seconds() * 1000);
    spark_timer_->start();

    // 启动时先读缓存，界面立刻有内容；随后马上刷新一次
    QFile f(Config::instance().data_dir() + "/quotes_cache.json");
    if (f.exists() && f.open(QIODevice::ReadOnly)) {
        const QByteArray raw = f.readAll();
        f.close();
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (doc.isArray()) parse_quotes(doc.array());
        emit quotes_updated();
    }

    QTimer::singleShot(300, this, [this]() {
        refresh_now();
        refresh_sparklines();
    });
}

void QuoteService::set_refresh_seconds(int s) {
    if (timer_) timer_->start(qBound(3, s, 3600) * 1000);
}

// ── 常驻守护进程 ────────────────────────────────────────────────────────────

void QuoteService::ensure_daemon() {
    if (daemon_ && daemon_->state() != QProcess::NotRunning) return;
    if (!PyEnv().is_ready()) return;

    const QString py = Config::instance().python_path();
    const QString script = Config::instance().script_path();
    if (!QFile::exists(py) || !QFile::exists(script)) return;

    daemon_ = new QProcess(this);
    daemon_->setProcessChannelMode(QProcess::SeparateChannels);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    const QString cache = Config::instance().cache_dir();
    QDir().mkpath(cache);
    env.insert(QStringLiteral("XDG_CACHE_HOME"), cache);
    daemon_->setProcessEnvironment(env);

    connect(daemon_, &QProcess::readyReadStandardOutput, this, [this]() { on_daemon_output(); });
    connect(daemon_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        log_status(QStringLiteral("守护进程结束：exit=%1 status=%2")
                       .arg(code)
                       .arg(st == QProcess::NormalExit ? QStringLiteral("normal") : QStringLiteral("crash")));
        on_daemon_finished();
    });
    connect(daemon_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (daemon_ && daemon_->state() == QProcess::NotRunning) on_daemon_finished();
    });

    daemon_->start(py, QStringList{script, QStringLiteral("daemon")});
    log_status(QStringLiteral("行情守护进程启动：%1 %2 daemon").arg(py, script));
}

void QuoteService::on_daemon_finished() {
    const QString err = daemon_ ? QString::fromUtf8(daemon_->readAllStandardError()).trimmed() : QString();
    const QString out_tail = daemon_ ? QString::fromUtf8(daemon_->readAllStandardOutput()).trimmed() : QString();
    if (!out_tail.isEmpty())
        log_status(QStringLiteral("守护进程 stdout：%1").arg(out_tail.left(300)));
    if (!err.isEmpty()) log_status(QStringLiteral("守护进程 stderr：%1").arg(err.left(300)));
    if (daemon_) {
        daemon_->deleteLater();
        daemon_ = nullptr;
    }
    daemon_buf_.clear();

    // 未回应的请求全部按失败处理（调用方各自有兜底路径）
    const auto pend = pending_;
    pending_.clear();
    quotes_in_flight_ = false;
    spark_busy_ = false;
    for (auto it = pend.begin(); it != pend.end(); ++it) {
        if (it.value()) it.value()(false, {}, err.isEmpty() ? QStringLiteral("行情守护进程已退出") : err.right(160));
    }

    // 5 秒后重来；期间 refresh_now 会退回一次性脚本
    if (restart_timer_ && !restart_timer_->isActive()) restart_timer_->start(5000);
}

void QuoteService::on_daemon_output() {
    if (!daemon_) return;
    daemon_buf_ += daemon_->readAllStandardOutput();

    int nl = 0;
    while ((nl = daemon_buf_.indexOf('\n')) >= 0) {
        const QByteArray line = daemon_buf_.left(nl);
        daemon_buf_.remove(0, nl + 1);
        if (line.trimmed().isEmpty()) continue;

        const QJsonObject o = QJsonDocument::fromJson(line).object();
        const int id = o.value("id").toInt(-1);
        auto cb = pending_.take(id);
        if (!cb) continue;

        const bool ok = o.value("ok").toBool(false);
        if (ok) {
            cb(true, o.value("data"), {});
        } else {
            cb(false, {}, o.value("error").toString(QStringLiteral("行情请求失败")));
        }
    }
    if (daemon_buf_.size() > 8 * 1024 * 1024) daemon_buf_.clear();   // 防跑飞
}

void QuoteService::send_request(const QString& cmd, const QJsonObject& extra,
                                std::function<void(bool, QJsonValue, QString)> cb) {
    if (!daemon_ || daemon_->state() != QProcess::Running) {
        ensure_daemon();
    }
    if (!daemon_ || daemon_->state() != QProcess::Running) {
        if (cb) cb(false, {}, QStringLiteral("行情守护进程未就绪"));
        return;
    }
    const int id = next_req_id_++;
    QJsonObject req = extra;
    req.insert(QStringLiteral("id"), id);
    req.insert(QStringLiteral("cmd"), cmd);
    pending_.insert(id, cb);
    daemon_->write(QJsonDocument(req).toJson(QJsonDocument::Compact) + "\n");
}

void QuoteService::stop() {
    if (timer_) timer_->stop();
    if (spark_timer_) spark_timer_->stop();
    if (restart_timer_) restart_timer_->stop();
    if (daemon_) {
        pending_.clear();
        daemon_->closeWriteChannel();
        if (!daemon_->waitForFinished(1500)) {
            daemon_->kill();
            daemon_->waitForFinished(500);
        }
    }
}

int QuoteService::seconds_to_next() const {
    if (!timer_) return 0;
    return timer_->isActive() ? (timer_->remainingTime() + 999) / 1000 : 0;
}

bool QuoteService::in_cooldown() const {
    return QDateTime::currentMSecsSinceEpoch() < cooldown_until_ms_;
}

int QuoteService::cooldown_seconds_left() const {
    const qint64 left = cooldown_until_ms_ - QDateTime::currentMSecsSinceEpoch();
    return left > 0 ? int((left + 999) / 1000) : 0;
}

void QuoteService::enter_cooldown(int seconds, const QString& reason) {
    const qint64 until = QDateTime::currentMSecsSinceEpoch() + qint64(seconds) * 1000;
    if (until <= cooldown_until_ms_) return;
    cooldown_until_ms_ = until;
    had_cooldown_ = true;
    last_error_ = QStringLiteral("%1（冷却 %2 秒）").arg(reason).arg(seconds);
    emit status_changed();
}

void QuoteService::log_status(const QString& message) {
    const QString path = Config::instance().data_dir() + "/quote_status.log";
    QDir().mkpath(Config::instance().data_dir());

    QFile f(path);
    if (f.exists() && f.size() > 256 * 1024) f.remove();   // 简单轮转
    if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
        const QString line = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) +
                             QStringLiteral("  ") + message + QLatin1Char('\n');
        f.write(line.toUtf8());
        f.close();
    }
}

void QuoteService::note_success() {
    const bool was_cooling = had_cooldown_;
    had_cooldown_ = false;
    consecutive_failures_ = 0;
    cooldown_until_ms_ = 0;
    last_error_.clear();
    last_ok_ms_ = QDateTime::currentMSecsSinceEpoch();
    if (was_cooling) log_status(QStringLiteral("冷却结束，行情恢复正常"));
}

void QuoteService::note_failure(const QString& error) {
    last_error_ = error;

    // 限流：立刻冷却且绝不重试（重试会把限流打成封禁）
    const QString lower = error.toLower();
    if (lower.contains(QStringLiteral("too many requests")) || lower.contains(QStringLiteral("ratelimit")) ||
        lower.contains(QStringLiteral("rate limit")) || lower.contains(QStringLiteral("rate_limited")) ||
        lower.contains(QStringLiteral("429"))) {
        consecutive_failures_ = 0;
        enter_cooldown(kRateLimitCooldownSec, QStringLiteral("行情源限流"));
        log_status(QStringLiteral("检测到限流 → 进入 %1 秒冷却（期间不发任何请求）").arg(kRateLimitCooldownSec));
        return;
    }

    // 其它失败：连续多次才退避，避免网络抖动导致长时间静默
    if (++consecutive_failures_ >= kFailuresBeforeBackoff) {
        consecutive_failures_ = 0;
        enter_cooldown(kFailureCooldownSec, QStringLiteral("行情连续获取失败"));
        log_status(QStringLiteral("连续 %1 次失败 → 退避 %2 秒").arg(kFailuresBeforeBackoff).arg(kFailureCooldownSec));
    } else {
        log_status(QStringLiteral("行情获取失败（第 %1 次）：%2").arg(consecutive_failures_).arg(error.left(160)));
    }
    emit status_changed();
}

// 把一轮批量结果落盘（下次启动先读缓存，界面无白屏）
static void save_quote_cache(const QJsonArray& arr) {
    const QString path = Config::instance().data_dir() + "/quotes_cache.json";
    QDir().mkpath(Config::instance().data_dir());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
        f.close();
    }
}

void QuoteService::refresh_now() {
    if (quotes_in_flight_) return;           // 上一轮没结束，跳过（不排队，避免堆积）
    if (in_cooldown()) return;               // 冷却期内完全不发请求

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - last_call_ms_ < kMinGapMs) return;  // 节流

    const QStringList symbols = Config::instance().all_symbols();
    if (symbols.isEmpty()) return;

    if (!PyEnv().is_ready()) {
        last_error_ = QStringLiteral("行情依赖未安装");
        emit status_changed();
        return;
    }

    quotes_in_flight_ = true;
    last_call_ms_ = now;
    emit status_changed();

    // 拿到数据后统一处理
    auto ingest = [this](bool ok, const QJsonValue& data, const QString& err) {
        quotes_in_flight_ = false;
        if (!ok) {
            note_failure(err);
        } else if (!parse_quotes(data)) {
            note_failure(last_error_.isEmpty() ? QStringLiteral("行情返回内容异常") : last_error_);
        } else {
            note_success();
            save_quote_cache(data.toArray());
        }
        emit quotes_updated();
        emit status_changed();
    };

    // 优先走常驻进程；它没起来时退回“一次性脚本”，保证任何环境下都有行情
    if (daemon_ && daemon_->state() == QProcess::Running) {
        QJsonArray arr;
        for (const auto& s : symbols) arr.append(s);
        QJsonObject extra;
        extra.insert(QStringLiteral("symbols"), arr);

        const QStringList syms = symbols;
        send_request(QStringLiteral("quotes"), extra,
                     [this, ingest, syms](bool ok, QJsonValue data, QString err) {
            if (ok) {
                ingest(true, data, {});
                return;
            }
            // 守护进程这一轮失败了：先不判死刑，改用一次性脚本再试一次
            log_status(QStringLiteral("守护进程报价失败，改用一次性脚本：%1").arg(err.left(120)));
            run_script(QStringList{QStringLiteral("quotes")} + syms,
                       [ingest](bool ok2, QByteArray out2, QString err2) {
                if (!ok2) { ingest(false, {}, err2); return; }
                const QJsonDocument d2 = QJsonDocument::fromJson(out2);
                if (d2.isArray()) {
                    ingest(true, d2.array(), {});
                } else {
                    const QJsonObject o2 = d2.object();
                    ingest(false, {}, o2.value("error").toString(QStringLiteral("行情返回内容异常")));
                }
            });
        });
    } else {
        const QStringList syms = symbols;
        run_script(QStringList{QStringLiteral("quotes")} + syms,
                   [ingest](bool ok, QByteArray out, QString err) {
            if (!ok) { ingest(false, {}, err); return; }
            const QJsonDocument d = QJsonDocument::fromJson(out);
            if (d.isArray()) {
                ingest(true, d.array(), {});
            } else {
                ingest(false, {}, d.object().value("error").toString(QStringLiteral("行情返回内容异常")));
            }
        });
    }
}

void QuoteService::refresh_sparklines() {
    if (spark_busy_) return;
    if (in_cooldown()) return;                 // 与报价共用冷却：限流期间不发请求
    const QStringList symbols = Config::instance().all_symbols();
    if (symbols.isEmpty() || !PyEnv().is_ready()) return;

    spark_busy_ = true;

    auto ingest = [this](bool ok, const QJsonValue& data, const QString& err) {
        spark_busy_ = false;
        if (ok) {
            parse_sparklines(data);
        } else {
            note_failure(err);                 // 走势失败也参与限流判定
        }
        emit quotes_updated();
    };

    QJsonArray arr;
    for (const auto& s : symbols) arr.append(s);
    QJsonObject extra;
    extra.insert(QStringLiteral("symbols"), arr);

    if (daemon_ && daemon_->state() == QProcess::Running) {
        send_request(QStringLiteral("spark"), extra, ingest);
    } else {
        run_script(QStringList{QStringLiteral("spark")} + symbols,
                   [ingest](bool ok, QByteArray out, QString err) {
            if (!ok) { ingest(false, {}, err); return; }
            ingest(true, QJsonDocument::fromJson(out).object(), {});
        });
    }
}

void QuoteService::run_script(const QStringList& args, std::function<void(bool, QByteArray, QString)> cb) {
    const QString py = Config::instance().python_path();
    const QString script = Config::instance().script_path();

    if (!QFile::exists(py) || !QFile::exists(script)) {
        if (cb) cb(false, {}, QStringLiteral("找不到 %1 或 %2").arg(py, script));
        return;
    }

    auto* p = new QProcess(this);
    p->setProcessChannelMode(QProcess::SeparateChannels);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    // yfinance/curl_cffi 的 cookie 库是 sqlite，必须有可写目录；指到项目内既满足
    // “自带运行环境”，也避免只读缓存导致 "attempt to write a readonly database"
    // 把整条行情链路打死。
    const QString cache = Config::instance().cache_dir();
    QDir().mkpath(cache);
    env.insert(QStringLiteral("XDG_CACHE_HOME"), cache);
    p->setProcessEnvironment(env);

    connect(p, &QProcess::finished, this, [p, cb](int code, QProcess::ExitStatus) {
        const QByteArray out = p->readAllStandardOutput();
        const QString err = QString::fromUtf8(p->readAllStandardError()).trimmed();
        p->deleteLater();
        if (code != 0) {
            if (cb) cb(false, {}, err.isEmpty() ? QStringLiteral("进程退出码 %1").arg(code) : err.right(200));
            return;
        }
        if (cb) cb(true, out, {});
    });

    p->start(py, QStringList{script} + args);
}

bool QuoteService::parse_quotes(const QJsonValue& data) {
    if (data.isObject()) {
        // 脚本以 {"error": …} 上报失败（退出码仍是 0）
        const QJsonObject o = data.toObject();
        if (o.contains("error")) {
            const QString e = o.value("error").toString();
            last_error_ = e.isEmpty() ? QStringLiteral("行情返回错误") : e;
            return false;
        }
        last_error_ = QStringLiteral("行情返回内容异常");
        return false;
    }
    if (!data.isArray()) {
        last_error_ = QStringLiteral("行情返回不是 JSON");
        return false;
    }
    const QJsonArray arr = data.toArray();
    if (arr.isEmpty()) {
        last_error_ = QStringLiteral("行情返回空列表");
        return false;
    }

    // 保留上一轮的 sparkline，按 symbol 合并
    QHash<QString, QVector<double>> sparks = spark_by_symbol_;
    for (const auto& q : quotes_) {
        if (!sparks.contains(q.symbol) && !q.spark.isEmpty()) sparks.insert(q.symbol, q.spark);
    }

    // 分组归属
    QHash<QString, QString> group_of;
    for (const auto& g : Config::instance().quote_groups()) {
        for (const auto& s : g.symbols) group_of.insert(s, g.id);
    }

    QVector<Quote> out;
    for (const auto& v : arr) {
        const QJsonObject o = v.toObject();
        Quote q;
        q.symbol = o.value("symbol").toString();
        if (q.symbol.isEmpty()) continue;
        q.name = o.value("name").toString(q.symbol);
        q.alias = Config::instance().quote_alias(q.symbol);
        q.price = o.value("price").toDouble();
        q.prevClose = o.value("previous_close").toDouble();
        q.change = o.value("change").toDouble();
        // 主显示与排序一律用「盘中」口径（脚本里 change_percent 已等于盘中涨跌幅，
        // 单独取 regular_change_percent 是为了口径显式、不依赖脚本的兼容字段）
        q.changePct = o.contains("regular_change_percent") && !o.value("regular_change_percent").isNull()
                          ? o.value("regular_change_percent").toDouble()
                          : o.value("change_percent").toDouble();
        q.high = o.value("day_high").toDouble();
        q.low = o.value("day_low").toDouble();
        q.volume = o.value("volume").toDouble();
        q.currency = o.value("currency").toString();
        q.session = o.value("session").toString();
        q.sessionLabel = o.value("session_label").toString();
        q.sessionActive = o.value("session_active").toBool(false);
        q.dataTs = static_cast<qint64>(o.value("data_time").toDouble());
        q.prePrice = o.value("pre_price").toDouble();
        q.regularPrice = o.value("regular_price").toDouble();
        q.postPrice = o.value("post_price").toDouble();
        q.extSession = o.value("ext_session").toString();
        q.extLabel = o.value("ext_label").toString();
        q.extPrice = o.value("ext_price").toDouble();
        q.extPct = o.value("ext_change_percent").toDouble();
        q.extTs = static_cast<qint64>(o.value("ext_time").toDouble());
        q.sourceLabel = o.value("source_label").toString();
        q.delayed = o.value("delayed").toBool(false);
        q.delayNote = o.value("delay_note").toString();
        q.group = group_of.value(q.symbol);
        q.spark = sparks.value(q.symbol);
        q.ok = q.price > 0;
        q.ts = QDateTime::currentSecsSinceEpoch();
        if (q.ok) out.append(q);
    }

    // 上一轮有、本轮没有的（临时失败）保留旧值，避免界面闪空
    for (const auto& q : quotes_) {
        bool present = false;
        for (const auto& n : out) {
            if (n.symbol == q.symbol) {
                present = true;
                break;
            }
        }
        if (!present && q.ok) out.append(q);
    }

    quotes_ = out;
    return !out.isEmpty();
}

void QuoteService::parse_sparklines(const QJsonValue& data) {
    const QJsonObject root = data.toObject();
    if (root.isEmpty()) return;

    for (auto it = root.begin(); it != root.end(); ++it) {
        const QJsonArray closes = it.value().toArray();
        if (closes.isEmpty()) continue;
        QVector<double> pts;
        pts.reserve(closes.size());
        for (const auto& c : closes) pts.append(c.toDouble());
        if (!pts.isEmpty()) spark_by_symbol_.insert(it.key(), pts);
    }

    // 已有报价立刻补上走势
    for (auto& q : quotes_) {
        if (spark_by_symbol_.contains(q.symbol)) q.spark = spark_by_symbol_.value(q.symbol);
    }
}

Quote QuoteService::quote_for(const QString& symbol) const {
    for (const auto& q : quotes_) {
        if (q.symbol == symbol) return q;
    }
    return {};
}

// ── 自检：报价 + 迷你走势 + 历史 K 线 ────────────────────────────────────────

void QuoteService::run_selftest(const QString& out_path) {
    // 顺序执行报价 → 走势 → 历史，保证报告里的数字是确定可读的
    // （三条并发时 spark 可能先于报价回来，届时还没有 symbol 可挂）。
    struct State {
        QJsonObject root;
    };
    auto st = std::make_shared<State>();
    st->root["python"] = Config::instance().python_path();
    st->root["script"] = Config::instance().script_path();
    st->root["python_ready"] = PyEnv().is_ready();

    const QStringList symbols = Config::instance().all_symbols();

    auto write_report = [st, out_path]() {
        QDir().mkpath(Config::instance().data_dir());
        QFile f(out_path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(QJsonDocument(st->root).toJson(QJsonDocument::Indented));
            f.close();
        }
        QTextStream ts(stdout);
        ts << QJsonDocument(st->root).toJson(QJsonDocument::Indented) << Qt::endl;
        ts.flush();
        QCoreApplication::quit();
    };

    auto step_history = std::make_shared<std::function<void()>>();
    auto step_spark = std::make_shared<std::function<void()>>();
    auto step_quotes = std::make_shared<std::function<void()>>();

    // 3) 历史 K 线
    *step_history = [this, st, symbols, write_report]() {
        const QString sym = symbols.isEmpty() ? QStringLiteral("AAPL") : symbols.first();
        run_script({QStringLiteral("history"), sym, QStringLiteral("1mo"), QStringLiteral("1d")},
                   [st, write_report, sym](bool ok, QByteArray out, QString err) {
                       QJsonObject r;
                       r["ok"] = ok;
                       r["symbol"] = sym;
                       if (ok) {
                           const QJsonDocument doc = QJsonDocument::fromJson(out);
                           const int n = doc.isArray() ? doc.array().size() : 0;
                           r["bars"] = n;
                           r["ok"] = n > 0;
                           if (n > 0) {
                               r["first_close"] = doc.array().first().toObject().value("close").toDouble();
                               r["last_close"] = doc.array().last().toObject().value("close").toDouble();
                           }
                       } else {
                           r["error"] = err;
                       }
                       st->root["history"] = r;
                       write_report();
                   });
    };

    // 2) 迷你走势（在报价之后，此时已有 symbol 可挂）
    *step_spark = [this, st, symbols, step_history]() {
        QStringList args{QStringLiteral("spark")};
        args << symbols.mid(0, 3);
        run_script(args, [this, st, step_history](bool ok, QByteArray out, QString err) {
            QJsonObject r;
            r["ok"] = ok;
            if (ok) {
                parse_sparklines(QJsonDocument::fromJson(out).object());
                r["series"] = spark_by_symbol_.size();
            } else {
                r["error"] = err;
            }
            st->root["sparklines"] = r;
            (*step_history)();
        });
    };

    // 1) 报价
    *step_quotes = [this, st, symbols, step_spark]() {
        QStringList args{QStringLiteral("quotes")};
        args << symbols.mid(0, 6);
        run_script(args, [this, st, step_spark](bool ok, QByteArray out, QString err) {
            QJsonObject r;
            r["ok"] = ok;
            if (ok) {
                const QJsonDocument d = QJsonDocument::fromJson(out);
                const bool parsed = parse_quotes(d.isArray() ? QJsonValue(d.array()) : QJsonValue(d.object()));
                r["ok"] = parsed;
                if (!parsed) r["error"] = last_error_;
                r["count"] = quotes_.size();
                QJsonArray sample;
                for (int i = 0; i < qMin(3, int(quotes_.size())); ++i) {
                    const Quote& q = quotes_.at(i);
                    QJsonObject o;
                    o["symbol"] = q.symbol;
                    o["name"] = q.name;
                    o["session"] = q.sessionLabel;
                    o["price"] = q.price;
                    o["change_percent"] = q.changePct;
                    sample.append(o);
                }
                r["sample"] = sample;
            } else {
                r["error"] = err;
            }
            st->root["quotes"] = r;
            (*step_spark)();
        });
    };

    (*step_quotes)();
}

void QuoteService::fetch_history(const QString& symbol, const QString& range, const QString& interval,
                                 std::function<void(bool, QVector<Bar>)> cb) {
    const QString key = symbol + "|" + range + "|" + interval;
    if (history_cache_.contains(key)) {
        if (cb) cb(true, history_cache_.value(key));
        return;
    }
    if (!PyEnv().is_ready()) {
        if (cb) cb(false, {});
        return;
    }

    auto ingest = [this, cb, key, symbol](bool ok, const QJsonValue& data, const QString& err) {
        if (!ok) note_failure(err);   // 历史失败也参与限流判定
        QVector<Bar> bars;
        if (ok && data.isArray()) {
            for (const auto& v : data.toArray()) {
                const QJsonObject o = v.toObject();
                Bar b;
                b.ts = static_cast<qint64>(o.value("timestamp").toDouble());
                b.open = o.value("open").toDouble();
                b.high = o.value("high").toDouble();
                b.low = o.value("low").toDouble();
                b.close = o.value("close").toDouble();
                b.volume = o.value("volume").toDouble();
                if (b.close > 0) bars.append(b);
            }
        }
        if (!bars.isEmpty()) history_cache_.insert(key, bars);
        emit history_ready(symbol, bars);
        if (cb) cb(!bars.isEmpty(), bars);
    };

    // 点开图表时也走常驻进程：省掉 1.5~2 秒解释器启动，K 线几乎瞬间出来
    if (daemon_ && daemon_->state() == QProcess::Running) {
        QJsonObject extra;
        extra.insert(QStringLiteral("symbol"), symbol);
        extra.insert(QStringLiteral("range"), range);
        extra.insert(QStringLiteral("interval"), interval);
        send_request(QStringLiteral("history"), extra, ingest);
        return;
    }

    run_script({QStringLiteral("history"), symbol, range, interval},
               [ingest](bool ok, QByteArray out, QString err) {
                   if (!ok) { ingest(false, {}, err); return; }
                   const QJsonDocument doc = QJsonDocument::fromJson(out);
                   if (doc.isArray()) {
                       ingest(true, doc.array(), {});
                   } else {
                       ingest(false, {}, doc.object().value("error").toString(QStringLiteral("历史数据获取失败")));
                   }
               });
}

} // namespace nb
