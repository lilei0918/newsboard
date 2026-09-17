#include "ui/NewsListPanel.h"

#include "core/Config.h"
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
// 三档新闻密度：盯盘用紧凑（一屏看更多），读新闻用舒适（多一行摘要）。
//   紧凑 54px = 标题两行 + 元信息
//   标准 68px = 上面基础上多留白（默认）
//   舒适 84px = 再多一行摘要
int row_height_for(int density) {
    switch (density) {
        case 0: return 54;
        case 2: return 84;
        default: return 68;
    }
}
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

/// 分类徽章做成药丸。**中性灰**：琥珀要留给「操作 / 焦点 / 重要」，
/// 如果每个分类都是琥珀，琥珀就不再代表任何东西了。
QString category_chip_html(const QString& cat) {
    if (cat.isEmpty()) return {};
    const QString label = catalog::category_label(cat);
    return QStringLiteral(
               "<span style='background-color:#1c1f24;color:#9ba2ab;font-size:10px;"
               "padding:1px 6px;border-radius:8px;'>%1</span>&nbsp; ")
        .arg(html_escape(label));
}

/// 突发专用药丸（红色）—— 只在真正突发时出现，所以从不泛滥
QString breaking_chip_html() {
    return QStringLiteral(
        "<span style='background-color:#4a1512;color:#ff8b80;font-size:10px;font-weight:600;"
        "padding:1px 6px;border-radius:8px;'>突发</span>&nbsp; ");
}

/// 三档重要度的配色：突发=红、重要=琥珀、普通=不强调（灰）
struct TierStyle {
    QColor bar;
    QString title_color;
    int title_weight;
};
TierStyle tier_style(NewsTier t, bool unread) {
    switch (t) {
        case NewsTier::Breaking:
            return {QColor("#f04438"), unread ? QStringLiteral("#f7f8f9") : QStringLiteral("#b9bec4"),
                    unread ? 600 : 500};
        case NewsTier::Important:
            return {QColor("#e08b12"), unread ? QStringLiteral("#eef0f2") : QStringLiteral("#aeb4bb"),
                    unread ? 600 : 400};
        default:
            return {QColor("#4a4f57"), unread ? QStringLiteral("#e9eaec") : QStringLiteral("#a9afb7"),
                    unread ? 500 : 400};
    }
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

    // 左侧竖条：颜色 = 重要度（突发红 / 重要琥珀 / 普通灰），未读才实心；
    // 已读的重要条目留一条淡淡的痕迹，方便回头找。选中时统一用琥珀（琥珀=焦点）。
    const TierStyle ts = tier_style(item.tier, unread);
    if (selected) {
        p->fillRect(QRect(r.left(), r.top() + 8, 3, r.height() - 16), theme::accent());
    } else if (unread) {
        p->fillRect(QRect(r.left(), r.top() + 8, 3, r.height() - 16), ts.bar);
    } else if (item.tier != NewsTier::Normal) {
        QColor faint = ts.bar;
        faint.setAlpha(90);
        p->fillRect(QRect(r.left(), r.top() + 8, 2, r.height() - 16), faint);
    }

    // 底部细分隔线
    p->setPen(QPen(theme::border_dim()));
    p->drawLine(r.left() + kPadX, r.bottom(), r.right() - kPadX, r.bottom());

    const QStringList* kws = keywords_;
    static const QStringList empty_kw;
    if (!kws) kws = &empty_kw;

    // ── 标题（原文）+ 高亮，最多两行 ──────────────────────────────────────
    const QString tier_chip = item.tier == NewsTier::Breaking ? breaking_chip_html() : QString();
    const QString title_html =
        QStringLiteral("<div style='color:%1;font-size:%2px;line-height:18px;font-weight:%3'>%4</div>")
            .arg(ts.title_color)
            .arg(theme::fs::title)
            .arg(ts.title_weight)
            .arg(tier_chip + category_chip_html(item.category) + highlight(item.title, *kws));

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

    // 舒适档多一行摘要（标题两行 + 摘要一行 + 元信息一行）
    const int density = Config::instance().news_density();
    if (density == 2 && !item.summary.isEmpty()) {
        QFont sum_font = theme::ui_font(theme::fs::small);
        p->setFont(sum_font);
        p->setPen(theme::text_faint());
        const QFontMetrics sfm(sum_font);
        const int sum_w = r.width() - kPadX * 2;
        const int sum_y = r.top() + kPadY + 40;
        p->drawText(QRect(r.left() + kPadX, sum_y, sum_w, sfm.height()),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    sfm.elidedText(item.summary.simplified(), Qt::ElideRight, sum_w));
    }

    QString meta = item.source;
    if (!item.origin.isEmpty()) meta += QStringLiteral("  ·  ") + item.origin;
    if (!item.tickers.isEmpty())
        meta += QStringLiteral("  ·  ") + item.tickers.mid(0, 4).join(QStringLiteral("/"));
    else if (!item.stocks.isEmpty())
        meta += QStringLiteral("  ·  ") + item.stocks.mid(0, 3).join(QStringLiteral("/"));
    const int meta_w = qMax(20, r.width() - kPadX * 2 - rel_w - 14);
    p->setPen(theme::text_dim());
    p->drawText(QRect(r.left() + kPadX, meta_y, meta_w, meta_fm.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                meta_fm.elidedText(meta, Qt::ElideRight, meta_w));

    p->restore();
}

QSize NewsRowDelegate::sizeHint(const QStyleOptionViewItem& opt, const QModelIndex&) const {
    Q_UNUSED(opt);
    return QSize(100, row_height_for(Config::instance().news_density()));
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

    // 密度三档：盯着盘的时候要“一屏看更多”，读新闻的时候要“多一行摘要”
    {
        auto* lb = new QLabel(QStringLiteral("密度"), head);
        lb->setStyleSheet(QStringLiteral("color:#6c7278;font-size:10px;padding-left:6px;"));
        hl->addWidget(lb);
        density_group_ = new QButtonGroup(this);
        density_group_->setExclusive(true);
        const QStringList names{QStringLiteral("紧凑"), QStringLiteral("标准"), QStringLiteral("舒适")};
        for (int i = 0; i < names.size(); ++i) {
            auto* b = new QPushButton(names.at(i), head);
            b->setObjectName("mini");
            b->setCheckable(true);
            b->setCursor(Qt::PointingHandCursor);
            b->setFocusPolicy(Qt::NoFocus);
            b->setChecked(Config::instance().news_density() == i);
            density_group_->addButton(b, i);
            hl->addWidget(b);
        }
        connect(density_group_, &QButtonGroup::idClicked, this, [this](int id) {
            Config::instance().set_news_density(id);
            Config::instance().save();
            rebuild_rows();
            list_->doItemsLayout();
            emit density_changed(id);
        });
    }
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
        row->setSizeHint(QSize(100, row_height_for(Config::instance().news_density())));
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
