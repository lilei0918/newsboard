#include "ui/Theme.h"

#include <QFontDatabase>
#include <QFontMetrics>

namespace nb::theme {

namespace {
/// 应用统一字体族（与 QSS 里写的族名保持一致），只解析一次
QString ui_family() {
    static const QString fam = [] {
        const QStringList preferred{QStringLiteral("Noto Sans CJK SC"),
                                    QStringLiteral("Source Han Sans SC"),
                                    QStringLiteral("WenQuanYi Micro Hei"),
                                    QStringLiteral("Microsoft YaHei"),
                                    QStringLiteral("PingFang SC")};
        const QStringList have = QFontDatabase::families();
        for (const auto& f : preferred) {
            if (have.contains(f)) return f;
        }
        return QString();
    }();
    return fam;
}
} // namespace

QFont ui_font(int px, int weight) {
    QFont f;
    const QString fam = ui_family();
    if (!fam.isEmpty()) f.setFamily(fam);
    f.setPixelSize(px);              // 统一用像素，字号在与 QSS 混用时不会漂
    f.setWeight(QFont::Weight(weight));
    return f;
}

QFont num_font(int px, int weight) {
    return numeric(ui_font(px, weight));
}

QString global_qss() {
    return QStringLiteral(R"QSS(
/* ── 基础 ───────────────────────────────────────────────────────────────── */
* { font-family: "Noto Sans CJK SC", "Source Han Sans SC", "WenQuanYi Micro Hei", "Microsoft YaHei", "PingFang SC", sans-serif; }
QWidget { background: #0b0b0d; color: #e9eaec; font-size: 12px; }
QMainWindow, QDialog { background: #0b0b0d; }
QToolTip { background: #1a1c21; color: #e9eaec; border: 1px solid #2c3037; border-radius: 6px; padding: 6px 9px; font-size: 11px; }

/* ── 三栏分隔：1px 低对比，不抢视线 ─────────────────────────────────────── */
QSplitter::handle { background: #1a1c20; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }
QSplitter::handle:hover { background: #e08b12; }

/* ── 标题：面板标题用中性灰，区块标题用琥珀，都靠字重与字距拉开层级 ───── */
QLabel#panelTitle { color: #9ba2ab; font-size: 12px; font-weight: 600; letter-spacing: 0.5px; padding: 10px 10px 8px 10px; }
QLabel#sectionTitle { color: #e08b12; font-size: 11px; font-weight: 700; letter-spacing: 0.6px; padding: 10px 0 2px 0; }
QLabel#hint { color: #6c7278; font-size: 10px; padding: 0 0 2px 0; }

/* ── 滚动条：细、无轨道、悬停才明显 ─────────────────────────────────────── */
QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }
QScrollBar::handle:vertical { background: #2f3339; min-height: 28px; border-radius: 4px; }
QScrollBar::handle:vertical:hover { background: #454b53; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar:horizontal { background: transparent; height: 8px; margin: 0; }
QScrollBar::handle:horizontal { background: #2f3339; min-width: 28px; border-radius: 4px; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
QScrollArea { border: none; background: transparent; }

/* ── 列表：分隔线交给 delegate 自绘，这里只给底色与状态 ────────────────── */
QListWidget, QListView { background: #101114; border: none; outline: none; }
QListWidget::item { border: none; }
QListWidget::item:selected { background: #241f16; }
QListWidget::item:hover { background: #1a1c21; }

/* ── 输入框 / 下拉：暗色下必须一起改，否则下拉框会是系统浅色 ───────────── */
QLineEdit, QComboBox { background: #16181c; border: 1px solid #24272d; border-radius: 5px; padding: 5px 9px; color: #e9eaec; font-size: 12px; }
QLineEdit:focus, QComboBox:focus { border-color: #e08b12; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox::down-arrow { image: none; border-left: 4px solid transparent; border-right: 4px solid transparent; border-top: 4px solid #9ba2ab; width: 0; height: 0; margin-right: 4px; }
QComboBox QAbstractItemView { background: #16181c; border: 1px solid #24272d; border-radius: 6px; padding: 4px; color: #e9eaec; selection-background-color: #241f16; selection-color: #f0b060; outline: none; }

/* ── 按钮 ───────────────────────────────────────────────────────────────── */
QPushButton { background: #1a1c21; border: 1px solid #2a2d34; border-radius: 5px; padding: 4px 11px; color: #d5d8dc; font-size: 12px; }
QPushButton:hover { background: #22252b; border-color: #3a3e46; }
QPushButton:pressed { background: #14161a; }
QPushButton#accent { background: #e08b12; color: #17130a; border: none; font-weight: 600; }
QPushButton#accent:hover { background: #f09b23; }
QPushButton#chip { background: #16181c; border: 1px solid #24272d; border-radius: 11px; padding: 3px 10px; color: #9ba2ab; font-size: 11px; }
QPushButton#chip:hover { border-color: #3a3e46; color: #d5d8dc; }
QPushButton#chip:checked { background: #2b2110; border-color: #e08b12; color: #f0b060; }
QPushButton#ghost { background: transparent; border: none; color: #9ba2ab; padding: 2px 6px; font-size: 11px; }
QPushButton#ghost:hover { color: #e9eaec; }
QPushButton#mini { background: transparent; border: 1px solid #24272d; border-radius: 4px; padding: 1px 7px; color: #9ba2ab; font-size: 11px; }
QPushButton#mini:hover { color: #e9eaec; border-color: #3a3e46; }
QPushButton#mini:checked { background: #2b2110; border-color: #e08b12; color: #f0b060; }

/* ── 复选 ───────────────────────────────────────────────────────────────── */
QCheckBox { color: #b6bcc4; spacing: 7px; font-size: 12px; }
QCheckBox::indicator { width: 13px; height: 13px; border: 1px solid #31353c; border-radius: 3px; background: #14161a; }
QCheckBox::indicator:hover { border-color: #454b53; }
QCheckBox::indicator:checked { background: #e08b12; border-color: #e08b12; }

/* ── 状态栏：比正文更小一号，弱化成一条信息带 ───────────────────────────── */
QStatusBar { background: #0d0e11; color: #9ba2ab; border-top: 1px solid #1a1c20; font-size: 10px; }
QStatusBar::item { border: none; }
QStatusBar QLabel { color: #9ba2ab; font-size: 10px; padding: 0 2px; }
QStatusBar QPushButton#chip { padding: 1px 8px; font-size: 10px; border-radius: 9px; }

/* ── 浮层 ───────────────────────────────────────────────────────────────── */
QFrame#overlay { background: #121317; border: 1px solid #2c3037; border-radius: 8px; }
QFrame#separator { background: #1a1c20; }
)QSS");
}

} // namespace nb::theme
