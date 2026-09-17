#pragma once
// NewsBoard — 配色、字号阶梯与全局样式
//
// 设计基线参考了主流同类程序的做法：
//   · TradingView 自选列表：数字右对齐 + 等宽数字（tabular figures），行悬停高亮，
//     分组头带数量，颜色只用来表达涨跌
//   · Feedly / Reeder 新闻流：分类徽章做成药丸、来源与时间分离（时间右对齐）、
//     未读用左侧竖条而不是整行变色
//   · Linear / Notion：8px 间距栅格、极少量低对比描边、只有强调色是彩色的
//
// 原则：层级靠字号与灰度拉开，不靠花哨的颜色；数字一定等宽，否则刷新时整列会抖。

#include <QColor>
#include <QFont>
#include <QString>
#include <QtGlobal>

namespace nb::theme {

// ── 背景层次（从深到浅，一层只比上一层亮一点点）──────────────────────────
inline const QColor bg() { return QColor("#0b0b0d"); }          // 窗口底
inline const QColor panel() { return QColor("#101114"); }       // 面板底
inline const QColor panel_alt() { return QColor("#16181c"); }   // 次级块（表头、输入框）
inline const QColor hover() { return QColor("#1a1c21"); }       // 悬停
inline const QColor selected() { return QColor("#241f16"); }    // 选中（琥珀底调）

// ── 描边 ──────────────────────────────────────────────────────────────────
inline const QColor border() { return QColor("#24272d"); }
inline const QColor border_dim() { return QColor("#1a1c20"); }

// ── 文字（三级灰度，对比度按暗底调过）────────────────────────────────────
inline const QColor text() { return QColor("#e9eaec"); }        // 正文/标题
inline const QColor text_dim() { return QColor("#9ba2ab"); }    // 次要信息
inline const QColor text_faint() { return QColor("#6c7278"); }  // 最弱：时间、单位

// ── 强调与涨跌 ────────────────────────────────────────────────────────────
inline const QColor accent() { return QColor("#e08b12"); }       // 琥珀（唯一强调色）
inline const QColor accent_soft() { return QColor("#f0b060"); }  // 琥珀浅（高亮词）
inline const QColor up() { return QColor("#f04438"); }           // 涨 = 红（A 股习惯）
inline const QColor down() { return QColor("#12b76a"); }         // 跌 = 绿
inline const QColor flat() { return QColor("#9ba2ab"); }
inline const QColor unread() { return QColor("#f59e0b"); }
inline const QColor warn() { return QColor("#facc15"); }

/// 涨跌取色：红涨绿跌（国内习惯）。K 线、走势线、迷你走势共用。
inline QColor change_color(double v) {
    if (v > 0) return up();
    if (v < 0) return down();
    return flat();
}

// ── 字号阶梯（只保留四档，避免到处手写 pointSize）────────────────────────
namespace fs {
constexpr int meta = 10;     // 状态栏、最小注释
constexpr int small = 11;    // 次要信息：来源、时间、分组头
constexpr int body = 12;     // 正文：列表标题、行情名称、按钮
constexpr int title = 13;    // 新闻标题、浮层标题
constexpr int big = 15;      // 浮层大标题
} // namespace fs

/// 等宽数字（tabular figures）。行内数字刷新时不会左右跳动，列才对得齐。
inline QFont numeric(QFont f) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    f.setFeature(QFont::Tag("tnum"), 1);
#endif
    return f;
}

/// 按像素字号取字体（QSS 之外的自绘代码统一走这里）
QFont ui_font(int px, int weight = QFont::Normal);
/// 等宽数字版：在 ui_font 基础上开 tabular figures，数字刷新时不会左右跳
QFont num_font(int px, int weight = QFont::Normal);

QString global_qss();

} // namespace nb::theme
