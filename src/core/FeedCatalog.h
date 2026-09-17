#pragma once
// NewsBoard — 内置资讯源目录 + 自选行情默认分组
//
// 所有 RSS 地址都在本机实测过（HTTP 200 且返回真实 XML），财联社因为没有官方
// RSS，走它的签名接口（见 ClsFetcher）。列表可以在配置文件里覆盖：
//   ~/.config/newsboard/config.json

#include "core/Models.h"

#include <QHash>
#include <QString>
#include <QVector>

namespace nb::catalog {

/// 内置资讯源。分类键：markets / macro / tech / crypto / geopolitics / energy / regulatory / cn
QVector<FeedDef> builtin_feeds();

/// 分类键 → 中文名（界面用）
QString category_label(const QString& key);

/// 全部分类键（按界面展示顺序）
QStringList category_keys();

/// 默认行情分组
QVector<QuoteGroup> default_quote_groups();

/// 代码 → 中文别名（显示在右栏名称位；留空则用 Yahoo 返回的名称）
QHash<QString, QString> builtin_quote_aliases();

} // namespace nb::catalog
