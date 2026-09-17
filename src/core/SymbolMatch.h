#pragma once
// NewsBoard — 新闻 ↔ 标的 匹配
//
// 为什么需要：RSS 里没有「这条新闻讲的是哪只股票」这种结构化字段（财联社有
// stock_list，但那是 A 股）。我们自己的 44 个自选标的，名称/别名/英文名都在手上，
// 直接在标题+摘要里做词表匹配就够了 —— 成本极低，而且命中的标的是我们真正在看的。

#include <QString>
#include <QStringList>

namespace nb::symbols {

/// 从一段文本里找出命中的自选标的，返回代码列表（按配置里的分组顺序，已去重）。
/// 匹配依据：代码本身（按词边界，避免 "AMD" 命中 "AMDOCS"）、
/// 中文别名（如「英伟达」）、以及常用英文公司名（如 nvidia / sk hynix）。
QStringList match(const QString& text);

/// 每个标的的全部匹配词（供调试/展示用）
QStringList keywords_for(const QString& symbol);

} // namespace nb::symbols
