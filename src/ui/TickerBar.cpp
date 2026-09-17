#include "ui/TickerBar.h"

#include "ui/Theme.h"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>

namespace nb {

TickerBar::TickerBar(QWidget* parent) : QWidget(parent) {
    setFixedHeight(kHeight);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);

    timer_ = new QTimer(this);
    timer_->setInterval(4000);  // 每 4 秒滚动一格
    connect(timer_, &QTimer::timeout, this, [this]() {
        if (paused_ || cells_.isEmpty()) return;
        offset_ = (offset_ + 1) % cells_.size();
        rebuild_cells();
        update();
    });
    timer_->start();
}

void TickerBar::set_quotes(const QVector<Quote>& quotes) {
    quotes_ = quotes;
    rebuild_cells();
    update();
}

void TickerBar::set_paused(bool paused) {
    paused_ = paused;
    emit pause_toggled(paused);
}

void TickerBar::rebuild_cells() {
    cells_.clear();
    for (const auto& q : quotes_) {
        if (!q.ok) continue;
        Cell c;
        c.symbol = q.symbol;
        c.label = !q.alias.isEmpty() ? q.alias : (q.name.isEmpty() ? q.symbol : q.name);
        c.price = QString::number(q.price, 'f', q.price >= 100 ? 2 : 4);
        c.pct = QStringLiteral("%1%2%")
                    .arg(q.changePct >= 0 ? QStringLiteral("+") : QString())
                    .arg(q.changePct, 0, 'f', 2);
        c.color = theme::change_color(q.changePct);
        cells_.append(c);
    }
    if (cells_.isEmpty()) return;

    // 依次排布，从 offset_ 开始，形成滚动效果。宽度按三段文字实算，避免留白忽大忽小。
    const QFontMetrics fm_name(theme::ui_font(theme::fs::body));
    const QFontMetrics fm_sym(theme::ui_font(theme::fs::small));
    const QFontMetrics fm_num(theme::num_font(theme::fs::body, QFont::DemiBold));
    int x = 14;
    for (int i = 0; i < cells_.size(); ++i) {
        const int idx = (offset_ + i) % cells_.size();
        const Cell& c = cells_[idx];
        const int w = fm_name.horizontalAdvance(c.label) + fm_sym.horizontalAdvance(c.symbol) +
                      fm_num.horizontalAdvance(c.price) + fm_num.horizontalAdvance(c.pct) + 52;
        cells_[idx].rect = QRect(x, 0, w, kHeight);
        x += w;
        if (x > width() + 260) break;
    }
}

void TickerBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor("#0d0e11"));
    p.setPen(theme::border_dim());
    p.drawLine(0, height() - 1, width(), height() - 1);

    if (cells_.isEmpty()) {
        p.setPen(theme::text_faint());
        p.setFont(theme::ui_font(theme::fs::small));
        p.drawText(rect().adjusted(14, 0, -14, 0), Qt::AlignVCenter | Qt::AlignLeft,
                   QStringLiteral("行情数据准备中…"));
        return;
    }

    p.setClipRect(rect());
    const QFont f_name = theme::ui_font(theme::fs::body);
    const QFont f_sym = theme::ui_font(theme::fs::small);
    const QFont f_num = theme::num_font(theme::fs::body, QFont::DemiBold);
    const QFontMetrics fm_name(f_name), fm_sym(f_sym), fm_num(f_num);

    for (int i = 0; i < cells_.size(); ++i) {
        const Cell& c = cells_.at(i);
        if (c.rect.right() < 0 || c.rect.left() > width()) continue;

        // 悬停：给这一格一个淡背景，说明“可点开 K 线”
        if (i == hover_) p.fillRect(c.rect.adjusted(2, 4, -2, -4), theme::hover());

        // 三段排布：名称（正文色）· 代码（弱色）· 现价（亮色）· 涨跌幅（涨跌色）
        int x = c.rect.left() + 10;
        const int cy = c.rect.top();
        const int hh = c.rect.height();

        p.setFont(f_name);
        p.setPen(theme::text_dim());
        p.drawText(QRect(x, cy, fm_name.horizontalAdvance(c.label), hh),
                   Qt::AlignVCenter | Qt::AlignLeft, c.label);
        x += fm_name.horizontalAdvance(c.label) + 6;

        p.setFont(f_sym);
        p.setPen(theme::text_faint());
        p.drawText(QRect(x, cy, fm_sym.horizontalAdvance(c.symbol), hh),
                   Qt::AlignVCenter | Qt::AlignLeft, c.symbol);
        x += fm_sym.horizontalAdvance(c.symbol) + 10;

        p.setFont(f_num);
        p.setPen(theme::text());
        p.drawText(QRect(x, cy, fm_num.horizontalAdvance(c.price), hh),
                   Qt::AlignVCenter | Qt::AlignRight, c.price);
        x += fm_num.horizontalAdvance(c.price) + 8;

        p.setPen(c.color);
        p.drawText(QRect(x, cy, fm_num.horizontalAdvance(c.pct), hh),
                   Qt::AlignVCenter | Qt::AlignRight, c.pct);

        // 格与格之间的细竖线（比圆点更像行情条）
        p.setPen(theme::border_dim());
        p.drawLine(c.rect.right() - 1, c.rect.top() + 8, c.rect.right() - 1, c.rect.bottom() - 8);
    }

    if (paused_) {
        p.setPen(theme::warn());
        p.setFont(theme::ui_font(theme::fs::small));
        p.drawText(rect().adjusted(0, 0, -12, 0), Qt::AlignVCenter | Qt::AlignRight, QStringLiteral("已暂停"));
    }
}

void TickerBar::mousePressEvent(QMouseEvent* e) {
    for (const auto& c : cells_) {
        if (c.rect.contains(e->pos())) {
            emit symbol_activated(c.symbol);
            return;
        }
    }
}

void TickerBar::mouseMoveEvent(QMouseEvent* e) {
    int h = -1;
    for (int i = 0; i < cells_.size(); ++i) {
        if (cells_.at(i).rect.contains(e->pos())) {
            h = i;
            break;
        }
    }
    if (h != hover_) {
        hover_ = h;
        update();
    }
}

} // namespace nb
