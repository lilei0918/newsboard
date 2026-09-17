#pragma once
// NewsBoard — 浮层：新闻详情 与 行情图表

#include "core/Models.h"

#include <QFrame>
#include <QVector>

class QLabel;
class QPushButton;
class QTextBrowser;

namespace nb {

/// 新闻详情浮层（覆盖在中栏上方，Esc 关闭）
class NewsDetailOverlay : public QFrame {
    Q_OBJECT
  public:
    explicit NewsDetailOverlay(QWidget* parent = nullptr);

    void show_item(const NewsItem& item);
    void hide_overlay();

  signals:
    void closed();

  protected:
    void keyPressEvent(QKeyEvent* e) override;

  private:
    QLabel* title_ = nullptr;
    QLabel* meta_ = nullptr;
    QTextBrowser* body_ = nullptr;
    QPushButton* open_btn_ = nullptr;
    QPushButton* copy_btn_ = nullptr;
    NewsItem current_;
};

/// 行情图表浮层（右栏上方，画历史 K 线 + 区间切换）
class ChartOverlay : public QFrame {
    Q_OBJECT
  public:
    explicit ChartOverlay(QWidget* parent = nullptr);

    void show_symbol(const QString& symbol);
    void set_bars(const QVector<Bar>& bars, const QString& label);
    void set_error(const QString& message);
    void hide_overlay();
    QString symbol() const { return symbol_; }
    QString range() const { return range_; }

  signals:
    void closed();
    void range_changed(const QString& symbol, const QString& range, const QString& interval);

  protected:
    void keyPressEvent(QKeyEvent* e) override;
    void paintEvent(QPaintEvent* e) override;

  private:
    void paint_chart(QPainter& p, const QRect& r);

    QLabel* title_ = nullptr;
    QLabel* status_ = nullptr;
    QWidget* plot_ = nullptr;
    QVector<Bar> bars_;
    QString symbol_;
    QString range_ = "3mo";
    QString interval_ = "1d";
    QString error_;
    int hover_ = -1;
};

} // namespace nb
