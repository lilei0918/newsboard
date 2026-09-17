#pragma once
// NewsBoard — 右栏：行情看板（分组 + 迷你走势，自绘）

#include "core/Models.h"

#include <QPushButton>
#include <QScrollArea>
#include <QVector>
#include <QWidget>
#include <QWidget>

namespace nb {

/// 自绘看板：每个分组一个标题条，下面是若干行（名称/代码 · 迷你走势 · 价格 · 涨跌幅）
class QuoteBoardPanel : public QWidget {
    Q_OBJECT
  public:
    explicit QuoteBoardPanel(QWidget* parent = nullptr);

    void set_quotes(const QVector<Quote>& quotes);
    void set_loading(bool loading);
    /// 行高或排序方式改了之后重建（紧凑/宽松、按涨跌幅/按原顺序）
    void refresh_layout();
    QString selected_symbol() const { return selected_; }

  signals:
    void symbol_activated(const QString& symbol);
    void add_symbol_requested(const QString& group_id);
    void remove_symbol_requested(const QString& group_id, const QString& symbol);

  protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

  private:
    struct Row {
        bool header = false;
        QString group_id;
        QString group_title;
        int group_count = 0;     // 分组里的标的数（画在分组头右侧）
        Quote quote;
        QRect rect;
    };

    void rebuild_rows();
    int row_height() const;
    int row_at(const QPoint& p) const;
    int content_height() const;

    QVector<Row> rows_;
    QVector<Quote> quotes_;
    QString selected_;
    int hover_ = -1;
    bool loading_ = false;
};

/// 带滚动条的行情栏容器
class QuoteBoardView : public QWidget {
    Q_OBJECT
  public:
    explicit QuoteBoardView(QWidget* parent = nullptr);
    QuoteBoardPanel* board() const { return board_; }

  private:
    QScrollArea* scroll_ = nullptr;
    QPushButton* sort_btn_ = nullptr;
    QuoteBoardPanel* board_ = nullptr;
};

} // namespace nb
