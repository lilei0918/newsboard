#pragma once
// NewsBoard — 资讯抓取与聚合
//
//  · RssFetcher：单条 RSS/Atom 抓取 + 解析（QXmlStreamReader）
//  · ClsFetcher：财联社电报（无官方 RSS，走其签名接口）
//  · NewsAggregator：并发调度所有启用源、去重、排序、过滤、落盘缓存

#include "core/Models.h"

#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QVector>

#include <functional>

class QNetworkReply;

namespace nb {

/// 单源抓取结果
struct FetchResult {
    QString feedId;
    bool ok = false;
    QVector<NewsItem> items;
    QString error;
};

/// ── RSS / Atom ──────────────────────────────────────────────────────────────
class RssFetcher : public QObject {
    Q_OBJECT
  public:
    explicit RssFetcher(QNetworkAccessManager* nam, QObject* parent = nullptr);

    /// 抓取一条源；回调在 GUI 线程执行。
    void fetch(const FeedDef& feed, std::function<void(FetchResult)> cb);

  private:
    QVector<NewsItem> parse(const QByteArray& xml, const FeedDef& feed, QString* error) const;

    QNetworkAccessManager* nam_;
};

/// ── 财联社电报 ──────────────────────────────────────────────────────────────
/// 财联社没有官方 RSS，网页是前端异步渲染。它自己的接口带签名：
///   sign = md5( hex(sha1( "app=..&category=..&os=web&...")) )   ← 参数按 key 排序
///   GET https://www.cls.cn/v1/roll/get_roll_list?<params>&sign=<sign>
/// Qt 自带 SHA1/MD5（QCryptographicHash），所以不需要任何第三方库。
class ClsFetcher : public QObject {
    Q_OBJECT
  public:
    explicit ClsFetcher(QNetworkAccessManager* nam, QObject* parent = nullptr);

    void fetch(const FeedDef& feed, std::function<void(FetchResult)> cb);

  private:
    static QString sign_for(const QVector<QPair<QString, QString>>& params);
    static QByteArray url_encode_sorted(const QVector<QPair<QString, QString>>& params);

    QNetworkAccessManager* nam_;
    qint64 last_time_ = 0;  // 翻页游标（秒）
};

/// ── 聚合器 ──────────────────────────────────────────────────────────────────
class NewsAggregator : public QObject {
    Q_OBJECT
  public:
    explicit NewsAggregator(QObject* parent = nullptr);

    /// 全量刷新（并发上限 6，单源 12s 超时，失败重试 1 次）
    void refresh_all(bool force = false);

    /// 已聚合的新闻（按时间倒序、已去重、已应用保留期）
    QVector<NewsItem> items() const { return items_; }

    QVector<FeedHealth> health() const;
    int feed_count() const { return last_feed_total_; }
    int feed_ok_count() const { return last_feed_ok_; }
    qint64 last_refresh_ms() const { return last_refresh_ms_; }
    bool refreshing() const { return in_flight_ > 0; }

    void mark_read(const QString& id, bool read);
    void mark_all_read();

    /// 过滤后的视图（分类为空 = 全部；源集合为空 = 全部；关键词为空 = 不过滤）
    /// 多源共振热点：默认看最近 6 小时，同题（词集相似度 ≥ 0.42）且时间相近的条目聚成一簇，
    /// 只保留「至少 2 个不同来源」的簇，按来源数 + 新鲜度排序。
    QVector<HotCluster> hot_clusters(int max = 3, int window_minutes = 360) const;

    QVector<NewsItem> filtered(const QString& category, const QSet<QString>& source_ids,
                               const QString& keyword) const;

    void load_cache();
    void save_cache() const;

  signals:
    /// 每批源返回就发一次，界面可增量刷新
    void news_updated(int total_items, int feeds_done, int feeds_total);
    void feed_health_changed();

  private:
    void ingest(FetchResult r);
    void prune();

    QNetworkAccessManager* nam_ = nullptr;
    RssFetcher* rss_ = nullptr;
    ClsFetcher* cls_ = nullptr;

    QHash<QString, NewsItem> by_id_;      // id → item（去重）
    QHash<QString, QString> title_key_;   // 标题指纹 → id（跨源去重）
    QHash<QString, FeedHealth> health_;
    QSet<QString> read_ids_;

    QVector<NewsItem> items_;
    int in_flight_ = 0;
    int last_feed_total_ = 0;
    int last_feed_ok_ = 0;
    qint64 last_refresh_ms_ = 0;
    bool loaded_cache_ = false;
};

} // namespace nb
