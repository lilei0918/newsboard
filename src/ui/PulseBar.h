#pragma once
// NewsBoard — 中栏顶部「脉冲条」：异动榜 + 多源共振热点
//
// 两条信息的来源完全不同，放在一起是因为它们回答的是同一个问题：「现在最该看什么」。
//   · 异动：来自我们自己的 5 秒行情（最近 5 分钟涨速），是「市场正在动」
//   · 热点：来自跨源标题聚类（同一件事被几家在报），是「消息在扩散」
// 两者都不是「编辑推荐」，所以文案如实写「5 分钟涨速」「N 源 · M 分钟」，不写「重磅」。

#include "core/Models.h"

#include <QVector>
#include <QWidget>

class QHBoxLayout;
class QLabel;
class QPushButton;

namespace nb {

class PulseBar : public QWidget {
    Q_OBJECT
  public:
    explicit PulseBar(QWidget* parent = nullptr);

    void set_movers(const QVector<Mover>& movers);
    void set_clusters(const QVector<HotCluster>& clusters);

  signals:
    void symbol_activated(const QString& symbol);   // 点异动标的 → K 线
    void item_activated(const QString& item_id);    // 点热点 → 新闻详情

  private:
    void rebuild();
    static QString rel_time(qint64 ts);

    QVector<Mover> movers_;
    QVector<HotCluster> clusters_;

    QPushButton* toggle_ = nullptr;
    QWidget* movers_row_ = nullptr;
    QWidget* hot_row_ = nullptr;
    QHBoxLayout* movers_lay_ = nullptr;
    QHBoxLayout* hot_lay_ = nullptr;
    bool collapsed_ = false;
};

} // namespace nb
