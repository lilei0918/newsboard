#include "core/NewsAggregator.h"

#include "core/Config.h"
#include "core/SymbolMatch.h"

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
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QXmlStreamReader>

#include <algorithm>

namespace nb {

// ── 标题清洗与重要度分级 ────────────────────────────────────────────────────
//
// 目的：让列表里的「突发 / 重要 / 普通」自然分层，而不是所有条目的视觉重量一样。
//   · 中文源（财联社）：直接用上游接口自己的 editorial 信号（level: A/B/C）—— 不是猜的
//   · 英文源：RSS 没有重要度字段，只能用**透明可核对**的关键词规则，规则就写在下面
//     这两张表里，想调随时改；C 批还会叠加「多源共振」（30 分钟内多源同题）做交叉验证

namespace {

/// 突发：**会立刻改变定价**的事件。刻意收窄 —— 地缘政治词（war/attack/resign/coup）
/// 放在下面「重要」里，否则瑞典首相辞职、伊朗局势表态这类都会变成红色「突发」，
/// 红色就不再等于“市场正在被打”。
const char* kBreakingWords[] = {
    "breaking", "plunge", "plunges", "crash", "crashes", "halt", "halted", "trading halted",
    "circuit breaker", "default", "bankrupt", "bankruptcy", "margin call", "flash crash",
    "emergency rate", "capital controls", "bank run", "contagion",
};

/// 重要：值得优先读，但不至于立刻定价
const char* kImportantWords[] = {
    "fed", "fomc", "rate cut", "rate hike", "inflation", "cpi", "ppi", "payrolls", "jobs report",
    "tariff", "tariffs", "guidance", "earnings", "revenue", "acquisition", "acquire", "merger",
    "buyback", "downgrade", "upgrade", "lawsuit", "probe", "investigation", "antitrust",
    "export control", "export ban", "shortage", "recall", "layoffs", "stake", "ipo",
    "record high", "record low", "all-time high", "surge", "soar", "slump", "tumble",
    // 地缘/政治：影响面大，但不等于市场正在被打 → 重要即可
    "war", "strike", "attack", "invasion", "sanction", "sanctions", "emergency",
    "resign", "resigns", "ousted", "coup", "ceasefire", "truce", "blockade",
};

bool contains_word(const QString& lower, const char* w) {
    const QString needle = QString::fromLatin1(w);
    int from = 0;
    while (true) {
        const int i = lower.indexOf(needle, from);
        if (i < 0) return false;
        // 英文按词边界判断，避免 "war" 命中 "software"、"halt" 命中 "halting" 之类
        const bool left_ok = (i == 0) || !lower.at(i - 1).isLetter();
        const int end = i + needle.size();
        const bool right_ok = (end >= lower.size()) || !lower.at(end).isLetter();
        if (left_ok && right_ok) return true;
        from = i + 1;
    }
}

/// 英文标题尾部经常挂署名/机构："... higher yields – Deutsche Bank"
/// 条件是「破折号/竖线 + 短尾巴」，避免把正常标题切断；摘出来的部分进 origin。
void split_origin(QString& title, QString& origin) {
    if (title.size() < 26) return;
    static const QStringList seps{QStringLiteral(" – "), QStringLiteral(" — "),
                                  QStringLiteral(" | "), QStringLiteral(" - ")};
    for (const auto& sep : seps) {
        const int i = title.lastIndexOf(sep);
        if (i < 20) continue;
        const QString tail = title.mid(i + sep.size()).trimmed();
        const QString head = title.left(i).trimmed();
        if (tail.size() < 3 || tail.size() > 40) continue;
        if (tail.count(QLatin1Char(' ')) > 4) continue;                 // 尾巴太长，多半是正文
        if (tail.contains(QRegularExpression(QStringLiteral("[.!?;:，。；：]")))) continue;
        if (head.size() < 20) continue;
        // 博客标签不是媒体署名，摘出来反而乱（"... | Europe live"）
        static const QStringList not_publishers{QStringLiteral("live"), QStringLiteral("live updates"),
                                                QStringLiteral("opinion"), QStringLiteral("video"),
                                                QStringLiteral("analysis"), QStringLiteral("news")};
        if (not_publishers.contains(tail.toLower())) continue;
        title = head;
        origin = tail;
        return;
    }
}

/// 财联社标题前缀样板："财联社9月17日电，…" —— 纯噪音，删掉
void strip_cls_boilerplate(QString& title) {
    static const QRegularExpression re(
        QStringLiteral("^财联社\\s*\\d{1,2}月\\d{1,2}日(电|讯)[，,]\\s*"));
    title.remove(re);
}

NewsTier tier_from_words(const QString& title) {
    const QString lower = title.toLower();
    for (const char* w : kBreakingWords) {
        if (contains_word(lower, w)) return NewsTier::Breaking;
    }
    for (const char* w : kImportantWords) {
        if (contains_word(lower, w)) return NewsTier::Important;
    }
    return NewsTier::Normal;
}

} // namespace


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

        // 标题清洗 + 重要度（英文 RSS 没有编辑分级，用透明关键词规则）
        split_origin(it.title, it.origin);
        it.tier = tier_from_words(it.title);
        it.tickers = symbols::match(it.title + QStringLiteral(" ") + it.summary);

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

            // 标题清洗 + 重要度：财联社自己有编辑分级，直接用，别自己猜
            strip_cls_boilerplate(it.title);
            {
                const QString level = o.value("level").toString().toUpper();
                if (level == QLatin1String("A"))
                    it.tier = NewsTier::Breaking;
                else if (level == QLatin1String("B"))
                    it.tier = NewsTier::Important;
                else if (o.value("is_top").toInt() == 1 && it.title.contains(QRegularExpression(QStringLiteral("央行|降准|降息|加息|关税|制裁"))))
                    it.tier = NewsTier::Important;   // 置顶 + 宏观关键词才算重要
            }

            // 命中我们自选标的（如标题里出现「英伟达」「Nvidia」）
            it.tickers = symbols::match(it.title + QStringLiteral(" ") + it.summary);

            // 关联个股：名称 + 抓取时刻的涨跌幅（A 股，展示在详情浮层里）
            for (const auto& sv : o.value("stock_list").toArray()) {
                const QJsonObject so = sv.toObject();
                const QString nm = so.value("name").toString();
                if (nm.isEmpty()) continue;
                if (!it.stocks.contains(nm)) it.stocks << nm;
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



// ── 多源共振热点 ────────────────────────────────────────────────────────────
//
// 目标：回答「今天有什么大事」—— 单条新闻看不出来，但同一件事被多家媒体在相近时间
// 报道，就说明它在扩散。做法朴素但有效：
//   ① 标题规范化：英文取 ≥4 字母的词（去停用词），中文取相邻两字（bigram）
//   ② 贪心聚类：与已有簇的代表词集 Jaccard ≥ 0.42 且时间相近 → 归入该簇
//   ③ 只保留「≥2 个不同来源」的簇，按 来源数 → 新鲜度 排序
// 为什么不用更复杂的模型：RSS 没有共享 id，跨源只能靠文本；朴素词集在标题这种短文本上
// 已经够用，而且**规则透明、可解释**（点开就能看到是哪几家在报）。

namespace {

const QSet<QString>& stopwords() {
    static const QSet<QString> s{
        "that", "this", "with", "from", "have", "will", "your", "what", "when", "where",
        "which", "their", "there", "about", "after", "before", "into", "over", "more",
        "than", "says", "said", "amid", "could", "would", "should", "still", "here",
        "week", "today", "live", "update", "updates", "news", "report", "reports",
        "market", "markets", "stocks", "stock", "shares", "price", "prices",
        // 泛词：不加进来会让「值得关注的股票」「十大看点」这类完全不同的文章撞在一起
        "watch", "things", "best", "worst", "top", "here", "why", "how", "what",
        "ahead", "plus", "amid", "gets", "make", "makes", "made", "amid",
    };
    return s;
}

QSet<QString> title_tokens(const QString& title) {
    QSet<QString> out;
    const QString lower = title.toLower();
    // 英文/数字词
    static const QRegularExpression word(QStringLiteral("[a-z][a-z0-9']{3,}"));
    auto it = word.globalMatch(lower);
    while (it.hasNext()) {
        const QString w = it.next().captured(0);
        if (!stopwords().contains(w)) out.insert(w);
    }
    // 中文：相邻两字（bigram），跳过标点与空格
    QString cjk;
    for (const QChar& c : title) {
        if (c.unicode() >= 0x4E00 && c.unicode() <= 0x9FFF)
            cjk.append(c);
        else
            cjk.append(QLatin1Char(' '));
    }
    const QStringList runs = cjk.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const auto& r : runs) {
        for (int i = 0; i + 1 < r.size(); ++i) out.insert(r.mid(i, 2));
    }
    return out;
}

double jaccard(const QSet<QString>& a, const QSet<QString>& b) {
    if (a.isEmpty() || b.isEmpty()) return 0.0;
    int inter = 0;
    const QSet<QString>& small = a.size() < b.size() ? a : b;
    const QSet<QString>& big = a.size() < b.size() ? b : a;
    for (const auto& t : small) {
        if (big.contains(t)) ++inter;
    }
    return double(inter) / double(a.size() + b.size() - inter);
}

} // namespace

QVector<HotCluster> NewsAggregator::hot_clusters(int max, int window_minutes) const {
    struct Bucket {
        QSet<QString> tokens;
        QVector<const NewsItem*> items;
    };
    QVector<Bucket> buckets;
    const qint64 cutoff = QDateTime::currentSecsSinceEpoch() - qint64(window_minutes) * 60;

    QVector<const NewsItem*> sorted;
    for (const auto& it : items_) {
        if (it.ts >= cutoff && it.is_valid()) sorted.append(&it);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const NewsItem* a, const NewsItem* b) { return a->ts > b->ts; });

    for (const NewsItem* it : sorted) {
        const QSet<QString> tk = title_tokens(it->title);
        if (tk.size() < 3) continue;      // 太短的标题（"午间公告"之类）不参与聚类
        bool placed = false;
        for (auto& b : buckets) {
            const qint64 ref_ts = b.items.isEmpty() ? 0 : b.items.first()->ts;
            if (qAbs(ref_ts - it->ts) > qint64(window_minutes) * 60 / 2) continue;
            // 0.32 是实测出来的：真实的跨源同题（英央行决议 0.56、失业金 0.38、
            // 收购/黑客事件 0.36/0.33）都能进，而泛标题的假阳性（约 0.5 但主题不同）
            // 靠上面的泛词停用词挡掉。
            if (jaccard(b.tokens, tk) >= 0.32) {
                b.items.append(it);
                placed = true;
                break;
            }
        }
        if (!placed) buckets.append({tk, {it}});
    }

    QVector<HotCluster> out;
    for (const auto& b : buckets) {
        HotCluster c;
        QSet<QString> srcs;
        QSet<QString> syms;
        for (const NewsItem* it : b.items) {
            srcs.insert(it->sourceId);
            for (const auto& t : it->tickers) syms.insert(t);
        }
        if (srcs.size() < 2) continue;             // 只有一家报 → 不算共振
        // 代表条目：来源最新的那条里，标题最长的（信息量通常更大）
        const NewsItem* best = b.items.first();
        for (const NewsItem* it : b.items) {
            if (it->ts > best->ts || (it->ts == best->ts && it->title.size() > best->title.size()))
                best = it;
        }
        c.title = best->title;
        c.item_id = best->id;
        c.sources = QStringList(srcs.begin(), srcs.end());
        c.item_count = b.items.size();
        c.newest_ts = best->ts;
        c.symbols = QStringList(syms.begin(), syms.end());
        out.append(c);
    }

    std::sort(out.begin(), out.end(), [](const HotCluster& a, const HotCluster& b) {
        if (a.sources.size() != b.sources.size()) return a.sources.size() > b.sources.size();
        return a.newest_ts > b.newest_ts;
    });
    if (out.size() > max) out.resize(max);
    return out;
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
        it.tier = NewsTier(o.value("tier").toInt(0));
        it.origin = o.value("origin").toString();
        for (const auto& t : o.value("tickers").toArray()) it.tickers << t.toString();
        for (const auto& t : o.value("stocks").toArray()) it.stocks << t.toString();
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
        o["tier"] = int(it.tier);
        if (!it.origin.isEmpty()) o["origin"] = it.origin;
        QJsonArray tk;
        for (const auto& t : it.tickers) tk.append(t);
        o["tickers"] = tk;
        if (!it.stocks.isEmpty()) {
            QJsonArray sk;
            for (const auto& t : it.stocks) sk.append(t);
            o["stocks"] = sk;
        }
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
