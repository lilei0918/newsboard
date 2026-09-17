#include "ui/QuoteBoardPanel.h"

#include "core/Config.h"
#include "ui/Theme.h"

#include <QContextMenuEvent>
#include <QInputDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QToolTip>

namespace nb {

namespace {
constexpr int kHeaderH = 26;
constexpr int kRowH = 38;        // 宽松模式
constexpr int kRowHC = 30;       // 紧凑模式（默认）：够放「盘中% + 盘后%」两行
constexpr int kPadX = 10;
constexpr int kSparkW = 56;      // 迷你走势宽度
constexpr int kPctW = 72;        // 涨跌幅列（两行：盘中% 在上，盘后% 在下）
constexpr int kColGap = 10;      // 列间距
constexpr int kNameMinW = 96;    // 名称列优先保留的宽度；不够就先丢迷你走势
constexpr int kNameTinyW = 56;   // 名称列的硬下限（只够放“英伟达/AAOI”这种短名）

QString fmt_price(double v) {
    if (v == 0) return QStringLiteral("—");
    if (v >= 1000) return QString::number(v, 'f', 2);
    if (v >= 10) return QString::number(v, 'f', 2);
    if (v >= 1) return QString::number(v, 'f', 3);
    return QString::number(v, 'f', 4);
}

QString fmt_pct(double v) {
    return QStringLiteral("%1%2%").arg(v >= 0 ? QStringLiteral("+") : QString()).arg(v, 0, 'f', 2);
}

/// 把收盘序列画成迷你走势
void draw_spark(QPainter* p, const QRect& r, const QVector<double>& pts, const QColor& color) {
    if (pts.size() < 2) return;
    double lo = pts.first(), hi = pts.first();
    for (double v : pts) {
        lo = qMin(lo, v);
        hi = qMax(hi, v);
    }
    const double span = (hi - lo) > 1e-9 ? (hi - lo) : 1.0;

    QPainterPath path;
    for (int i = 0; i < pts.size(); ++i) {
        const double x = r.left() + (r.width() - 1) * double(i) / double(pts.size() - 1);
        const double y = r.bottom() - (r.height() - 4) * ((pts.at(i) - lo) / span) - 2;
        if (i == 0)
            path.moveTo(x, y);
        else
            path.lineTo(x, y);
    }
    p->setRenderHint(QPainter::Antialiasing, true);
    p->setPen(QPen(color, 1.4));
    p->drawPath(path);
    p->setRenderHint(QPainter::Antialiasing, false);
}
} // namespace

QuoteBoardPanel::QuoteBoardPanel(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setMinimumWidth(260);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

void QuoteBoardPanel::set_quotes(const QVector<Quote>& quotes) {
    quotes_ = quotes;
    rebuild_rows();
    updateGeometry();
    update();
}

void QuoteBoardPanel::set_loading(bool loading) {
    loading_ = loading;
    update();
}

int QuoteBoardPanel::content_height() const {
    int h = 6;
    const int rh = row_height();
    for (const auto& r : rows_) h += r.header ? kHeaderH : rh;
    return h + 6;
}

int QuoteBoardPanel::row_height() const {
    return Config::instance().compact_mode() ? kRowHC : kRowH;
}

void QuoteBoardPanel::refresh_layout() {
    rebuild_rows();
    update();
}

void QuoteBoardPanel::rebuild_rows() {
    rows_.clear();

    QHash<QString, const Quote*> by_sym;
    for (const auto& q : quotes_) by_sym.insert(q.symbol, &q);

    const int rh = row_height();
    const bool by_change = Config::instance().sort_by_change();

    int y = 6;
    for (const auto& g : Config::instance().quote_groups()) {
        Row hdr;
        hdr.header = true;
        hdr.group_id = g.id;
        hdr.group_title = g.title;
        hdr.group_count = g.symbols.size();
        hdr.rect = QRect(0, y, width(), kHeaderH);
        rows_.append(hdr);
        y += kHeaderH;

        // 组内排序：涨得最猛的排最前（跌得最狠的沉底）。
        // 没有数据的标的（停牌/接口失败）不带涨跌幅，统一放到组末尾，避免它们插在
        // 中间制造“跳来跳去”的视觉噪音。
        QStringList order = g.symbols;
        if (by_change) {
            std::stable_sort(order.begin(), order.end(), [&by_sym](const QString& a, const QString& b) {
                const bool ha = by_sym.contains(a) && by_sym.value(a)->ok;
                const bool hb = by_sym.contains(b) && by_sym.value(b)->ok;
                if (ha != hb) return ha;                       // 有数据的在前
                if (!ha) return false;
                return by_sym.value(a)->changePct > by_sym.value(b)->changePct;
            });
        }

        for (const auto& sym : order) {
            Row r;
            r.group_id = g.id;
            if (by_sym.contains(sym)) {
                r.quote = *by_sym.value(sym);
            } else {
                r.quote.symbol = sym;
                r.quote.name = sym;
            }
            r.rect = QRect(0, y, width(), rh);
            rows_.append(r);
            y += rh;
        }
    }
    setFixedHeight(content_height());
}

void QuoteBoardPanel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), theme::panel());

    if (rows_.isEmpty()) {
        p.setPen(theme::text_faint());
        p.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignTop | Qt::AlignLeft,
                   QStringLiteral("暂无行情数据"));
        return;
    }

    // 字体统一从字号阶梯取（像素字号），数字一律等宽：刷新时整列不会左右抖
    const QFont base = theme::ui_font(theme::fs::body);
    const QFont small = theme::ui_font(theme::fs::small);
    const QFont tiny = theme::ui_font(theme::fs::meta);
    const QFont num = theme::num_font(theme::fs::body, QFont::DemiBold);
    const QFont num_small = theme::num_font(theme::fs::small);
    const QFont num_tiny = theme::num_font(theme::fs::meta);
    const QFont hdr = theme::ui_font(theme::fs::small, QFont::DemiBold);
    Q_UNUSED(tiny);
    Q_UNUSED(num_tiny);

    for (int i = 0; i < rows_.size(); ++i) {
        const Row& r = rows_.at(i);
        const QRect rr(0, r.rect.top(), width(), r.rect.height());

        if (r.header) {
            p.fillRect(rr, QColor("#14161c"));   // 分组头比数据行略亮一点，形成“段”的感觉
            QFont hf = hdr;
            hf.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
            p.setFont(hf);
            // 分组标题是结构信息，用中性灰；琥珀只留给「选中/操作」
            p.setPen(theme::text_dim());
            p.drawText(rr.adjusted(kPadX, 0, -kPadX - 40, 0), Qt::AlignVCenter | Qt::AlignLeft,
                       r.group_title);
            // 分组右侧的条数：一眼知道这组有几个人，也方便判断有没有折叠
            p.setFont(num_small);
            p.setPen(theme::text_faint());
            p.drawText(rr.adjusted(kPadX, 0, -kPadX, 0), Qt::AlignVCenter | Qt::AlignRight,
                       QStringLiteral("%1").arg(r.group_count, 2, 10, QLatin1Char('0')));
            p.setPen(theme::border_dim());
            p.drawLine(rr.left(), rr.bottom(), rr.right(), rr.bottom());
            continue;
        }

        // 行状态：悬停淡背景、选中琥珀底调 + 左侧竖条（与新闻列表同一套语言）
        if (i == hover_) p.fillRect(rr, theme::hover());
        const bool sel = r.quote.symbol == selected_;
        if (sel) {
            p.fillRect(rr, theme::selected());
            p.fillRect(QRect(rr.left(), rr.top() + 4, 3, rr.height() - 8), theme::accent());
        }

        QColor cch = theme::change_color(r.quote.changePct);
        QColor spark_c = cch;
        spark_c.setAlpha(190);

        // ── 列布局 ────────────────────────────────────────────────────────
        // 只保留三样：名称 · 迷你走势 · 涨跌幅（两行）。
        // 不再显示任何价格（盘中价、盘后价都去掉，价格在悬浮提示与 K 线里看），
        // 涨跌幅列上行是「盘中涨跌幅」（主口径，排序也按它），
        // 下行小字是「盘前/盘后涨跌幅」（没有延长时段数据时该行留空）。
        const int right = rr.right() - kPadX;
        const QRect pct_r(right - kPctW, rr.top(), kPctW, rr.height());

        const bool has_ext = r.quote.ok && !r.quote.extLabel.isEmpty();
        // 名称最多能占到哪里（涨跌幅列左侧）；迷你走势能塞下就塞，塞不下就让给名称
        const int name_right = pct_r.left() - kColGap;
        const int spark_left = name_right - kColGap - kSparkW;
        const bool spark_fits = (spark_left - kColGap - kPadX) >= kNameMinW;
        if (spark_fits && r.quote.spark.size() > 1) {
            const QRect spark_r(spark_left, rr.top() + 6, kSparkW, rr.height() - 12);
            draw_spark(&p, spark_r, r.quote.spark, spark_c);
        }

        // 名称（代码）—— 占满左侧剩余空间
        const int name_w = qMax(40, (spark_fits ? spark_left : name_right) - kColGap - kPadX);
        // 名称：中文别名（正文色）+ 代码（小号弱色）。代码永远显示，别靠记忆认标的。
        // 代码紧跟在别名后面（而不是贴到列右侧），否则中间会留一大段空白。
        const QString alias = !r.quote.alias.isEmpty()
                                  ? r.quote.alias
                                  : ((r.quote.ok && r.quote.name != r.quote.symbol) ? r.quote.name
                                                                                    : QString());
        const QFontMetrics bfm(base);
        const QFontMetrics sfm(small);
        const QString main_txt = alias.isEmpty() ? r.quote.symbol : alias;
        const int main_nat = bfm.horizontalAdvance(main_txt);
        const int sym_nat = alias.isEmpty() ? 0 : sfm.horizontalAdvance(r.quote.symbol) + 7;
        // 代码最多占名称列的一半，长名优先
        const int sym_w = alias.isEmpty() ? 0 : qMin(sym_nat, qMax(0, name_w / 2));
        const int main_w = qMax(24, qMin(main_nat, name_w - sym_w));

        p.setFont(base);
        p.setPen(r.quote.ok ? theme::text() : theme::text_faint());
        p.drawText(QRect(kPadX, rr.top(), main_w, rr.height()), Qt::AlignVCenter | Qt::AlignLeft,
                   bfm.elidedText(main_txt, Qt::ElideRight, main_w));
        if (sym_w > 0) {
            p.setFont(small);
            p.setPen(theme::text_faint());
            p.drawText(QRect(kPadX + main_w + 7, rr.top(), sym_w, rr.height()),
                       Qt::AlignVCenter | Qt::AlignLeft,
                       sfm.elidedText(r.quote.symbol, Qt::ElideRight, sym_w));
        }

        // 涨跌幅（两行）：上行 = 盘中涨跌幅（主口径），下行 = 盘前/盘后涨跌幅（小字）
        // 用户要求：不要“盘后”文字、不要盘后价格、不要盘中价格，只把盘后涨跌幅放到盘中涨跌幅下面。
        {
            const int half = rr.height() / 2;
            const QRect up_r(pct_r.left(), rr.top(), pct_r.width(), half);
            const QRect dn_r(pct_r.left(), rr.top() + half, pct_r.width(), rr.height() - half);

            p.setFont(num);
            p.setPen(r.quote.ok ? cch : theme::text_faint());
            p.drawText(up_r, Qt::AlignVCenter | Qt::AlignRight,
                       r.quote.ok ? fmt_pct(r.quote.changePct) : QStringLiteral("—"));

            if (has_ext) {
                p.setFont(num_small);
                p.setPen(theme::change_color(r.quote.extPct));
                // 只放一个数（不带“盘后/盘前”字样）；想知道是哪个时段看悬浮提示
                p.drawText(dn_r, Qt::AlignVCenter | Qt::AlignRight, fmt_pct(r.quote.extPct));
            }
        }

        p.setPen(QPen(theme::border_dim()));
        p.drawLine(rr.left() + kPadX, rr.bottom(), rr.right() - kPadX, rr.bottom());
    }

    if (loading_) {
        p.setPen(theme::warn());
        p.setFont(theme::ui_font(theme::fs::small));
        p.drawText(rect().adjusted(0, 4, -kPadX, 0), Qt::AlignTop | Qt::AlignRight, QStringLiteral("刷新中…"));
    }
}

int QuoteBoardPanel::row_at(const QPoint& pos) const {
    for (int i = 0; i < rows_.size(); ++i) {
        if (rows_.at(i).rect.contains(pos)) return i;
    }
    return -1;
}

void QuoteBoardPanel::mousePressEvent(QMouseEvent* e) {
    const int i = row_at(e->pos());
    if (i < 0) return;
    const Row& r = rows_.at(i);
    if (r.header) return;
    selected_ = r.quote.symbol;
    update();
    emit symbol_activated(selected_);
}

void QuoteBoardPanel::mouseMoveEvent(QMouseEvent* e) {
    const int i = row_at(e->pos());
    if (i != hover_) {
        hover_ = i;
        update();
        if (i >= 0 && !rows_.at(i).header) {
            const Quote& q = rows_.at(i).quote;
            if (q.ok) {
                const QString sym = q.symbol;
                auto pct_vs = [](double price, double ref) -> QString {
                    if (price <= 0 || ref <= 0) return QStringLiteral("—");
                    const double p = (price - ref) / ref * 100.0;
                    return QStringLiteral("%1（%2%3%）")
                        .arg(price, 0, 'f', price >= 100 ? 2 : 4)
                        .arg(p >= 0 ? QStringLiteral("+") : QString())
                        .arg(p, 0, 'f', 2);
                };
                QString title = q.symbol;
                if (!q.alias.isEmpty() && !q.name.isEmpty() && q.name != q.symbol)
                    title = QStringLiteral("%1 · %2").arg(q.alias, q.name);
                else if (!q.alias.isEmpty())
                    title = QStringLiteral("%1 · %2").arg(q.alias, q.symbol);
                QString tip = QStringLiteral("%1（%2）\n时段：%3%4\n昨收 %5\n")
                                  .arg(title, sym, q.sessionLabel,
                                       q.sessionActive ? QStringLiteral("（正在交易）")
                                                       : QStringLiteral("（已收盘）"),
                                       fmt_price(q.prevClose));
                if (q.regularPrice > 0)
                    tip += QStringLiteral("盘中 %1\n").arg(pct_vs(q.regularPrice, q.prevClose));
                if (q.prePrice > 0) tip += QStringLiteral("盘前 %1\n").arg(pct_vs(q.prePrice, q.prevClose));
                if (q.postPrice > 0)
                    tip += QStringLiteral("盘后 %1\n").arg(pct_vs(q.postPrice, q.regularPrice));
                if (!q.extLabel.isEmpty()) {
                    tip += QStringLiteral("上排数字 = 盘中涨跌幅；下排小字 = %1涨跌幅\n").arg(q.extLabel);
                } else {
                    tip += QStringLiteral("上排数字 = 盘中涨跌幅（该品种无盘前/盘后数据）\n");
                }
                if (!q.sourceLabel.isEmpty())
                    tip += QStringLiteral("数据源：%1%2\n")
                               .arg(q.sourceLabel,
                                    q.delayed ? QStringLiteral("（源端延迟）") : QString());
                if (q.delayed && !q.delayNote.isEmpty())
                    tip += QStringLiteral("⚠ %1\n").arg(q.delayNote);
                tip += QStringLiteral("最高 %1  最低 %2\n成交量 %3")
                           .arg(fmt_price(q.high), fmt_price(q.low))
                           .arg(q.volume, 0, 'f', 0);
                if (q.dataTs > 0) {
                    const qint64 age_s = QDateTime::currentSecsSinceEpoch() - q.dataTs;
                    const QString age = age_s < 90 ? QStringLiteral("刚刚")
                                        : age_s < 3600
                                            ? QStringLiteral("%1 分钟前").arg(age_s / 60)
                                            : QStringLiteral("%1 小时前").arg(age_s / 3600);
                    tip += QStringLiteral("\n数据：%1（%2）")
                               .arg(age, QDateTime::fromSecsSinceEpoch(q.dataTs)
                                             .toString(QStringLiteral("MM-dd HH:mm")));
                }
                QToolTip::showText(e->globalPosition().toPoint(), tip, this);
            }
        }
    }
}

void QuoteBoardPanel::mouseDoubleClickEvent(QMouseEvent* e) {
    const int i = row_at(e->pos());
    if (i < 0 || rows_.at(i).header) return;
    selected_ = rows_.at(i).quote.symbol;
    emit symbol_activated(selected_);
}

void QuoteBoardPanel::contextMenuEvent(QContextMenuEvent* e) {
    const int i = row_at(e->pos());
    if (i < 0 || rows_.at(i).header) return;
    const Row r = rows_.at(i);
    if (r.group_id != QLatin1String("watch")) return;  // 只允许编辑自选分组

    QMenu menu(this);
    QAction* del = menu.addAction(QStringLiteral("从自选移除 %1").arg(r.quote.symbol));
    QAction* chosen = menu.exec(e->globalPos());
    if (chosen == del) emit remove_symbol_requested(r.group_id, r.quote.symbol);
}

// ── 容器 ────────────────────────────────────────────────────────────────────

QuoteBoardView::QuoteBoardView(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── 顶栏：一个排序开关，不占地方 ──
    auto* bar = new QWidget(this);
    bar->setObjectName("quoteBar");
    bar->setFixedHeight(34);
    auto* bl = new QHBoxLayout(bar);
    bl->setContentsMargins(10, 0, 8, 0);
    bl->setSpacing(6);

    auto* title = new QLabel(QStringLiteral("行情"), bar);
    title->setObjectName("quoteBarTitle");
    title->setStyleSheet(QStringLiteral("color:#9ba2ab;font-size:12px;font-weight:600;"
                                        "letter-spacing:0.5px;background:transparent;"));
    bl->addWidget(title);
    auto* sub = new QLabel(QStringLiteral("盘中涨跌幅排序"), bar);
    sub->setStyleSheet(QStringLiteral("color:#5f656c;font-size:10px;background:transparent;padding-top:1px;"));
    bl->addWidget(sub);
    bl->addStretch(1);

    sort_btn_ = new QPushButton(bar);
    sort_btn_->setObjectName("mini");
    sort_btn_->setCheckable(true);
    sort_btn_->setCursor(Qt::PointingHandCursor);
    sort_btn_->setFocusPolicy(Qt::NoFocus);
    sort_btn_->setToolTip(QStringLiteral(
        "组内按「盘中」涨跌幅从高到低排序（红涨在前、绿跌在后）。\n"
        "盘前/盘后的小字不参与排序，免得夜里排序全乱。"));
    sort_btn_->setFixedHeight(18);
    sort_btn_->setChecked(Config::instance().sort_by_change());
    sort_btn_->setText(sort_btn_->isChecked() ? QStringLiteral("涨跌幅 ↓")
                                              : QStringLiteral("原顺序"));
    connect(sort_btn_, &QPushButton::toggled, this, [this](bool on) {
        Config::instance().set_sort_by_change(on);
        Config::instance().save();
        sort_btn_->setText(on ? QStringLiteral("涨跌幅 ↓") : QStringLiteral("原顺序"));
        if (board_) board_->refresh_layout();
    });
    bl->addWidget(sort_btn_);
    root->addWidget(bar);

    scroll_ = new QScrollArea(this);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setWidgetResizable(true);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    board_ = new QuoteBoardPanel(scroll_);
    scroll_->setWidget(board_);
    root->addWidget(scroll_, 1);

    bar->setStyleSheet(QStringLiteral("#quoteBar { background: #0f1013; border-bottom: 1px solid #1a1c20; }"));
}

} // namespace nb
