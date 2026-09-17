#include "ui/NewsListPanel.h"

#include "core/FeedCatalog.h"
#include "ui/Theme.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QTextDocument>
#include <QVBoxLayout>

namespace nb {

namespace {
// 行高按“标题两行 + 元信息一行 + 上下留白”定：68px 在 1600 宽下大约一屏 12 条，
// 既不像 60px 那样挤，也不会让阅读节奏散掉。
constexpr int kRowHeight = 68;
constexpr int kPadX = 14;        // 左右留白（与左栏一致，视觉上同一条栅格）
constexpr int kPadY = 10;

QString html_escape(const QString& s) {
    QString o = s;
    o.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    o.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    o.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    return o;
}

/// 把命中关键词的部分包成高亮 span（大小写不敏感）
QString highlight(const QString& text, const QStringList& keywords) {
    QString html = html_escape(text);
    for (const auto& kw : keywords) {
        const QString k = kw.trimmed();
        if (k.isEmpty()) continue;
        const QString esc = html_escape(k);
        int pos = 0;
        while (true) {
            const int idx = html.indexOf(esc, pos, Qt::CaseInsensitive);
            if (idx < 0) break;
            // 避免在已经插入的 <span> 标签内部再次匹配
            const int tag = html.lastIndexOf(QLatin1Char('<'), idx);
            const int tag_end = html.lastIndexOf(QLatin1Char('>'), idx);
            if (tag > tag_end) {
                pos = idx + esc.size();
                continue;
            }
            const QString wrapped =
                QStringLiteral("<span style='background-color:#3a2a10;color:#f0b060;font-weight:600;'>%1</span>")
                    .arg(esc);
            html.replace(idx, esc.size(), wrapped);
            pos = idx + wrapped.size();
        }
    }
    return html;
}

/// 分类徽章做成药丸：底色淡、字色琥珀，比 "[分类]" 这种括号文字好看也更好扫读
QString category_chip_html(const QString& cat) {
    if (cat.isEmpty()) return {};
    const QString label = catalog::category_label(cat);
    return QStringLiteral(
               "<span style='background-color:#2b2110;color:#e08b12;font-size:10px;"
               "padding:1px 6px;border-radius:8px;'>%1</span>&nbsp; ")
        .arg(html_escape(label));
}
} // namespace

// ── Delegate ────────────────────────────────────────────────────────────────

NewsRowDelegate::NewsRowDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

void NewsRowDelegate::set_lookup(const QHash<QString, NewsItem>* lookup, const QStringList* keywords) {
    lookup_ = lookup;
    keywords_ = keywords;
}

void NewsRowDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const {
    if (!lookup_) {
        QStyledItemDelegate::paint(p, opt, idx);
        return;
    }
    const QString id = idx.data(Qt::UserRole).toString();
    const NewsItem item = lookup_->value(id);
    if (item.id.isEmpty()) {
        QStyledItemDelegate::paint(p, opt, idx);
        return;
    }

    p->save();
    const QRect r = opt.rect;

    const bool selected = opt.state & QStyle::State_Selected;
    const bool hovered = opt.state & QStyle::State_MouseOver;
    const bool unread = !item.read;

    // 背景：已读用面板底、未读略亮一点点（不整行变色，靠左侧竖条区分）
    if (selected)
        p->fillRect(r, theme::selected());
    else if (hovered)
        p->fillRect(r, theme::hover());
    else
        p->fillRect(r, unread ? QColor("#12141a") : theme::panel());

    // 未读左侧竖条（Feedly/邮件客户端式）：3px，选中时同时是最左强调条
    if (unread || selected)
        p->fillRect(QRect(r.left(), r.top() + 8, 3, r.height() - 16),
                    selected ? theme::accent() : theme::unread());

    // 底部细分隔线
    p->setPen(QPen(theme::border_dim()));
    p->drawLine(r.left() + kPadX, r.bottom(), r.right() - kPadX, r.bottom());

    const QStringList* kws = keywords_;
    static const QStringList empty_kw;
    if (!kws) kws = &empty_kw;

    // ── 标题（原文）+ 高亮，最多两行 ──────────────────────────────────────
    const QString title_html =
        QStringLiteral("<div style='color:%1;font-size:%2px;line-height:18px;%3'>%4</div>")
            .arg(unread ? QStringLiteral("#e9eaec") : QStringLiteral("#a9afb7"))
            .arg(theme::fs::title)
            .arg(unread ? QStringLiteral("font-weight:600;") : QString())
            .arg(category_chip_html(item.category) + highlight(item.title, *kws));

    QTextDocument doc;
    doc.setDefaultFont(theme::ui_font(theme::fs::title));
    doc.setHtml(title_html);
    const int text_w = r.width() - kPadX * 2;
    doc.setTextWidth(text_w);

    p->translate(r.left() + kPadX, r.top() + kPadY);
    p->setClipRect(QRect(0, 0, text_w, 38));      // 两行（18px 行高）
    doc.drawContents(p);
    p->setClipping(false);
    p->translate(-(r.left() + kPadX), -(r.top() + kPadY));

    // ── 元信息行：左边「来源 · 关联标的」，右边「相对时间」（右对齐更易扫读）──
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 d = qMax<qint64>(0, now - item.ts);
    QString rel;
    if (d < 60)
        rel = QStringLiteral("刚刚");
    else if (d < 3600)
        rel = QStringLiteral("%1 分钟前").arg(d / 60);
    else if (d < 86400)
        rel = QStringLiteral("%1 小时前").arg(d / 3600);
    else
        rel = QStringLiteral("%1 天前").arg(d / 86400);

    QFont meta_font = theme::num_font(theme::fs::small);
    p->setFont(meta_font);
    const QFontMetrics meta_fm(meta_font);

    const int meta_y = r.bottom() - kPadY - meta_fm.height();
    const int rel_w = meta_fm.horizontalAdvance(rel);
    p->setPen(theme::text_faint());
    p->drawText(QRect(r.right() - kPadX - rel_w - 4, meta_y, rel_w + 4, meta_fm.height()),
                Qt::AlignRight | Qt::AlignVCenter, rel);

    QString meta = item.source;
    if (!item.tickers.isEmpty())
        meta += QStringLiteral("  ·  ") + item.tickers.mid(0, 4).join(QStringLiteral("/"));
    const int meta_w = qMax(20, r.width() - kPadX * 2 - rel_w - 14);
    p->setPen(theme::text_dim());
    p->drawText(QRect(r.left() + kPadX, meta_y, meta_w, meta_fm.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                meta_fm.elidedText(meta, Qt::ElideRight, meta_w));

    p->restore();
}

QSize NewsRowDelegate::sizeHint(const QStyleOptionViewItem& opt, const QModelIndex&) const {
    Q_UNUSED(opt);
    return QSize(100, kRowHeight);
}

// ── Panel ───────────────────────────────────────────────────────────────────

NewsListPanel::NewsListPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // 面板头：标题在左，操作在右；下面一条 1px 分隔线，把“工具条”和内容分开
    auto* head = new QWidget(this);
    head->setStyleSheet(QStringLiteral("background:#0f1013;"));
    auto* hl = new QHBoxLayout(head);
    hl->setContentsMargins(14, 9, 10, 9);
    hl->setSpacing(8);
    auto* title = new QLabel(QStringLiteral("资讯"), head);
    title->setObjectName("panelTitle");
    title->setStyleSheet(QStringLiteral("padding:0;color:#9ba2ab;font-size:12px;font-weight:600;"));
    hl->addWidget(title);
    head_count_ = new QLabel(head);
    head_count_->setStyleSheet(QStringLiteral("color:#6c7278;font-size:11px;"));
    hl->addWidget(head_count_);
    hl->addStretch();
    auto* mark = new QPushButton(QStringLiteral("全部已读"), head);
    mark->setObjectName("ghost");
    mark->setCursor(Qt::PointingHandCursor);
    connect(mark, &QPushButton::clicked, this, &NewsListPanel::mark_all_read_requested);
    hl->addWidget(mark);
    root->addWidget(head);
    auto* sep = new QFrame(this);
    sep->setObjectName("separator");
    sep->setFixedHeight(1);
    root->addWidget(sep);

    list_ = new QListWidget(this);
    list_->setUniformItemSizes(true);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setMouseTracking(true);

    auto* delegate = new NewsRowDelegate(list_);
    delegate->set_lookup(&by_id_, &keywords_);
    list_->setItemDelegate(delegate);

    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
        const QString id = it->data(Qt::UserRole).toString();
        if (by_id_.contains(id)) emit item_activated(by_id_.value(id));
    });

    root->addWidget(list_, 1);
}

void NewsListPanel::set_keywords(const QStringList& keywords) {
    keywords_ = keywords;
    list_->viewport()->update();
}

void NewsListPanel::set_items(const QVector<NewsItem>& items) {
    // 记住滚动位置，避免刷新时跳动
    const int scroll = list_->verticalScrollBar()->value();
    items_ = items;
    by_id_.clear();
    for (const auto& it : items_) by_id_.insert(it.id, it);
    rebuild_rows();
    update_head_count();
    list_->verticalScrollBar()->setValue(scroll);
}

void NewsListPanel::rebuild_rows() {
    list_->clear();
    for (const auto& it : items_) {
        auto* row = new QListWidgetItem(list_);
        row->setData(Qt::UserRole, it.id);
        row->setSizeHint(QSize(100, kRowHeight));
        row->setToolTip(it.title);
    }
}

void NewsListPanel::update_head_count() {
    if (!head_count_) return;
    const int n = unread_count();
    head_count_->setText(n > 0 ? QStringLiteral("· %1 条未读").arg(n) : QStringLiteral("· 已全部读完"));
}

int NewsListPanel::unread_count() const {
    int n = 0;
    for (const auto& it : items_) {
        if (!it.read) ++n;
    }
    return n;
}

} // namespace nb
