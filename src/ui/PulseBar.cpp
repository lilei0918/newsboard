#include "ui/PulseBar.h"

#include "ui/Theme.h"

#include <QDateTime>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace nb {

namespace {
constexpr int kChartW = 62;   // 迷你走势宽度
}

PulseBar::PulseBar(QWidget* parent) : QWidget(parent) {
    // QWidget 子类要显式打开 WA_StyledBackground，QSS 里的背景才会真的画出来
    setAttribute(Qt::WA_StyledBackground, true);
    // 不带类型选择器的写法才作用于控件自身（带 "PulseBar{}" 时 QWidget 子类常常不生效）
    setStyleSheet(QStringLiteral("background:#0f1014; border-bottom:1px solid #1a1c20;"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 6, 10, 6);
    root->setSpacing(3);

    // ── 异动行 ──
    movers_row_ = new QWidget(this);
    movers_lay_ = new QHBoxLayout(movers_row_);
    movers_lay_->setContentsMargins(0, 0, 0, 0);
    movers_lay_->setSpacing(6);
    root->addWidget(movers_row_);

    // ── 热点行 ──
    hot_row_ = new QWidget(this);
    hot_lay_ = new QHBoxLayout(hot_row_);
    hot_lay_->setContentsMargins(0, 0, 0, 0);
    hot_lay_->setSpacing(6);
    root->addWidget(hot_row_);

    // ── 折叠开关（右上角，小到不抢视线）──
    toggle_ = new QPushButton(QStringLiteral("收起"), this);
    toggle_->setObjectName("ghost");
    toggle_->setCursor(Qt::PointingHandCursor);
    toggle_->setFocusPolicy(Qt::NoFocus);
    toggle_->setToolTip(QStringLiteral("收起/展开「异动 + 热点」这条脉冲带"));
    connect(toggle_, &QPushButton::clicked, this, [this]() {
        collapsed_ = !collapsed_;
        toggle_->setText(collapsed_ ? QStringLiteral("展开") : QStringLiteral("收起"));
        rebuild();
    });

    auto* top = qobject_cast<QVBoxLayout*>(layout());
    Q_UNUSED(top);
    root->addWidget(toggle_, 0, Qt::AlignRight);

    hide();   // 两条都为空时不占地方
}

QString PulseBar::rel_time(qint64 ts) {
    const qint64 d = qMax<qint64>(0, QDateTime::currentSecsSinceEpoch() - ts);
    if (d < 60) return QStringLiteral("刚刚");
    if (d < 3600) return QStringLiteral("%1 分钟前").arg(d / 60);
    if (d < 86400) return QStringLiteral("%1 小时前").arg(d / 3600);
    return QStringLiteral("%1 天前").arg(d / 86400);
}

void PulseBar::set_movers(const QVector<Mover>& movers) {
    movers_ = movers;
    rebuild();
}

void PulseBar::set_clusters(const QVector<HotCluster>& clusters) {
    clusters_ = clusters;
    rebuild();
}

void PulseBar::rebuild() {
    auto clear = [](QHBoxLayout* lay) {
        while (QLayoutItem* it = lay->takeAt(0)) {
            if (QWidget* w = it->widget()) w->deleteLater();
            delete it;
        }
    };
    clear(movers_lay_);
    clear(hot_lay_);

    auto make_label = [this](const QString& text, const QColor& color, bool bold) {
        auto* lb = new QLabel(text, this);
        lb->setStyleSheet(QStringLiteral("color:%1;font-size:11px;%2")
                              .arg(color.name(), bold ? QStringLiteral("font-weight:600;") : QString()));
        return lb;
    };

    // ── 异动：最近 5 分钟涨速最大的几个，点一下看 K 线 ──
    const int kMoversShown = 5;
    if (!movers_.isEmpty() && !collapsed_) {
        movers_lay_->addWidget(make_label(QStringLiteral("异动 5 分钟"), theme::text_faint(), false));
        int shown = 0;
        for (const auto& m : movers_) {
            if (shown++ >= kMoversShown) break;
            auto* b = new QPushButton(QStringLiteral("%1 %2%")
                                          .arg(m.label)
                                          .arg(m.pct5, 0, 'f', 2)
                                          .replace(QStringLiteral("+"), QStringLiteral("+")),
                                      this);
            b->setObjectName("mini");
            b->setCursor(Qt::PointingHandCursor);
            b->setToolTip(QStringLiteral("%1（%2）\n最近 %3 分钟涨速 %4%\n当日 %5%")
                              .arg(m.label, m.symbol)
                              .arg(m.span_sec / 60)
                              .arg(m.pct5, 0, 'f', 2)
                              .arg(m.pct, 0, 'f', 2));
            const QString color = m.pct5 > 0 ? QStringLiteral("#f04438") : QStringLiteral("#12b76a");
            b->setStyleSheet(QStringLiteral(
                                 "QPushButton{background:#15171c;border:1px solid #24272d;border-radius:4px;"
                                 "padding:1px 7px;font-size:11px;color:%1;}"
                                 "QPushButton:hover{border-color:%1;}")
                                 .arg(color));
            const QString sym = m.symbol;
            connect(b, &QPushButton::clicked, this, [this, sym]() { emit symbol_activated(sym); });
            movers_lay_->addWidget(b);
        }
        movers_lay_->addStretch(1);
    }

    // ── 热点：同一件事被几家在报（不是编辑推荐，所以如实写「N 源」）──
    const int kHotShown = 2;
    if (!clusters_.isEmpty() && !collapsed_) {
        hot_lay_->addWidget(make_label(QStringLiteral("多源热点"), theme::text_faint(), false));
        int shown = 0;
        for (const auto& c : clusters_) {
            if (shown++ >= kHotShown) break;
            QString t = c.title;
            if (t.size() > 42) t = t.left(41) + QStringLiteral("…");
            auto* b = new QPushButton(QStringLiteral("%1 源 · %2 ｜ %3")
                                          .arg(c.sources.size())
                                          .arg(rel_time(c.newest_ts), t),
                                      this);
            b->setObjectName("mini");
            b->setCursor(Qt::PointingHandCursor);
            b->setToolTip(QStringLiteral("%1\n\n%2 家来源在报：%3\n涉及自选标的：%4")
                              .arg(c.title)
                              .arg(c.sources.size())
                              .arg(c.sources.join(QStringLiteral("、")))
                              .arg(c.symbols.isEmpty() ? QStringLiteral("—")
                                                       : c.symbols.join(QStringLiteral("、"))));
            const QString id = c.item_id;
            connect(b, &QPushButton::clicked, this, [this, id]() { emit item_activated(id); });
            hot_lay_->addWidget(b);
        }
        hot_lay_->addStretch(1);
    }

    movers_row_->setVisible(!movers_.isEmpty() && !collapsed_);
    hot_row_->setVisible(!clusters_.isEmpty() && !collapsed_);
    toggle_->setVisible(!movers_.isEmpty() || !clusters_.isEmpty());

    if (qEnvironmentVariableIsSet("NB_PULSE_DEBUG")) {
        QFile f(QStringLiteral("/tmp/pulse_debug.log"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
            f.write(QStringLiteral("rebuild: movers=%1 clusters=%2 collapsed=%3\n")
                        .arg(movers_.size())
                        .arg(clusters_.size())
                        .arg(collapsed_)
                        .toUtf8());
        }
    }

    // 顶部一条细线，与新闻列表分开
    setVisible(!movers_.isEmpty() || !clusters_.isEmpty());
    updateGeometry();
}

} // namespace nb
