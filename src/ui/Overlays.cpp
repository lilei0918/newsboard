#include "ui/Overlays.h"

#include "ui/Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

namespace nb {

// ════════════════════════════════════════════════════════════════════════════
// 新闻详情
// ════════════════════════════════════════════════════════════════════════════

NewsDetailOverlay::NewsDetailOverlay(QWidget* parent) : QFrame(parent) {
    setObjectName("overlay");
    setFrameShape(QFrame::NoFrame);
    hide();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 12);
    root->setSpacing(8);

    auto* top = new QHBoxLayout();
    title_ = new QLabel(this);
    title_->setWordWrap(true);
    title_->setStyleSheet(QStringLiteral("font-size:15px;font-weight:600;color:#e9eaec;line-height:22px;"));
    top->addWidget(title_, 1);
    auto* close = new QPushButton(QStringLiteral("✕"), this);
    close->setObjectName("ghost");
    close->setCursor(Qt::PointingHandCursor);
    connect(close, &QPushButton::clicked, this, [this]() { hide_overlay(); });
    top->addWidget(close, 0, Qt::AlignTop);
    root->addLayout(top);

    meta_ = new QLabel(this);
    meta_->setStyleSheet(QStringLiteral("color:#9ba2ab;font-size:11px;"));
    root->addWidget(meta_);

    body_ = new QTextBrowser(this);
    body_->setOpenExternalLinks(false);
    body_->setStyleSheet(QStringLiteral("QTextBrowser{background:#16181c;border:1px solid #24272d;border-radius:6px;"
                                        "border-radius:4px;padding:10px;color:#d8d8d8;}"));
    root->addWidget(body_, 1);

    auto* btns = new QHBoxLayout();
    open_btn_ = new QPushButton(QStringLiteral("在浏览器打开原文"), this);
    open_btn_->setObjectName("accent");
    open_btn_->setCursor(Qt::PointingHandCursor);
    connect(open_btn_, &QPushButton::clicked, this, [this]() {
        if (!current_.url.isEmpty()) QDesktopServices::openUrl(QUrl(current_.url));
    });
    btns->addWidget(open_btn_);

    copy_btn_ = new QPushButton(QStringLiteral("复制标题"), this);
    copy_btn_->setCursor(Qt::PointingHandCursor);
    connect(copy_btn_, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(current_.title);
        copy_btn_->setText(QStringLiteral("已复制"));
    });
    btns->addWidget(copy_btn_);
    btns->addStretch();
    auto* hint = new QLabel(QStringLiteral("Esc 关闭"), this);
    hint->setStyleSheet(QStringLiteral("color:#6c7278;font-size:11px;"));
    btns->addWidget(hint);
    root->addLayout(btns);
}

void NewsDetailOverlay::show_item(const NewsItem& item) {
    current_ = item;
    title_->setText(item.title);

    const QDateTime dt = QDateTime::fromSecsSinceEpoch(item.ts);
    QString meta = QStringLiteral("%1  ·  %2").arg(item.source, dt.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    if (!item.tickers.isEmpty()) meta += QStringLiteral("  ·  关联标的：") + item.tickers.join(QStringLiteral("、"));
    meta_->setText(meta);

    QString body = item.summary;
    if (body.isEmpty()) body = QStringLiteral("（该源未提供摘要，点下方按钮打开原文）");
    body_->setPlainText(body);

    copy_btn_->setText(QStringLiteral("复制标题"));
    show();
    raise();
}

void NewsDetailOverlay::hide_overlay() {
    hide();
    emit closed();
}

void NewsDetailOverlay::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {
        hide_overlay();
        return;
    }
    QFrame::keyPressEvent(e);
}

// ════════════════════════════════════════════════════════════════════════════
// 行情图表
// ════════════════════════════════════════════════════════════════════════════

ChartOverlay::ChartOverlay(QWidget* parent) : QFrame(parent) {
    setObjectName("overlay");
    setFrameShape(QFrame::NoFrame);
    hide();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(8);

    auto* top = new QHBoxLayout();
    title_ = new QLabel(QStringLiteral("行情图表"), this);
    title_->setStyleSheet(QStringLiteral("font-size:14px;font-weight:600;color:#e9eaec;"));
    top->addWidget(title_, 1);
    auto* close = new QPushButton(QStringLiteral("✕"), this);
    close->setObjectName("ghost");
    close->setCursor(Qt::PointingHandCursor);
    connect(close, &QPushButton::clicked, this, [this]() { hide_overlay(); });
    top->addWidget(close);
    root->addLayout(top);

    auto* ranges = new QHBoxLayout();
    ranges->setSpacing(4);
    const QVector<QPair<QString, QPair<QString, QString>>> defs{
        {QStringLiteral("1月"), {QStringLiteral("1mo"), QStringLiteral("1d")}},
        {QStringLiteral("3月"), {QStringLiteral("3mo"), QStringLiteral("1d")}},
        {QStringLiteral("6月"), {QStringLiteral("6mo"), QStringLiteral("1d")}},
        {QStringLiteral("1年"), {QStringLiteral("1y"), QStringLiteral("1wk")}},
        {QStringLiteral("5年"), {QStringLiteral("5y"), QStringLiteral("1mo")}},
    };
    for (const auto& d : defs) {
        auto* b = new QPushButton(d.first, this);
        b->setObjectName("chip");
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setChecked(d.second.first == range_);
        connect(b, &QPushButton::clicked, this, [this, b, d]() {
            range_ = d.second.first;
            interval_ = d.second.second;
            for (auto* sib : findChildren<QPushButton*>()) {
                if (sib != b && sib->objectName() == QLatin1String("chip")) sib->setChecked(false);
            }
            b->setChecked(true);
            emit range_changed(symbol_, range_, interval_);
        });
        ranges->addWidget(b);
    }
    ranges->addStretch();
    root->addLayout(ranges);

    plot_ = new QWidget(this);
    plot_->setMinimumHeight(220);
    plot_->installEventFilter(this);
    root->addWidget(plot_, 1);

    status_ = new QLabel(this);
    status_->setStyleSheet(QStringLiteral("color:#9ba2ab;font-size:11px;"));
    root->addWidget(status_);

    // plot_ 由本类的 paintEvent 绘制（把 plot_ 的绘制转交给父类处理）
    plot_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
}

void ChartOverlay::show_symbol(const QString& symbol) {
    symbol_ = symbol;
    bars_.clear();
    error_.clear();
    title_->setText(QStringLiteral("%1 · 历史走势").arg(symbol));
    status_->setText(QStringLiteral("加载中…"));
    show();
    raise();
    update();
}

void ChartOverlay::set_bars(const QVector<Bar>& bars, const QString& label) {
    bars_ = bars;
    error_.clear();
    if (bars.isEmpty()) {
        status_->setText(QStringLiteral("没有取到数据"));
    } else {
        const double first = bars.first().close;
        const double last = bars.last().close;
        const double pct = first > 0 ? (last - first) / first * 100.0 : 0;
        status_->setText(QStringLiteral("%1 根 · 区间 %2  %3 → %4  (%5%6%)")
                             .arg(bars.size())
                             .arg(label)
                             .arg(first, 0, 'f', 2)
                             .arg(last, 0, 'f', 2)
                             .arg(pct >= 0 ? QStringLiteral("+") : QString())
                             .arg(pct, 0, 'f', 2));
    }
    update();
}

void ChartOverlay::set_error(const QString& message) {
    error_ = message;
    bars_.clear();
    status_->setText(message);
    update();
}

void ChartOverlay::hide_overlay() {
    hide();
    emit closed();
}

void ChartOverlay::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {
        hide_overlay();
        return;
    }
    QFrame::keyPressEvent(e);
}

void ChartOverlay::paintEvent(QPaintEvent* e) {
    QFrame::paintEvent(e);
    if (!plot_ || bars_.isEmpty()) return;

    QPainter p(this);
    const QRect plot_rect = plot_->geometry();
    paint_chart(p, plot_rect.adjusted(0, 0, 0, 0));
}

void ChartOverlay::paint_chart(QPainter& p, const QRect& r) {
    p.fillRect(r, QColor("#101114"));
    p.setPen(theme::border());
    p.drawRect(r.adjusted(0, 0, -1, -1));

    double lo = bars_.first().low;
    double hi = bars_.first().high;
    for (const auto& b : bars_) {
        lo = qMin(lo, b.low);
        hi = qMax(hi, b.high);
    }
    const double span = (hi - lo) > 1e-9 ? (hi - lo) : 1.0;

    // 网格 + 右侧价格刻度
    QFont small = theme::num_font(theme::fs::small);
    p.setFont(small);
    for (int i = 0; i <= 4; ++i) {
        const int y = r.top() + 8 + (r.height() - 16) * i / 4;
        p.setPen(QColor("#1c1c1c"));
        p.drawLine(r.left() + 6, y, r.right() - 46, y);
        p.setPen(theme::text_faint());
        const double v = hi - span * i / 4.0;
        p.drawText(QRect(r.right() - 44, y - 8, 42, 16), Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(v, 'f', v >= 100 ? 0 : 2));
    }

    // K 线
    const int w = r.width() - 56;
    const int n = bars_.size();
    const double cw = qMax(1.0, double(w) / qMax(1, n));
    for (int i = 0; i < n; ++i) {
        const Bar& b = bars_.at(i);
        const double x = r.left() + 6 + cw * i + cw * 0.5;
        auto y_of = [&](double v) {
            return r.bottom() - 8 - (r.height() - 16) * ((v - lo) / span);
        };
        const QColor c = b.close >= b.open ? theme::up() : theme::down();

        p.setPen(c);
        p.drawLine(QPointF(x, y_of(b.high)), QPointF(x, y_of(b.low)));
        const double body_w = qMax(1.0, cw * 0.6);
        QRectF body(x - body_w / 2, y_of(qMax(b.open, b.close)), body_w,
                    qMax(1.0, qAbs(y_of(b.open) - y_of(b.close))));
        p.fillRect(body, c);
    }
}

} // namespace nb
