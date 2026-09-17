#include "core/SymbolMatch.h"

#include "core/Config.h"

#include <QHash>
#include <QRegularExpression>

namespace nb::symbols {

namespace {

/// 常用英文公司名 → 代码。只收录自选标的，宁少勿滥（错配比漏配更烦人）。
const QHash<QString, QStringList>& english_names() {
    static const QHash<QString, QStringList> m{
        {"NVDA", {"nvidia"}},
        {"AVGO", {"broadcom"}},
        {"MRVL", {"marvell"}},
        {"NBIS", {"nebius"}},
        {"ORCL", {"oracle"}},
        {"CRM", {"salesforce"}},
        {"FTNT", {"fortinet"}},
        {"AAOI", {"applied optoelectronics"}},
        {"LITE", {"lumentum"}},
        {"COHR", {"coherent"}},
        {"GLW", {"corning"}},
        {"AXTI", {"axt", "axt inc"}},
        {"MU", {"micron"}},
        {"WDC", {"western digital"}},
        {"SNDK", {"sandisk", "sanDisk"}},
        {"KXIAY", {"kioxia"}},
        {"SKHY", {"sk hynix", "hynix"}},
        {"ASML", {"asml"}},
        {"AMAT", {"applied materials"}},
        {"LRCX", {"lam research"}},
        {"KLAC", {"kla"}},
        {"TER", {"teradyne"}},
        {"ACMR", {"acm research"}},
        {"TSEM", {"tower semiconductor"}},
        {"AMKR", {"amkor"}},
        {"MTSI", {"macom"}},
        {"GEV", {"ge vernova"}},
        {"VRT", {"vertiv"}},
        {"FRO", {"frontline"}},
        {"META", {"meta platforms", "facebook"}},
        {"GOOGL", {"alphabet", "google"}},
        {"TSLA", {"tesla"}},
        {"INTC", {"intel"}},
        {"TSM", {"tsmc", "taiwan semiconductor"}},
        {"NOK", {"nokia"}},
        {"MRNA", {"moderna"}},
        {"APH", {"amphenol"}},
        {"EWY", {"ishares msci south korea"}},
        {"SPCX", {"spacex", "space exploration technologies"}},
    };
    return m;
}

bool ascii_word_hit(const QString& lower, const QString& kw) {
    int from = 0;
    while (true) {
        const int i = lower.indexOf(kw, from);
        if (i < 0) return false;
        const bool left_ok = (i == 0) || !(lower.at(i - 1).isLetterOrNumber());
        const int end = i + kw.size();
        const bool right_ok = (end >= lower.size()) || !(lower.at(end).isLetterOrNumber());
        if (left_ok && right_ok) return true;
        from = i + 1;
    }
}

/// 一次性构建「代码 → 匹配词」表（配置变了要重建，所以带版本号缓存）
struct Table {
    QVector<QPair<QString, QStringList>> items;   // 保持配置顺序
    int stamp = -1;
};

const Table& table() {
    static Table t;
    // 用分组里标的数量 + 第一个代码当指纹：改配置（增删标的）后自动重建
    const auto groups = Config::instance().quote_groups();
    int n = 0;
    QString first;
    for (const auto& g : groups) {
        n += g.symbols.size();
        if (first.isEmpty() && !g.symbols.isEmpty()) first = g.symbols.first();
    }
    const int stamp = n * 131 + first.size();
    if (t.stamp == stamp) return t;

    t.items.clear();
    t.stamp = stamp;
    for (const auto& g : groups) {
        for (const auto& sym : g.symbols) {
            QStringList kws;
            const QString alias = Config::instance().quote_alias(sym);
            if (!alias.isEmpty() && alias != sym) kws << alias.toLower();
            for (const auto& e : english_names().value(sym)) kws << e.toLower();
            // 代码本身：去掉 =F / ^ 之类前缀后按词边界匹配（ES=F 这种不做正文匹配，太容易误报）
            if (!sym.contains(QLatin1Char('=')) && !sym.startsWith(QLatin1Char('^')))
                kws << sym.toLower();
            t.items.append({sym, kws});
        }
    }
    return t;
}

} // namespace

QStringList match(const QString& text) {
    if (text.isEmpty()) return {};
    const QString lower = text.toLower();
    QStringList out;
    for (const auto& it : table().items) {
        for (const auto& kw : it.second) {
            if (kw.isEmpty()) continue;
            // 中文关键词直接包含判断；ASCII 关键词要求词边界
            const bool hit = kw.at(0).unicode() > 0x2000 ? lower.contains(kw) : ascii_word_hit(lower, kw);
            if (hit) {
                if (!out.contains(it.first)) out << it.first;
                break;
            }
        }
    }
    return out;
}

QStringList keywords_for(const QString& symbol) {
    for (const auto& it : table().items) {
        if (it.first == symbol) return it.second;
    }
    return {};
}

} // namespace nb::symbols
