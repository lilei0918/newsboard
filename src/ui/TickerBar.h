#pragma once
// NewsBoard — 顶部滚动行情条

#include "core/Models.h"

#include <QVector>
#include <QWidget>

class QTimer;

namespace nb {

class TickerBar : public QWidget {
    Q_OBJECT
  public:
    explicit TickerBar(QWidget* parent = nullptr);

    void set_quotes(const QVector<Quote>& quotes);
    void set_paused(bool paused);
    bool paused() const { return paused_; }

  signals:
    void symbol_activated(const QString& symbol);
    void pause_toggled(bool paused);

  protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;

  private:
    struct Cell {
        QString symbol;    // 代码（弱色）
        QString price;     // 现价（亮色，等宽数字）
        QString pct;       // 涨跌幅（涨跌色，等宽数字）
        QString label;     // 显示名（中文别名优先）
        QColor color;
        QRect rect;
    };
    void rebuild_cells();

    QVector<Quote> quotes_;
    QVector<Cell> cells_;
    QTimer* timer_ = nullptr;
    int offset_ = 0;
    int hover_ = -1;
    bool paused_ = false;
    static constexpr int kHeight = 34;   // 与右栏顶栏同高，视觉上一条水平带
};

} // namespace nb
