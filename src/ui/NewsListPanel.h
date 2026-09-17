#pragma once
// NewsBoard — 中栏：新闻列表（含富文本行渲染与关键词高亮）

#include "core/Models.h"

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

  signals:
    void item_activated(const NewsItem& item);
    void mark_all_read_requested();

  private:
    void rebuild_rows();
    void update_head_count();
    QString relative_time(qint64 ts) const;

    QListWidget* list_ = nullptr;
    QLabel* head_count_ = nullptr;
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
