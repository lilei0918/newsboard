#pragma once
// NewsBoard — 主窗口（单页面：行情条 + 三栏 + 状态栏 + 浮层）

#include "core/Models.h"

#include <QMainWindow>

class QLabel;
class QPushButton;
class QSplitter;
class QTimer;

namespace nb {

class NewsAggregator;
class QuoteService;
class PyEnv;
class TickerBar;
class FilterPanel;
class NewsListPanel;
class QuoteBoardView;
class PulseBar;
class NewsDetailOverlay;
class ChartOverlay;

class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    /// 仅用于自动化截图核对（NB_SHOT_OVERLAY）：打开中栏第一条资讯 / 指定标的的 K 线浮层
    void debug_open_first_news();
    void debug_open_chart(const QString& symbol);
    int debug_item_count() const;

  protected:
    void resizeEvent(QResizeEvent* e) override;
    void closeEvent(QCloseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

  private:
    void build_ui();
    void build_statusbar();
    void wire();
    void apply_filters();
    void schedule_news_timer();
    void update_status();
    void refresh_news();
    void install_python();
    void layout_overlays();
    void open_news(const NewsItem& item);
    void open_chart(const QString& symbol);

    /// 左栏（筛选）显示/隐藏；隐藏时把宽度让给中右两栏，再次显示时恢复原宽度
    void set_left_panel_visible(bool on);

    NewsAggregator* news_ = nullptr;
    QuoteService* quotes_ = nullptr;
    PyEnv* pyenv_ = nullptr;

    TickerBar* ticker_ = nullptr;
    PulseBar* pulse_ = nullptr;
    QSplitter* splitter_ = nullptr;
    FilterPanel* filter_ = nullptr;
    NewsListPanel* list_ = nullptr;
    QuoteBoardView* board_view_ = nullptr;
    NewsDetailOverlay* detail_ = nullptr;
    ChartOverlay* chart_ = nullptr;

    QPushButton* left_toggle_ = nullptr;   // 状态栏里的「隐藏/显示筛选栏」
    QList<int> splitter_saved_sizes_;      // 隐藏前记录的三栏宽度
    void refresh_hot_clusters();
    QLabel* status_news_ = nullptr;
    QLabel* status_quotes_ = nullptr;
    QLabel* status_error_ = nullptr;

    QTimer* news_timer_ = nullptr;
    QTimer* status_timer_ = nullptr;

    QString current_category_;
    QSet<QString> current_sources_;
    QString current_keyword_;
};

} // namespace nb
