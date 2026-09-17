#pragma once
// NewsBoard — 配置与缓存路径

#include "core/Models.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace nb {

/// 单例配置。
///
/// 设计目标是「整个目录自带运行环境」：程序、Python 解释器、yfinance 依赖、
/// 配置与缓存全部在项目目录内，拷贝整个目录即可运行，不需要任何安装步骤。
///
///   <项目根>/
///     ├── runtime/python/current/bin/python3   自带 CPython（含 yfinance）
///     ├── scripts/yf_fetch.py                  行情桥脚本
///     ├── build/NewsBoard                      本程序
///     ├── config/config.json                   用户配置
///     └── data/                                缓存、状态日志、yfinance 的 cookie 库
///
/// 项目根按可执行文件路径向上查找（含 runtime/ 或 CMakeLists.txt 的目录）；
/// 找不到时回退到 XDG 目录，保证装在系统路径下也能工作。
class Config {
  public:
    static Config& instance();

    void load();
    void save() const;

    // ── 路径 ──
    QString project_root() const;     // 空 = 未识别（回退 XDG）
    QString config_dir() const;
    QString data_dir() const;
    QString cache_dir() const;        // 作为 XDG_CACHE_HOME 传给 Python
    QString python_path() const;      // 自带的 Python 解释器
    QString script_path() const;      // yf_fetch.py

    // ── 源 ──
    QVector<FeedDef> feeds() const { return feeds_; }
    void set_feed_enabled(const QString& feed_id, bool enabled);
    void add_feed(const FeedDef& f);
    void remove_feed(const QString& feed_id);
    bool is_builtin_feed(const QString& feed_id) const;

    // ── 行情分组 ──
    QVector<QuoteGroup> quote_groups() const { return groups_; }
    void set_group_symbols(const QString& group_id, const QStringList& symbols);
    QStringList all_symbols() const;

    // ── 行情别名（中文名）──
    QString quote_alias(const QString& symbol) const;   // 空 = 用 Yahoo 名称
    void set_quote_alias(const QString& symbol, const QString& alias);

    // ── 偏好 ──
    int news_refresh_minutes() const { return news_refresh_min_; }
    void set_news_refresh_minutes(int m);
    int quote_refresh_seconds() const { return quote_refresh_sec_; }
    bool compact_mode() const { return compact_mode_; }         // 紧凑行高（右栏一屏看更多）
    bool left_panel_visible() const { return left_visible_; }   // 左栏（筛选）是否显示
    bool sort_by_change() const { return sort_by_change_; }     // 组内按涨跌幅排序
    void set_quote_refresh_seconds(int s);
    void set_compact_mode(bool on) { compact_mode_ = on; }
    void set_left_panel_visible(bool on) { left_visible_ = on; }
    void set_sort_by_change(bool on) { sort_by_change_ = on; }
    QStringList highlight_keywords() const { return keywords_; }
    void set_highlight_keywords(const QStringList& k);
    int news_retention_days() const { return retention_days_; }
    void set_news_retention_days(int d);
    bool ticker_paused() const { return ticker_paused_; }
    void set_ticker_paused(bool p);
    QByteArray window_geometry() const { return geometry_; }
    void set_window_geometry(const QByteArray& g);

  private:
    Config() = default;

    QVector<FeedDef> feeds_;
    QVector<QuoteGroup> groups_;
    QStringList keywords_;
    QHash<QString, QString> aliases_;
    int news_refresh_min_ = 5;
    int quote_refresh_sec_ = 5;      // 默认 5 秒：批量接口下就是准实时
    bool compact_mode_ = true;
    bool left_visible_ = true;
    bool sort_by_change_ = true;
    int retention_days_ = 14;
    bool ticker_paused_ = false;
    QByteArray geometry_;
    QString script_dir_override_;
};

} // namespace nb
