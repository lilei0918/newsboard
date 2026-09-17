#include "core/NewsAggregator.h"

#include "core/Config.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QXmlStreamReader>

#include <algorithm>

namespace nb {

namespace {
constexpr const char* kUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";
constexpr int kPerFeedTimeoutMs = 12000;
constexpr int kMaxConcurrent = 6;

QString strip_html(const QString& in) {
    QString s = in;
    s.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    s.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    s.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    s.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    s.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    s.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    s.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    s.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return s.trimmed();
}

/// 标题指纹：去掉空白/标点并小写，用于跨源去重
QString title_fingerprint(const QString& title) {
    QString t = title.toLower();
    t.remove(QRegularExpression(QStringLiteral("[\\s\\p{P}\\p{S}]")));
    return t.left(120);
}

qint64 parse_date(const QString& raw) {
    if (raw.isEmpty()) return 0;
    const QDateTime dt = QDateTime::fromString(raw, Qt::ISODate);
    if (dt.isValid()) return dt.toSecsSinceEpoch();
    // RFC 822（RSS 常见）
    static const QStringList formats{
        QStringLiteral("ddd, dd MMM yyyy HH:mm:ss t"),
        QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"),
        QStringLiteral("ddd, dd MMM yyyy HH:mm t"),
    };
    for (const auto& f : formats) {
        const QDateTime d2 = QDateTime::fromString(raw, f);
        if (d2.isValid()) return d2.toSecsSinceEpoch();
    }
    return QDateTime::currentSecsSinceEpoch();
}

QString make_id(const QString& feed_id, const QString& guid_or_link, const QString& title) {
    const QString seed = guid_or_link.isEmpty() ? title : guid_or_link;
    const QByteArray h = QCryptographicHash::hash(seed.toUtf8(), QCryptographicHash::Md5).toHex();
    return feed_id + ":" + QString::fromLatin1(h.left(16));
}
} // namespace

// ════════════════════════════════════════════════════════════════════════════
// RssFetcher
// ════════════════════════════════════════════════════════════════════════════

RssFetcher::RssFetcher(QNetworkAccessManager* nam, QObject* parent) : QObject(parent), nam_(nam) {}

void RssFetcher::fetch(const FeedDef& feed, std::function<void(FetchResult)> cb) {
    QNetworkRequest req{QUrl(feed.url)};
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(kUserAgent));
    req.setRawHeader("Accept", "application/rss+xml, application/atom+xml, application/xml, text/xml, */*");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(kPerFeedTimeoutMs);

    QNetworkReply* reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, feed, cb = std::move(cb)]() {
        reply->deleteLater();
        FetchResult r;
        r.feedId = feed.id;

        const QByteArray data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            r.error = QStringLiteral("HTTP %1 %2")
                          .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt())
                          .arg(reply->errorString());
            if (cb) cb(r);
            return;
        }

        const QByteArray head = data.trimmed().left(24).toLower();
        if (head.contains("<html") || head.contains("<!doctype")) {
            r.error = QStringLiteral("返回的是 HTML 页面（可能被拒绝访问）");
            if (cb) cb(r);
            return;
        }

        QString parse_err;
        r.items = parse(data, feed, &parse_err);
        r.ok = !r.items.isEmpty();
        if (!r.ok) r.error = parse_err.isEmpty() ? QStringLiteral("未解析到条目") : parse_err;
        if (cb) cb(r);
    });
}

QVector<NewsItem> RssFetcher::parse(const QByteArray& xml, const FeedDef& feed, QString* error) const {
    QVector<NewsItem> out;
    QXmlStreamReader x(xml);

    bool in_item = false;
    QString title, summary, link, guid, pub_date;

    auto flush = [&]() {
        if (!in_item) return;
        NewsItem it;
        it.title = strip_html(title).trimmed();
        it.summary = strip_html(summary).left(600);
        it.url = link;
        it.source = feed.name;
        it.sourceId = feed.id;
        it.category = feed.category;
        it.lang = feed.lang;
        it.ts = parse_date(pub_date);
        it.id = make_id(feed.id, guid.isEmpty() ? it.url : guid, it.title);
        if (it.is_valid()) out.append(it);
        title.clear();
        summary.clear();
        link.clear();
        guid.clear();
        pub_date.clear();
    };

    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            const QString name = x.name().toString().toLower();
            if (name == QLatin1String("item") || name == QLatin1String("entry")) {
                in_item = true;
            } else if (in_item && name == QLatin1String("title")) {
                title = x.readElementText(QXmlStreamReader::IncludeChildElements);
            } else if (in_item && (name == QLatin1String("description") || name == QLatin1String("summary"))) {
                const QString t = x.readElementText(QXmlStreamReader::IncludeChildElements);
                if (summary.isEmpty()) summary = t;
            } else if (in_item && name == QLatin1String("content")) {
                const QString t = x.readElementText(QXmlStreamReader::IncludeChildElements);
                if (summary.isEmpty()) summary = t;
            } else if (in_item && name == QLatin1String("link")) {
                // Atom 用 <link href="…"/>，RSS 用 <link>…</link>
                const QString href = x.attributes().value(QStringLiteral("href")).toString();
                const QString rel = x.attributes().value(QStringLiteral("rel")).toString();
                if (!href.isEmpty()) {
                    if (link.isEmpty() && (rel.isEmpty() || rel == QLatin1String("alternate"))) link = href;
                } else {
                    const QString t = x.readElementText();
                    if (link.isEmpty()) link = t;
                }
            } else if (in_item && name == QLatin1String("guid")) {
                guid = x.readElementText();
            } else if (in_item && (name == QLatin1String("pubdate") || name == QLatin1String("published") ||
                                   name == QLatin1String("updated") || name == QLatin1String("date"))) {
                const QString t = x.readElementText();
                if (pub_date.isEmpty()) pub_date = t;
            }
        } else if (x.isEndElement()) {
            const QString name = x.name().toString().toLower();
            if (name == QLatin1String("item") || name == QLatin1String("entry")) {
                flush();
                in_item = false;
            }
        }
    }

    if (out.isEmpty() && x.hasError() && error) *error = x.errorString();
    return out;
}

// ════════════════════════════════════════════════════════════════════════════
// ClsFetcher（财联社）
// ════════════════════════════════════════════════════════════════════════════

ClsFetcher::ClsFetcher(QNetworkAccessManager* nam, QObject* parent) : QObject(parent), nam_(nam) {}

QByteArray ClsFetcher::url_encode_sorted(const QVector<QPair<QString, QString>>& params) {
    QVector<QPair<QString, QString>> sorted = params;
    std::sort(sorted.begin(), sorted.end(),
              [](const QPair<QString, QString>& a, const QPair<QString, QString>& b) { return a.first < b.first; });

    QByteArray out;
    for (int i = 0; i < sorted.size(); ++i) {
        if (i) out += '&';
        out += QUrl::toPercentEncoding(sorted.at(i).first);
        out += '=';
        out += QUrl::toPercentEncoding(sorted.at(i).second);
    }
    return out;
}

QString ClsFetcher::sign_for(const QVector<QPair<QString, QString>>& params) {
    const QByteArray text = url_encode_sorted(params);
    const QByteArray sha1 = QCryptographicHash::hash(text, QCryptographicHash::Sha1).toHex();
    return QString::fromLatin1(QCryptographicHash::hash(sha1, QCryptographicHash::Md5).toHex());
}

void ClsFetcher::fetch(const FeedDef& feed, std::function<void(FetchResult)> cb) {
    QVector<QPair<QString, QString>> params{
        {"app", "CailianpressWeb"},
        {"category", ""},
        {"last_time", last_time_ > 0 ? QString::number(last_time_) : QString()},
        {"os", "web"},
        {"refresh_type", "1"},
        {"rn", "30"},
        {"sv", "7.7.5"},
    };
    const QByteArray query = url_encode_sorted(params) + "&sign=" + sign_for(params).toUtf8();

    QNetworkRequest req{QUrl(QStringLiteral("https://www.cls.cn/v1/roll/get_roll_list?") +
                             QString::fromLatin1(query))};
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(kUserAgent));
    req.setRawHeader("Accept", "application/json, text/plain, */*");
    req.setRawHeader("Referer", "https://www.cls.cn/telegraph");
    req.setRawHeader("x-app-ver", "7.7.5");
    req.setRawHeader("x-os", "web");
    req.setTransferTimeout(kPerFeedTimeoutMs);

    QNetworkReply* reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, feed, cb = std::move(cb)]() {
        reply->deleteLater();
        FetchResult r;
        r.feedId = feed.id;

        if (reply->error() != QNetworkReply::NoError) {
            r.error = reply->errorString();
            if (cb) cb(r);
            return;
        }

        const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonArray rolls = root.value("data").toObject().value("roll_data").toArray();
        if (rolls.isEmpty()) {
            r.error = QStringLiteral("接口未返回电报数据（errno=%1）")
                          .arg(root.value("errno").toVariant().toString());
            if (cb) cb(r);
            return;
        }

        for (const auto& v : rolls) {
            const QJsonObject o = v.toObject();
            if (o.value("is_ad").toInt() == 1) continue;

            NewsItem it;
            it.title = o.value("title").toString().trimmed();
            const QString brief = o.value("brief").toString();
            it.summary = strip_html(brief.isEmpty() ? o.value("content").toString() : brief).left(600);
            if (it.title.isEmpty()) {
                // 电报经常只有正文没有标题 —— 用正文首句当标题
                QString first = it.summary;
                const int cut = first.indexOf(QRegularExpression(QStringLiteral("[。；;\\n]")));
                if (cut > 8) first = first.left(cut + 1);
                it.title = first.left(120);
            }
            it.url = o.value("shareurl").toString();
            if (it.url.isEmpty()) {
                it.url = QStringLiteral("https://www.cls.cn/detail/%1").arg(o.value("id").toVariant().toString());
            }
            it.source = feed.name;
            it.sourceId = feed.id;
            it.category = "cn";
            it.lang = "zh";
            it.ts = static_cast<qint64>(o.value("ctime").toDouble());
            if (it.ts == 0) it.ts = static_cast<qint64>(o.value("modified_time").toDouble());
            it.id = QStringLiteral("cls:%1").arg(o.value("id").toVariant().toString());

            for (const auto& sv : o.value("stock_list").toArray()) {
                const QJsonObject so = sv.toObject();
                QString t = so.value("name").toString();
                if (t.isEmpty()) t = so.value("stock_id").toString();
                if (!t.isEmpty() && !it.tickers.contains(t)) it.tickers << t;
            }

            if (it.ts > 0) last_time_ = std::max<qint64>(last_time_, it.ts);
            if (it.is_valid()) r.items.append(it);
        }

        r.ok = !r.items.isEmpty();
        if (!r.ok) r.error = QStringLiteral("解析后无有效条目");
        if (cb) cb(r);
    });
}

// ════════════════════════════════════════════════════════════════════════════
// NewsAggregator
// ════════════════════════════════════════════════════════════════════════════

NewsAggregator::NewsAggregator(QObject* parent) : QObject(parent) {
    nam_ = new QNetworkAccessManager(this);
    rss_ = new RssFetcher(nam_, this);
    cls_ = new ClsFetcher(nam_, this);
}

void NewsAggregator::refresh_all(bool force) {
    Q_UNUSED(force);
    QVector<FeedDef> enabled;
    for (const auto& f : Config::instance().feeds()) {
        if (f.enabled) enabled << f;
    }
    if (enabled.isEmpty()) {
        in_flight_ = 0;
        return;
    }

    last_feed_total_ = enabled.size();
    last_feed_ok_ = 0;
    in_flight_ = enabled.size();
    last_refresh_ms_ = QDateTime::currentMSecsSinceEpoch();

    // 并发闸门：同时最多 kMaxConcurrent 条源在飞，其余排队。
    struct State {
        QVector<FeedDef> queue;
        int next = 0;
        int active = 0;
        int done = 0;
    };
    auto st = std::make_shared<State>();
    st->queue = enabled;

    auto pump = std::make_shared<std::function<void()>>();
    *pump = [this, st, pump]() {
        while (st->active < kMaxConcurrent && st->next < st->queue.size()) {
            const FeedDef feed = st->queue.at(st->next++);
            ++st->active;

            auto cb = [this, st, pump](FetchResult r) {
                ingest(r);
                --st->active;
                ++st->done;
                if (st->done >= st->queue.size()) {
                    in_flight_ = 0;
                    emit feed_health_changed();
                    save_cache();
                    return;
                }
                (*pump)();
            };

            if (feed.kind == FeedKind::Cls)
                cls_->fetch(feed, cb);
            else
                rss_->fetch(feed, cb);
        }
    };
    (*pump)();
}

void NewsAggregator::ingest(FetchResult r) {
    FeedHealth& h = health_[r.feedId];
    h.feedId = r.feedId;
    h.lastAttemptMs = QDateTime::currentMSecsSinceEpoch();
    h.lastItemCount = r.items.size();
    h.ok = r.ok;
    if (r.ok) {
        h.consecutiveFailures = 0;
        h.lastSuccessMs = h.lastAttemptMs;
        h.lastError.clear();
        ++last_feed_ok_;
    } else {
        ++h.consecutiveFailures;
        h.lastError = r.error;
    }

    for (const auto& it : r.items) {
        // 同源内按 id 去重
        if (by_id_.contains(it.id)) continue;
        // 跨源按标题指纹去重（保留先到的那条）
        const QString fp = title_fingerprint(it.title);
        if (!fp.isEmpty() && title_key_.contains(fp)) continue;
        title_key_.insert(fp, it.id);
        NewsItem copy = it;
        copy.read = read_ids_.contains(copy.id);
        by_id_.insert(copy.id, copy);
    }

    // 重建排序视图
    items_.clear();
    items_.reserve(by_id_.size());
    for (const auto& it : by_id_) items_.append(it);
    std::sort(items_.begin(), items_.end(), [](const NewsItem& a, const NewsItem& b) { return a.ts > b.ts; });
    prune();

    emit news_updated(items_.size(), last_feed_total_ - in_flight_, last_feed_total_);
}

void NewsAggregator::prune() {
    const qint64 cutoff = QDateTime::currentSecsSinceEpoch() -
                          static_cast<qint64>(Config::instance().news_retention_days()) * 86400;
    if (items_.isEmpty() || items_.last().ts >= cutoff) return;

    QVector<NewsItem> keep;
    keep.reserve(items_.size());
    for (const auto& it : items_) {
        if (it.ts >= cutoff) keep.append(it);
    }
    items_ = keep;
}

QVector<FeedHealth> NewsAggregator::health() const {
    QVector<FeedHealth> out;
    for (const auto& f : Config::instance().feeds()) {
        FeedHealth h = health_.value(f.id);
        h.feedId = f.id;
        out << h;
    }
    return out;
}

void NewsAggregator::mark_read(const QString& id, bool read) {
    if (read)
        read_ids_.insert(id);
    else
        read_ids_.remove(id);
    if (auto it = by_id_.find(id); it != by_id_.end()) it->read = read;
    for (auto& it : items_) {
        if (it.id == id) it.read = read;
    }
}

void NewsAggregator::mark_all_read() {
    for (auto it = by_id_.begin(); it != by_id_.end(); ++it) {
        it->read = true;
        read_ids_.insert(it.key());
    }
    for (auto& it : items_) it.read = true;
}

QVector<NewsItem> NewsAggregator::filtered(const QString& category, const QSet<QString>& source_ids,
                                           const QString& keyword) const {
    const QString kw = keyword.trimmed();
    QVector<NewsItem> out;
    out.reserve(items_.size());
    for (const auto& it : items_) {
        if (!category.isEmpty() && it.category != category) continue;
        if (!source_ids.isEmpty() && !source_ids.contains(it.sourceId)) continue;
        if (!kw.isEmpty()) {
            if (!it.title.contains(kw, Qt::CaseInsensitive) && !it.summary.contains(kw, Qt::CaseInsensitive))
                continue;
        }
        out.append(it);
    }
    return out;
}

// ── 缓存 ────────────────────────────────────────────────────────────────────

void NewsAggregator::load_cache() {
    if (loaded_cache_) return;
    loaded_cache_ = true;

    QFile f(Config::instance().data_dir() + "/news_cache.json");
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    f.close();

    // 只认还在内置目录里的源：目录里删掉的源（曾经 45 个 → 现在 14 个），
    // 它们的旧新闻必须一起消失 —— 否则界面上会继续出现“已经删掉的源”的条目，
    // 让人以为那些源还在后面跑。
    QSet<QString> known_ids;
    for (const auto& f : Config::instance().feeds()) known_ids.insert(f.id);
    int pruned = 0;

    for (const auto& v : root.value("items").toArray()) {
        const QJsonObject o = v.toObject();
        if (!known_ids.isEmpty() && !known_ids.contains(o.value("sourceId").toString())) {
            ++pruned;
            continue;                       // 源已下线：连条目一起丢掉
        }
        NewsItem it;
        it.id = o.value("id").toString();
        it.title = o.value("title").toString();
        it.summary = o.value("summary").toString();
        it.url = o.value("url").toString();
        it.source = o.value("source").toString();
        it.sourceId = o.value("sourceId").toString();
        it.category = o.value("category").toString();
        it.lang = o.value("lang").toString();
        it.ts = static_cast<qint64>(o.value("ts").toDouble());
        it.read = o.value("read").toBool(false);
        for (const auto& t : o.value("tickers").toArray()) it.tickers << t.toString();
        if (!it.id.isEmpty() && it.is_valid()) {
            by_id_.insert(it.id, it);
            if (it.read) read_ids_.insert(it.id);
            title_key_.insert(title_fingerprint(it.title), it.id);
        }
    }

    items_.clear();
    for (const auto& it : by_id_) items_.append(it);
    std::sort(items_.begin(), items_.end(), [](const NewsItem& a, const NewsItem& b) { return a.ts > b.ts; });
    if (pruned > 0) {
        qInfo("[NewsBoard] 缓存剪枝：丢弃 %d 条来自已下线源的新闻，保留 %d 条",
              pruned, int(items_.size()));
        save_cache();                       // 立刻回写，别让旧条目在文件里躺着
    }
    if (!items_.isEmpty()) emit news_updated(items_.size(), 0, 0);
}

void NewsAggregator::save_cache() const {
    QJsonArray arr;
    for (const auto& it : items_) {
        QJsonObject o;
        o["id"] = it.id;
        o["title"] = it.title;
        o["summary"] = it.summary;
        o["url"] = it.url;
        o["source"] = it.source;
        o["sourceId"] = it.sourceId;
        o["category"] = it.category;
        o["lang"] = it.lang;
        o["ts"] = static_cast<double>(it.ts);
        o["read"] = it.read;
        QJsonArray tk;
        for (const auto& t : it.tickers) tk.append(t);
        o["tickers"] = tk;
        arr.append(o);
    }
    QJsonObject root;
    root["saved_at"] = static_cast<double>(QDateTime::currentSecsSinceEpoch());
    root["items"] = arr;

    QDir().mkpath(Config::instance().data_dir());
    QFile f(Config::instance().data_dir() + "/news_cache.json");
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        f.close();
    }
}

} // namespace nb
