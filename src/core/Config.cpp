#include "core/Config.h"

#include "core/FeedCatalog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace nb {

namespace {
QString kind_to_string(FeedKind k) {
    return k == FeedKind::Cls ? QStringLiteral("cls") : QStringLiteral("rss");
}
FeedKind kind_from_string(const QString& s) {
    return s == QLatin1String("cls") ? FeedKind::Cls : FeedKind::Rss;
}
} // namespace

Config& Config::instance() {
    static Config c;
    return c;
}

QString Config::project_root() const {
    // 先看启动目录：run.sh 会先 cd 到程序目录，所以这里通常就是 root。
    // 必须优先于 applicationDirPath()，因为后者是 Qt 通过 /proc/self/exe 解析出来的，
    // 一旦 lib/ 或 runtime/ 是软链接（整体软链部署、或开发时用暂存目录 + 软链），
    // 就会被解析成软链目标那一侧，导致 scripts/、config/、data/ 全部指错地方
    // （典型症状：守护进程/脚本明明更新了，程序却仍在跑旧的那一份）。
    const QDir cwd = QDir::current();
    if (QFileInfo::exists(cwd.filePath(QStringLiteral("runtime"))) ||
        QFileInfo::exists(cwd.filePath(QStringLiteral("CMakeLists.txt")))) {
        return cwd.absolutePath();
    }

    // 其次：可执行文件在 <root>/build/ 下，从它往上找带 runtime/ 或 CMakeLists.txt 的目录。
    // 找到就返回绝对路径；找不到（例如装到系统 bin 下）返回空，调用方回退 XDG。
    QDir d(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 4; ++i) {
        if (QFileInfo::exists(d.filePath(QStringLiteral("runtime"))) ||
            QFileInfo::exists(d.filePath(QStringLiteral("CMakeLists.txt")))) {
            return d.absolutePath();
        }
        if (!d.cdUp()) break;
    }
    return {};
}

QString Config::config_dir() const {
    // 允许把配置/数据挪到别处（例如把缓存放到 /tmp，或想在 U 盘上跑一份干净实例）：
    //   NB_CONFIG_DIR=/tmp/nb/config  NB_DATA_DIR=/tmp/nb/data  ./run.sh
    const QByteArray env_cfg = qgetenv("NB_CONFIG_DIR");
    if (!env_cfg.isEmpty()) return QString::fromLocal8Bit(env_cfg);

    const QString root = project_root();
    if (!root.isEmpty()) return root + "/config";
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/newsboard";
}

QString Config::data_dir() const {
    const QByteArray env_data = qgetenv("NB_DATA_DIR");
    if (!env_data.isEmpty()) return QString::fromLocal8Bit(env_data);

    const QString root = project_root();
    if (!root.isEmpty()) return root + "/data";
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/newsboard";
}

QString Config::cache_dir() const {
    // 交给 Python 当 XDG_CACHE_HOME：yfinance/curl_cffi 的 cookie 库（sqlite）
    // 需要可写目录，落到项目内既满足“自带运行环境”，也避免只读缓存导致
    // “attempt to write a readonly database” 这种整条行情链路失败。
    return data_dir() + "/cache";
}

QString Config::python_path() const {
    const QString root = project_root();
    if (!root.isEmpty()) return root + "/runtime/python/current/bin/python3";
    return data_dir() + "/runtime/python/current/bin/python3";
}

QString Config::script_path() const {
    // 优先项目根下的 scripts/，其次可执行文件旁边（兼容 build/scripts 布局）
    const QString root = project_root();
    const QString exe_dir = QCoreApplication::applicationDirPath();
    const QStringList candidates{
        root.isEmpty() ? QString() : root + "/scripts/yf_fetch.py",
        exe_dir + "/scripts/yf_fetch.py",
        exe_dir + "/../scripts/yf_fetch.py",
        exe_dir + "/../../scripts/yf_fetch.py",
    };
    for (const auto& c : candidates) {
        if (!c.isEmpty() && QFileInfo::exists(c)) return c;
    }
    return exe_dir + "/scripts/yf_fetch.py";
}

void Config::load() {
    QDir().mkpath(config_dir());
    QDir().mkpath(data_dir());
    // 一行启动记录：排查“读到的到底是哪一份配置/脚本”时最省事
    qInfo("[NewsBoard] root=%s config=%s data=%s",
          qUtf8Printable(project_root().isEmpty() ? QStringLiteral("(XDG)") : project_root()),
          qUtf8Printable(config_dir()), qUtf8Printable(data_dir()));

    // 默认值
    feeds_ = catalog::builtin_feeds();
    groups_ = catalog::default_quote_groups();
    aliases_ = catalog::builtin_quote_aliases();
    keywords_ = {};

    QFile f(config_dir() + "/config.json");
    if (f.exists() && f.open(QIODevice::ReadOnly)) {
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        f.close();

        // 源：内置源与用户改动合并（保留内置顺序，应用 enabled / 记住自定义源）
        const QJsonArray feeds_arr = root.value("feeds").toArray();
        if (!feeds_arr.isEmpty()) {
            QHash<QString, QJsonObject> saved;
            for (const auto& v : feeds_arr) {
                const QJsonObject o = v.toObject();
                saved.insert(o.value("id").toString(), o);
            }
            for (auto& bf : feeds_) {
                if (!saved.contains(bf.id)) continue;
                const QJsonObject o = saved.value(bf.id);
                bf.enabled = o.value("enabled").toBool(true);
                if (o.contains("name") && !o.value("name").toString().isEmpty())
                    bf.name = o.value("name").toString();
            }
            // 旧配置里曾经记录的、如今已从内置目录删掉的源：直接忽略，不要作为
            // “自定义源”复活——否则界面上会重新冒出一堆早就该消失的勾选项。
            // （内置目录是唯一事实来源；自定义源的添加入口已移除。）
        }

        // 行情分组
        const QJsonArray groups_arr = root.value("quote_groups").toArray();
        if (!groups_arr.isEmpty()) {
            QHash<QString, QStringList> syms;
            for (const auto& v : groups_arr) {
                const QJsonObject o = v.toObject();
                QStringList list;
                for (const auto& s : o.value("symbols").toArray()) list << s.toString();
                syms.insert(o.value("id").toString(), list);
            }
            for (auto& g : groups_) {
                if (syms.contains(g.id)) g.symbols = syms.value(g.id);
            }
        }

        // 别名：内置别名 + 用户改动（用户可在配置里覆盖）
        const QJsonObject al = root.value("quote_aliases").toObject();
        for (auto it = al.begin(); it != al.end(); ++it) aliases_.insert(it.key(), it.value().toString());

        QStringList kw;
        for (const auto& v : root.value("keywords").toArray()) kw << v.toString();
        keywords_ = kw;
        news_refresh_min_ = root.value("news_refresh_minutes").toInt(5);
        quote_refresh_sec_ = root.value("quote_refresh_seconds").toInt(5);
        compact_mode_ = root.value("compact_mode").toBool(true);
        left_visible_ = root.value("left_panel_visible").toBool(true);
        news_density_ = root.value("news_density").toInt(1);
        sort_by_change_ = root.value("sort_by_change").toBool(true);
        retention_days_ = root.value("news_retention_days").toInt(14);
        ticker_paused_ = root.value("ticker_paused").toBool(false);
        geometry_ = QByteArray::fromBase64(root.value("window_geometry").toString().toUtf8());
    }
}

void Config::save() const {
    QJsonObject root;

    QJsonArray feeds_arr;
    for (const auto& f : feeds_) {
        QJsonObject o;
        o["id"] = f.id;
        o["name"] = f.name;
        o["url"] = f.url;
        o["category"] = f.category;
        o["lang"] = f.lang;
        o["kind"] = kind_to_string(f.kind);
        o["enabled"] = f.enabled;
        o["tier"] = f.tier;
        feeds_arr.append(o);
    }
    root["feeds"] = feeds_arr;

    QJsonArray groups_arr;
    for (const auto& g : groups_) {
        QJsonObject o;
        o["id"] = g.id;
        o["title"] = g.title;
        QJsonArray syms;
        for (const auto& s : g.symbols) syms.append(s);
        o["symbols"] = syms;
        groups_arr.append(o);
    }
    root["quote_groups"] = groups_arr;

    QJsonObject al;
    for (auto it = aliases_.begin(); it != aliases_.end(); ++it) al[it.key()] = it.value();
    root["quote_aliases"] = al;

    QJsonArray kw;
    for (const auto& k : keywords_) kw.append(k);
    root["keywords"] = kw;

    root["news_refresh_minutes"] = news_refresh_min_;
    root["quote_refresh_seconds"] = quote_refresh_sec_;
    root["compact_mode"] = compact_mode_;
    root["left_panel_visible"] = left_visible_;
    root["news_density"] = news_density_;
    root["sort_by_change"] = sort_by_change_;
    root["news_retention_days"] = retention_days_;
    root["ticker_paused"] = ticker_paused_;
    root["window_geometry"] = QString::fromUtf8(geometry_.toBase64());

    QDir().mkpath(config_dir());
    QFile f(config_dir() + "/config.json");
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        f.close();
    }
}

void Config::set_feed_enabled(const QString& feed_id, bool enabled) {
    for (auto& f : feeds_) {
        if (f.id == feed_id) {
            f.enabled = enabled;
            break;
        }
    }
}

void Config::add_feed(const FeedDef& f) {
    for (auto& e : feeds_) {
        if (e.id == f.id) {
            e = f;
            return;
        }
    }
    feeds_ << f;
}

void Config::remove_feed(const QString& feed_id) {
    for (int i = 0; i < feeds_.size(); ++i) {
        if (feeds_.at(i).id == feed_id) {
            feeds_.remove(i);
            return;
        }
    }
}

bool Config::is_builtin_feed(const QString& feed_id) const {
    for (const auto& f : catalog::builtin_feeds()) {
        if (f.id == feed_id) return true;
    }
    return false;
}

void Config::set_group_symbols(const QString& group_id, const QStringList& symbols) {
    for (auto& g : groups_) {
        if (g.id == group_id) {
            g.symbols = symbols;
            return;
        }
    }
}

QStringList Config::all_symbols() const {
    QStringList out;
    for (const auto& g : groups_) {
        for (const auto& s : g.symbols) {
            if (!out.contains(s)) out << s;
        }
    }
    return out;
}

QString Config::quote_alias(const QString& symbol) const {
    return aliases_.value(symbol);
}

void Config::set_quote_alias(const QString& symbol, const QString& alias) {
    if (alias.isEmpty())
        aliases_.remove(symbol);
    else
        aliases_.insert(symbol, alias);
}

void Config::set_news_refresh_minutes(int m) {
    news_refresh_min_ = qBound(1, m, 120);
}
void Config::set_quote_refresh_seconds(int s) {
    quote_refresh_sec_ = qBound(3, s, 3600);   // 低到 3 秒（实时档）
}
void Config::set_highlight_keywords(const QStringList& k) {
    keywords_ = k;
}
void Config::set_news_retention_days(int d) {
    retention_days_ = qBound(1, d, 365);
}
void Config::set_ticker_paused(bool p) {
    ticker_paused_ = p;
}
void Config::set_window_geometry(const QByteArray& g) {
    geometry_ = g;
}

} // namespace nb
