#pragma once
// NewsBoard — 中栏：新闻列表（含富文本行渲染与关键词高亮）

#include "core/Models.h"

#include <QButtonGroup>
#include <QHash>
#include <QLabel>
#include <QListWidget>
#include <QStyledItemDelegate>
#include <QVector>

namespace nb {

class NewsListPanel : public QWidget {
    Q_OBJECT
  public:
    explicit NewsListPanel(QWidget* parent = nullptr);

    void set_items(const QVector<NewsItem>& items);
    void set_keywords(const QStringList& keywords);
    int unread_count() const;
    int item_count() const { return items_.size(); }
    /// 当前列表里正在显示的条目（供自动化截图核对用）
    QVector<NewsItem> items_shown() const { return items_; }
    /// 按 id 找条目（热点条点击后要打开对应的新闻）
    bool item_by_id(const QString& id, NewsItem* out) const {
        if (!by_id_.contains(id)) return false;
        if (out) *out = by_id_.value(id);
        return true;
    }

  signals:
    void item_activated(const NewsItem& item);
    void mark_all_read_requested();
    void density_changed(int density);   // 0 紧凑 / 1 标准 / 2 舒适

  private:
    void rebuild_rows();
    void update_head_count();
    QString relative_time(qint64 ts) const;

    QListWidget* list_ = nullptr;
    QLabel* head_count_ = nullptr;
    QButtonGroup* density_group_ = nullptr;
    QVector<NewsItem> items_;
    QHash<QString, NewsItem> by_id_;
    QStringList keywords_;
};

/// 单行新闻的富文本渲染：分类徽章 + 标题（原文，命中关键词高亮）+ 来源·时间
class NewsRowDelegate : public QStyledItemDelegate {
    Q_OBJECT
  public:
    explicit NewsRowDelegate(QObject* parent = nullptr);
    void set_lookup(const QHash<QString, NewsItem>* lookup, const QStringList* keywords);
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;
    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;

  private:
    const QHash<QString, NewsItem>* lookup_ = nullptr;
    const QStringList* keywords_ = nullptr;
};

} // namespace nb
