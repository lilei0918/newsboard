#pragma once
// NewsBoard — 数据模型

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

namespace nb {

/// 一条资讯。title/summary 一律保留原文（不翻译），界面文案才是中文。
/// 重要度分级：让「突发 / 重要 / 普通」在列表里自然分层。
/// 中文源（财联社）用上游接口自己的 level 字段，英文源用透明关键词规则 —— 不是猜的。
enum class NewsTier { Normal = 0, Important = 1, Breaking = 2 };

struct NewsItem {
    QString id;         // 稳定唯一键（源 id + 条目 guid/link 的指纹）
    QString title;      // 标题（原文）
    QString summary;    // 摘要（原文，已去 HTML）
    QString url;        // 原文链接
    QString source;     // 源显示名（如 Reuters / 财联社）
    QString sourceId;   // 源 id
    QString category;   // 归类的键（markets/macro/tech/crypto/geopolitics/energy/regulatory/cn）
    QString lang;       // "en" / "zh" / ...
    qint64 ts = 0;      // 发布/更新时间（UTC 秒）
    bool read = false;
    QStringList tickers;  // 关联标的（财联社 stock_list / 标题里命中的自选代码）

    // ── 视觉层级用（不改数据口径，只影响怎么显示）──
    NewsTier tier = NewsTier::Normal;  // 突发 / 重要 / 普通
    QString origin;                    // 标题尾部的媒体/机构署名（已从标题里摘出来）
    QStringList stocks;                // 财联社 stock_list：关联个股名（A 股，展示用）

    bool is_valid() const { return !title.isEmpty() && ts > 0; }
};

/// 一个报价。行情统一走 yfinance（scripts/yf_fetch.py）。
struct Quote {
    QString symbol;
    QString name;        // Yahoo 返回的名称
    QString alias;       // 配置里的中文别名（优先显示；空则用 name）
    QString group;      // indices / forex / crypto / commodities / watch
    double price = 0;          // = 盘中价（主显示，任何时候都是常规时段口径）
    double change = 0;
    double changePct = 0;      // = 盘中涨跌幅（主显示；排序、配色都用它）
    double high = 0;
    double low = 0;
    double prevClose = 0;
    double volume = 0;
    QString currency;
    QVector<double> spark;   // 迷你走势（收盘价序列）
    bool ok = false;
    qint64 ts = 0;

    // ── 交易时段（按本机时钟与交易所公布的窗口判定）──
    // pre / regular / post / closed
    QString session;
    QString sessionLabel;    // 盘前 / 盘中 / 盘后 / 收盘
    bool sessionActive = false;  // true = 该时段正在交易（标签用琥珀色），false = 已收盘（灰色）
    qint64 dataTs = 0;           // 所显示价格的 K 线时间（用于提示数据新鲜度）
    double prePrice = 0;     // 各时段最后成交价（悬浮提示用）
    double regularPrice = 0;
    double postPrice = 0;

    // ── 延长时段（盘前 / 盘后）：主行下面那行小字 ──
    // 盘中时段为空（此时盘中就是全部信息）；空字符串表示该标的没有延长时段数据
    // （指数、期货、OTC 等）。
    QString extSession;      // "pre" / "post" / ""
    QString extLabel;        // 盘前 / 盘后 / ""
    double extPrice = 0;
    double extPct = 0;
    qint64 extTs = 0;

    // ── 数据来源与新鲜度 ──
    // Yahoo 对 CME/CBOT 期货只给延迟 10 分钟的数据，所以期货改用新浪外盘实时接口；
    // 拿不到实时源时 delayed=true，界面会说明“为什么这些数字不动”。
    QString sourceLabel;     // "Yahoo" / "新浪实时"
    bool delayed = false;    // true = 已知源端延迟（不是本程序的刷新问题）
    QString delayNote;       // 延迟原因（悬浮提示里显示）
};

/// 热点簇：同一件事被多个源在相近时间报道时聚成一簇。
/// 这是「多源共振」，不是「编辑推荐」——所以界面上写「N 源 · M 分钟」而不是「重磅」。
struct HotCluster {
    QString title;          // 代表标题（最新、且最有信息量的那条）
    QStringList sources;    // 参与报道的源（去重）
    int item_count = 0;     // 命中的条目数
    qint64 newest_ts = 0;
    QString item_id;        // 代表条目的 id（点开看详情）
    QStringList symbols;    // 涉及的自选标的
};

/// 异动条目：用我们自己的 5 秒行情算出来的「最近在动」的标的。
/// 注意口径：这不是「新闻推动了市场」，而是「市场正在动」—— 两者不要混为一谈。
struct Mover {
    QString symbol;
    QString label;        // 中文别名（回退代码）
    double pct5 = 0;      // 最近 5 分钟涨跌幅（%）
    double pct = 0;       // 当日（盘中级）涨跌幅（%）
    int span_sec = 0;     // 实际用于计算的跨度（秒），说明数据够不够
};

/// 自选分组：一个分组 = 看板里的一段
struct QuoteGroup {
    QString id;          // indices / forex / crypto / commodities / watch
    QString title;       // 界面显示的中文标题
    QStringList symbols; // Yahoo 代码
    bool collapsible = true;
};

/// 资讯源定义
enum class FeedKind {
    Rss,  // 标准 RSS / Atom
    Cls,  // 财联社签名接口（无 RSS）
};

struct FeedDef {
    QString id;
    QString name;        // 显示名
    QString url;         // RSS 地址；Cls 类型此字段留空
    QString category;    // markets/macro/tech/crypto/geopolitics/energy/regulatory/cn
    QString lang;        // en / zh
    FeedKind kind = FeedKind::Rss;
    bool enabled = true;
    int tier = 2;        // 1 官方/通讯社，2 主流媒体，3 博客/聚合
};

/// 源的运行期健康度
struct FeedHealth {
    QString feedId;
    bool ok = false;
    int consecutiveFailures = 0;
    int lastItemCount = 0;
    qint64 lastAttemptMs = 0;
    qint64 lastSuccessMs = 0;
    QString lastError;
};

/// 历史 K 线单根
struct Bar {
    qint64 ts = 0;
    double open = 0, high = 0, low = 0, close = 0, volume = 0;
};

} // namespace nb
