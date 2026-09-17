#include "ui/FilterPanel.h"

#include "core/Config.h"
#include "core/FeedCatalog.h"
#include "ui/Theme.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

namespace nb {

FilterPanel::FilterPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── 顶部：标题 + 刷新 ──
    auto* head = new QWidget(this);
    auto* hl = new QHBoxLayout(head);
    head->setStyleSheet(QStringLiteral("background:#0f1013;"));
    hl->setContentsMargins(14, 9, 10, 9);
    auto* title = new QLabel(QStringLiteral("筛选"), head);
    title->setObjectName("panelTitle");
    title->setStyleSheet(QStringLiteral("padding:0;color:#9ba2ab;font-size:12px;font-weight:600;"));
    hl->addWidget(title);
    head_hint_ = new QLabel(QStringLiteral("· 分类 / 源 / 频率"), head);
    head_hint_->setStyleSheet(QStringLiteral("color:#6c7278;font-size:11px;"));
    hl->addWidget(head_hint_);
    hl->addStretch();
    auto* refresh = new QPushButton(QStringLiteral("刷新"), head);
    refresh->setObjectName("ghost");
    refresh->setCursor(Qt::PointingHandCursor);
    connect(refresh, &QPushButton::clicked, this, &FilterPanel::refresh_requested);
    hl->addWidget(refresh);
    root->addWidget(head);
    {
        auto* sep = new QFrame(this);
        sep->setObjectName("separator");
        sep->setFixedHeight(1);
        root->addWidget(sep);
    }

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    // 全局 QSS 里 QScrollArea 是透明的，这里显式刷成面板色，否则会漏出更深的窗口底色，
    // 三栏看起来就不像同一个平面上的三块面板了
    scroll->setStyleSheet(QStringLiteral("QScrollArea, QScrollArea > QWidget > QWidget { background:#101114; }"));
    auto* body = new QWidget(scroll);
    body->setStyleSheet(QStringLiteral("background:#101114;"));
    auto* bl = new QVBoxLayout(body);
    bl->setContentsMargins(10, 0, 10, 10);
    bl->setSpacing(8);

    // ── 分类 ──
    {
        auto* t = new QLabel(QStringLiteral("分类"), body);
        t->setObjectName("sectionTitle");
        bl->addWidget(t);

        category_group_ = new QButtonGroup(this);
        category_group_->setExclusive(true);

        auto* wrap = new QWidget(body);
        auto* grid = new QVBoxLayout(wrap);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(5);

        auto add_chip_row = [&](const QStringList& keys, QHBoxLayout* row) {
            for (const auto& k : keys) {
                auto* b = new QPushButton(catalog::category_label(k), wrap);
                b->setObjectName("chip");
                b->setCheckable(true);
                b->setCursor(Qt::PointingHandCursor);
                b->setProperty("cat", k);
                category_group_->addButton(b);
                row->addWidget(b);
            }
            row->addStretch();
        };

        auto* all_btn = new QPushButton(QStringLiteral("全部"), wrap);
        all_btn->setObjectName("chip");
        all_btn->setCheckable(true);
        all_btn->setChecked(true);
        all_btn->setCursor(Qt::PointingHandCursor);
        all_btn->setProperty("cat", QString());
        category_group_->addButton(all_btn);
        auto* r0 = new QHBoxLayout();
        r0->setSpacing(5);
        r0->addWidget(all_btn);
        r0->addStretch();
        grid->addLayout(r0);

        const QStringList keys = catalog::category_keys();
        for (int i = 0; i < keys.size(); i += 3) {
            auto* row = new QHBoxLayout();
            row->setSpacing(5);
            add_chip_row(keys.mid(i, 3), row);
            grid->addLayout(row);
        }

        connect(category_group_, &QButtonGroup::buttonClicked, this, [this](QAbstractButton* b) {
            current_category_ = b->property("cat").toString();
            emit category_changed(current_category_);
        });

        bl->addWidget(wrap);
    }

    // ── 搜索 ──
    {
        auto* t = new QLabel(QStringLiteral("搜索标题 / 摘要"), body);
        t->setObjectName("sectionTitle");
        bl->addWidget(t);
        search_ = new QLineEdit(body);
        search_->setPlaceholderText(QStringLiteral("输入关键词…"));
        connect(search_, &QLineEdit::textChanged, this, &FilterPanel::keyword_changed);
        bl->addWidget(search_);
    }

    // ── 高亮词 ──
    {
        auto* t = new QLabel(QStringLiteral("高亮词（逗号分隔）"), body);
        t->setObjectName("sectionTitle");
        bl->addWidget(t);
        highlight_ = new QLineEdit(body);
        highlight_->setPlaceholderText(QStringLiteral("例如：英伟达, 降息, 关税"));
        highlight_->setText(Config::instance().highlight_keywords().join(QStringLiteral(", ")));
        connect(highlight_, &QLineEdit::editingFinished, this, [this]() {
            QStringList kw;
            for (const auto& s : highlight_->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                const QString t = s.trimmed();
                if (!t.isEmpty()) kw << t;
            }
            Config::instance().set_highlight_keywords(kw);
            Config::instance().save();
            emit highlight_keywords_changed(kw);
        });
        bl->addWidget(highlight_);
    }

    // ── 源列表 ──
    {
        auto* t = new QLabel(QStringLiteral("资讯源"), body);
        t->setObjectName("sectionTitle");
        bl->addWidget(t);

        health_label_ = new QLabel(QStringLiteral("—"), body);
        health_label_->setStyleSheet(QStringLiteral("color:#5a5a5a;font-size:11px;"));
        bl->addWidget(health_label_);

        source_list_ = new QListWidget(body);
        source_list_->setMaximumHeight(260);
        source_list_->setSelectionMode(QAbstractItemView::NoSelection);
        connect(source_list_, &QListWidget::itemChanged, this, [this](QListWidgetItem* it) {
            const QString id = it->data(Qt::UserRole).toString();
            const bool on = it->checkState() == Qt::Checked;
            Config::instance().set_feed_enabled(id, on);
            Config::instance().save();
            emit feed_toggled(id, on);
            emit_source_filter();
        });
        bl->addWidget(source_list_);

    }

    // ── 刷新频率 ──
    {
        auto* t = new QLabel(QStringLiteral("刷新频率"), body);
        t->setObjectName("sectionTitle");
        bl->addWidget(t);

        auto* row1 = new QHBoxLayout();
        row1->addWidget(new QLabel(QStringLiteral("资讯"), body));
        news_interval_ = new QComboBox(body);
        news_interval_->addItems({QStringLiteral("1 分钟"), QStringLiteral("5 分钟"), QStringLiteral("10 分钟"),
                                  QStringLiteral("30 分钟")});
        news_interval_->setCurrentIndex(
            Config::instance().news_refresh_minutes() == 1    ? 0
            : Config::instance().news_refresh_minutes() <= 5  ? 1
            : Config::instance().news_refresh_minutes() <= 10 ? 2
                                                              : 3);
        connect(news_interval_, &QComboBox::currentIndexChanged, this, [this](int i) {
            static const int mins[] = {1, 5, 10, 30};
            emit news_interval_changed(mins[qBound(0, i, 3)]);
        });
        row1->addWidget(news_interval_, 1);
        bl->addLayout(row1);

        auto* row2 = new QHBoxLayout();
        row2->addWidget(new QLabel(QStringLiteral("行情"), body));
        quote_interval_ = new QComboBox(body);
        quote_interval_->setToolTip(QStringLiteral(
            "行情每隔多久拉一次。默认 5 秒：用 Yahoo 批量报价接口，一轮只要 1 个请求、约 0.3 秒，"
            "所以可以做到准实时；3 秒是上限（再快也只是重复拿同一个 tick）。"));
        quote_interval_->addItems({QStringLiteral("3 秒（实时）"), QStringLiteral("5 秒（默认·实时）"),
                                   QStringLiteral("10 秒"), QStringLiteral("30 秒"),
                                   QStringLiteral("60 秒")});
        const int qs = Config::instance().quote_refresh_seconds();
        const int qidx = qs <= 3 ? 0 : qs <= 5 ? 1 : qs <= 10 ? 2 : qs <= 30 ? 3 : 4;
        quote_interval_->setCurrentIndex(qidx);
        connect(quote_interval_, &QComboBox::currentIndexChanged, this, [this](int i) {
            static const int secs[] = {3, 5, 10, 30, 60};
            emit quote_interval_changed(secs[qBound(0, i, 4)]);
        });
        row2->addWidget(quote_interval_, 1);
        bl->addLayout(row2);
    }

    // ── Python 依赖 ──
    {
        // 正常情况下行情依赖已随目录自带，这个按钮只在运行环境缺失时才显示
        install_btn_ = new QPushButton(QStringLiteral("修复行情运行环境"), body);
        install_btn_->setCursor(Qt::PointingHandCursor);
        install_btn_->setVisible(false);
        connect(install_btn_, &QPushButton::clicked, this, &FilterPanel::install_python_requested);
        bl->addWidget(install_btn_);
    }

    bl->addStretch();
    scroll->setWidget(body);
    root->addWidget(scroll, 1);

    reload_sources();
}

void FilterPanel::set_repair_visible(bool visible) {
    if (install_btn_) install_btn_->setVisible(visible);
}

void FilterPanel::reload_sources() {
    if (!source_list_) return;
    source_list_->blockSignals(true);
    source_list_->clear();
    for (const auto& f : Config::instance().feeds()) {
        auto* it = new QListWidgetItem(QStringLiteral("%1  ·  %2").arg(f.name, catalog::category_label(f.category)),
                                       source_list_);
        it->setData(Qt::UserRole, f.id);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(f.enabled ? Qt::Checked : Qt::Unchecked);
        it->setForeground(f.enabled ? theme::text() : theme::text_faint());
    }
    source_list_->blockSignals(false);
}

void FilterPanel::emit_source_filter() {
    QSet<QString> ids;
    for (const auto& f : Config::instance().feeds()) {
        if (f.enabled) ids.insert(f.id);
    }
    emit source_filter_changed(ids);
}

void FilterPanel::update_health(const QVector<FeedHealth>& health, int ok, int total) {
    if (!health_label_) return;
    health_label_->setText(QStringLiteral("源健康度：%1/%2 正常").arg(ok).arg(total));

    QStringList failing;
    for (const auto& h : health) {
        if (!h.ok && !h.lastError.isEmpty() && h.consecutiveFailures > 0) {
            failing << QStringLiteral("%1（%2）").arg(h.feedId).arg(h.lastError.left(40));
        }
    }
    health_label_->setToolTip(failing.isEmpty() ? QStringLiteral("全部源正常")
                                                : QStringLiteral("异常源：\n") + failing.join(QStringLiteral("\n")));
}

} // namespace nb
