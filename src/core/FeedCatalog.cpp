#include "core/FeedCatalog.h"

namespace nb::catalog {

namespace {
FeedDef rss(const char* id, const char* name, const char* url, const char* cat, const char* lang, int tier) {
    FeedDef f;
    f.id = QString::fromUtf8(id);
    f.name = QString::fromUtf8(name);
    f.url = QString::fromUtf8(url);
    f.category = QString::fromUtf8(cat);
    f.lang = QString::fromUtf8(lang);
    f.kind = FeedKind::Rss;
    f.tier = tier;
    return f;
}
} // namespace

// 只保留「实测更新快、信息密度高」的 14 个源（2026-09 逐条实测：HTTP 200、返回真实 XML、
// 最新条目在 1 小时以内，而不是“接口活着但内容早就停更”）。曾经收录过 45 个源，但其中大半是官方机构稿（美联储
// 7440 分钟一条）、周更或已失效的源——留着只会占满列表、让人以为“没在更新”，
// 于是全部删除，不做成可勾选项。中文只保留财联社。
QVector<FeedDef> builtin_feeds() {
    QVector<FeedDef> v;

    // ── 中文（唯一中文源：财联社电报）────────────────────────────────────
    {
        FeedDef cls;
        cls.id = "cls";
        cls.name = QString::fromUtf8("财联社电报");
        cls.url = {};  // 走签名接口，见 ClsFetcher
        cls.category = "cn";
        cls.lang = "zh";
        cls.kind = FeedKind::Cls;
        cls.tier = 1;
        v << cls;
    }

    // ── 市场（更新最快的主力）────────────────────────────────────────────
    v << rss("bloomberg-mkts", "Bloomberg Markets", "https://feeds.bloomberg.com/markets/news.rss", "markets", "en", 2);
    // WSJ 的 markets feed 已经僵死：HTTP 200 但 last-modified 停在 2025-01-27，
    // 条目 pubDate 全是 20 个月前，被 14 天留存直接过滤 → 界面上永远是空的。
    // 换成实测最活跃的地缘源（45 条、最新 1 分钟内），地缘消息对市场的影响也不小。
    v << rss("guardian-world", "The Guardian", "https://www.theguardian.com/world/rss",
             "geopolitics", "en", 2);
    v << rss("cnbc-finance", "CNBC Finance",
            "https://search.cnbc.com/rs/search/combinedcms/view.xml?partnerId=wrss01&id=100003114", "markets", "en", 2);
    v << rss("cnbc-world", "CNBC World",
            "https://search.cnbc.com/rs/search/combinedcms/view.xml?partnerId=wrss01&id=100727362", "markets", "en", 2);
    v << rss("seekingalpha", "Seeking Alpha", "https://seekingalpha.com/market_currents.xml", "markets", "en", 2);
    v << rss("investing-news", "Investing.com", "https://www.investing.com/rss/news.rss", "markets", "en", 2);
    v << rss("fxstreet", "FXStreet", "https://www.fxstreet.com/rss/news", "markets", "en", 2);
    v << rss("benzinga", "Benzinga", "https://www.benzinga.com/feed", "markets", "en", 2);

    // ── 科技 ──────────────────────────────────────────────────────────────
    v << rss("techcrunch", "TechCrunch", "https://techcrunch.com/feed/", "tech", "en", 2);

    // ── 加密 ──────────────────────────────────────────────────────────────
    v << rss("coindesk", "CoinDesk", "https://www.coindesk.com/arc/outboundfeeds/rss/", "crypto", "en", 2);
    v << rss("cointelegraph", "CoinTelegraph", "https://cointelegraph.com/rss", "crypto", "en", 2);
    v << rss("decrypt", "Decrypt", "https://decrypt.co/feed", "crypto", "en", 2);

    // ── 能源 ──────────────────────────────────────────────────────────────
    v << rss("oilprice", "OilPrice", "https://oilprice.com/rss/main", "energy", "en", 2);

    return v;
}

QString category_label(const QString& key) {
    if (key == "cn") return QString::fromUtf8("中文");
    if (key == "markets") return QString::fromUtf8("市场");
    if (key == "macro") return QString::fromUtf8("宏观");
    if (key == "regulatory") return QString::fromUtf8("监管");
    if (key == "geopolitics") return QString::fromUtf8("地缘");
    if (key == "tech") return QString::fromUtf8("科技");
    if (key == "crypto") return QString::fromUtf8("加密");
    if (key == "energy") return QString::fromUtf8("能源");
    return key;
}

QStringList category_keys() {
    return {"cn", "markets", "macro", "regulatory", "geopolitics", "tech", "crypto", "energy"};
}

QVector<QuoteGroup> default_quote_groups() {
    QVector<QuoteGroup> g;
    auto add = [&g](const char* id, const char* title, const QStringList& syms) {
        QuoteGroup q;
        q.id = QString::fromUtf8(id);
        q.title = QString::fromUtf8(title);
        q.symbols = syms;
        g << q;
    };

    // 以下 44 个代码均已实测可从 Yahoo 取到数据（2026-09 验证）。
    // 注意：费城半导体指数用 ^SOX（.SOX 在 Yahoo 取不到）。
    add("futures", QString::fromUtf8("指数与期货").toUtf8().constData(), {
        "ES=F",    // 标普500 期货主连
        "NQ=F",    // 纳斯达克100 期货主连
        "YM=F",    // 道琼斯 期货主连
        "^SOX",    // 费城半导体指数
    });

    add("compute", QString::fromUtf8("算力 / 云 / 软件").toUtf8().constData(), {
        "NVDA",  // 英伟达
        "AVGO",  // 博通（算力互联龙头）
        "MRVL",  // 迈威尔（算力互联龙二）
        "AMD",   // 超微
        "NBIS",  // Nebius（AI 云算力）
        "ORCL",  // 甲骨文（云数据库）
        "CRM",   // 赛富时（企业 SaaS）
        "FTNT",  // 飞塔信息（网络安全）
    });

    add("optics", QString::fromUtf8("光模块 / 光器件").toUtf8().constData(), {
        "AAOI",  // Applied Opto 光模块
        "LITE",  // Lumentum
        "COHR",  // Coherent
        "GLW",   // 康宁（玻璃基板 / 光纤）
        "AXTI",  // AXT（磷化铟衬底）
    });

    add("memory", QString::fromUtf8("存储 / 内存").toUtf8().constData(), {
        "MU",     // 美光
        "WDC",    // 西部数据
        "SNDK",   // 闪迪
        "KXIAY",  // 铠侠 ADR（OTC）
        "SKHY",   // SK 海力士 ADR（OTC）
    });

    add("semicap", QString::fromUtf8("半导体设备 / 材料 / 封测").toUtf8().constData(), {
        "ASML",  // 阿斯麦
        "AMAT",  // 应用材料
        "LRCX",  // 泛林
        "KLAC",  // 科磊（量测）
        "TER",   // 泰瑞达（设备 / 封测）
        "ACMR",  // ACM Research
        "TSEM",  // Tower 半导体
        "AMKR",  // 安靠（封测）
        "MTSI",  // MACOM（电芯片）
    });

    add("power", QString::fromUtf8("电力 / 能源 / 运输").toUtf8().constData(), {
        "GEV",  // GE Vernova（燃气轮机）
        "VRT",  // 维谛（液冷）
        "FRO",  // Frontline（油运）
    });

    add("other", QString::fromUtf8("其他权重股").toUtf8().constData(), {
        "META",   // Meta Platforms
        "GOOGL",  // 谷歌-A
        "TSLA",   // 特斯拉
        "INTC",   // 英特尔
        "TSM",    // 台积电 ADR
        "NOK",    // 诺基亚
        "MRNA",   // Moderna
        "APH",    // 安费诺
        "EWY",    // MSCI 韩国指数 ETF
        "SPCX",   // SpaceX
    });

    return g;
}

QHash<QString, QString> builtin_quote_aliases() {
    // 中文别名：显示在右栏每行的名称位（比 Yahoo 的英文全称更好认）。
    // 留空则回退到 Yahoo 返回的名称。
    return {
        {"ES=F", QString::fromUtf8("标普500期货")},
        {"NQ=F", QString::fromUtf8("纳指100期货")},
        {"YM=F", QString::fromUtf8("道指期货")},
        {"^SOX", QString::fromUtf8("费城半导体")},
        {"NVDA", QString::fromUtf8("英伟达")},
        {"AVGO", QString::fromUtf8("博通·算力互联")},
        {"MRVL", QString::fromUtf8("迈威尔·算力互联")},
        {"AMD", QString::fromUtf8("AMD")},
        {"NBIS", QString::fromUtf8("Nebius·AI云")},
        {"ORCL", QString::fromUtf8("甲骨文")},
        {"CRM", QString::fromUtf8("赛富时")},
        {"FTNT", QString::fromUtf8("飞塔·网安")},
        {"AAOI", QString::fromUtf8("光模块 AAOI")},
        {"LITE", QString::fromUtf8("Lumentum")},
        {"COHR", QString::fromUtf8("Coherent")},
        {"GLW", QString::fromUtf8("康宁·光纤")},
        {"AXTI", QString::fromUtf8("AXT·磷化铟")},
        {"MU", QString::fromUtf8("美光·存储")},
        {"WDC", QString::fromUtf8("西部数据")},
        {"SNDK", QString::fromUtf8("闪迪")},
        {"KXIAY", QString::fromUtf8("铠侠 ADR")},
        {"SKHY", QString::fromUtf8("SK海力士 ADR")},
        {"ASML", QString::fromUtf8("阿斯麦")},
        {"AMAT", QString::fromUtf8("应用材料")},
        {"LRCX", QString::fromUtf8("泛林")},
        {"KLAC", QString::fromUtf8("科磊·量测")},
        {"TER", QString::fromUtf8("泰瑞达")},
        {"ACMR", QString::fromUtf8("ACM Research")},
        {"TSEM", QString::fromUtf8("Tower半导体")},
        {"AMKR", QString::fromUtf8("安靠·封测")},
        {"MTSI", QString::fromUtf8("MACOM·电芯片")},
        {"GEV", QString::fromUtf8("GE Vernova·燃机")},
        {"VRT", QString::fromUtf8("维谛·液冷")},
        {"FRO", QString::fromUtf8("Frontline·油运")},
        {"META", QString::fromUtf8("Meta")},
        {"GOOGL", QString::fromUtf8("谷歌-A")},
        {"TSLA", QString::fromUtf8("特斯拉")},
        {"INTC", QString::fromUtf8("英特尔")},
        {"TSM", QString::fromUtf8("台积电 ADR")},
        {"NOK", QString::fromUtf8("诺基亚")},
        {"MRNA", QString::fromUtf8("Moderna")},
        {"APH", QString::fromUtf8("安费诺")},
        {"EWY", QString::fromUtf8("韩国ETF")},
        {"SPCX", QString::fromUtf8("SpaceX")},
    };
}

} // namespace nb::catalog
