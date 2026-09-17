#include "MainWindow.h"

#include "core/Config.h"
#include "core/NewsAggregator.h"
#include "core/PyEnv.h"
#include "core/QuoteService.h"
#include "ui/FilterPanel.h"
#include "ui/NewsListPanel.h"
#include "ui/Overlays.h"
#include "ui/PulseBar.h"
#include "ui/QuoteBoardPanel.h"
#include "ui/Theme.h"
#include "ui/TickerBar.h"

#include <QApplication>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

namespace nb {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("NewsBoard — 资讯 · 行情"));
    resize(1560, 940);

    Config::instance().load();

    news_ = new NewsAggregator(this);
    quotes_ = new QuoteService(this);
    pyenv_ = new PyEnv(this);

    build_ui();
    build_statusbar();
    wire();

    // 读缓存先出内容，再联网刷新
    news_->load_cache();
    apply_filters();
    quotes_->start();

    QTimer::singleShot(500, this, [this]() { refresh_news(); });

    status_timer_ = new QTimer(this);
    status_timer_->setInterval(1000);
    connect(status_timer_, &QTimer::timeout, this, &MainWindow::update_status);
    status_timer_->start();

    set_left_panel_visible(Config::instance().left_panel_visible());

    if (!Config::instance().window_geometry().isEmpty())
        restoreGeometry(Config::instance().window_geometry());
}

MainWindow::~MainWindow() = default;

void MainWindow::build_ui() {
    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    ticker_ = new TickerBar(central);
    root->addWidget(ticker_);

    // 中栏 = 顶部脉冲条（异动 + 热点）+ 资讯列表
    auto* middle = new QWidget(central);
    auto* ml = new QVBoxLayout(middle);
    ml->setContentsMargins(0, 0, 0, 0);
    ml->setSpacing(0);
    pulse_ = new PulseBar(middle);
    ml->addWidget(pulse_);
    list_ = new NewsListPanel(middle);
    ml->addWidget(list_, 1);

    splitter_ = new QSplitter(Qt::Horizontal, central);
    splitter_->setHandleWidth(1);
    filter_ = new FilterPanel(splitter_);
    board_view_ = new QuoteBoardView(splitter_);
    splitter_->addWidget(filter_);
    splitter_->addWidget(middle);
    splitter_->addWidget(board_view_);
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);
    splitter_->setStretchFactor(2, 0);
    // 布局默认值参考主流终端：左栏是「抽屉」（平时不占地方，需要时才看），
    // 中间资讯给最多空间（阅读主体），右栏行情够放「名称+代码 / 迷你走势 / 两行涨跌幅」即可
    // （去掉价格列和「盘后」文字后，330px 就够；再窄会先让迷你走势给名称）。
    splitter_->setSizes({220, 1000, 330});
    root->addWidget(splitter_, 1);

    setCentralWidget(central);

    detail_ = new NewsDetailOverlay(central);
    chart_ = new ChartOverlay(central);
}

void MainWindow::build_statusbar() {
    // 左栏显隐开关：放在状态栏（常驻可见），左栏藏起来之后仍然能点回来
    left_toggle_ = new QPushButton(this);
    left_toggle_->setObjectName("chip");
    left_toggle_->setCursor(Qt::PointingHandCursor);
    left_toggle_->setFocusPolicy(Qt::NoFocus);
    left_toggle_->setToolTip(QStringLiteral(
        "隐藏/显示左栏（筛选/资讯源/刷新频率）。\n"
        "隐藏后宽度自动让给资讯列表与行情栏，状态会记住，下次启动保持一样。"));
    connect(left_toggle_, &QPushButton::clicked, this, [this]() {
        set_left_panel_visible(!Config::instance().left_panel_visible());
    });

    status_news_ = new QLabel(this);
    status_quotes_ = new QLabel(this);
    status_error_ = new QLabel(this);
    status_error_->setStyleSheet(QStringLiteral("color:#eab308;"));

    statusBar()->addWidget(left_toggle_);
    statusBar()->addWidget(status_news_);
    statusBar()->addWidget(status_quotes_);
    statusBar()->addPermanentWidget(status_error_);
    update_status();
}

void MainWindow::wire() {
    // ── 左栏 → 过滤 ──
    connect(filter_, &FilterPanel::category_changed, this, [this](const QString& c) {
        current_category_ = c;
        apply_filters();
    });
    connect(filter_, &FilterPanel::source_filter_changed, this, [this](const QSet<QString>& s) {
        current_sources_ = s;
        apply_filters();
    });
    connect(filter_, &FilterPanel::keyword_changed, this, [this](const QString& k) {
        current_keyword_ = k;
        apply_filters();
    });
    connect(filter_, &FilterPanel::highlight_keywords_changed, this, [this](const QStringList& kw) {
        list_->set_keywords(kw);
    });
    connect(filter_, &FilterPanel::refresh_requested, this, [this]() {
        refresh_news();
        quotes_->refresh_now();
    });
    connect(filter_, &FilterPanel::install_python_requested, this, &MainWindow::install_python);
    connect(filter_, &FilterPanel::news_interval_changed, this, [this](int m) {
        Config::instance().set_news_refresh_minutes(m);
        Config::instance().save();
        schedule_news_timer();
    });
    connect(filter_, &FilterPanel::quote_interval_changed, this, [this](int s) {
        Config::instance().set_quote_refresh_seconds(s);
        Config::instance().save();
        quotes_->set_refresh_seconds(s);   // 只改定时器：守护进程不用重启
    });

    // ── 中栏 ──
    connect(list_, &NewsListPanel::item_activated, this, &MainWindow::open_news);
    connect(list_, &NewsListPanel::mark_all_read_requested, this, [this]() {
        news_->mark_all_read();
        apply_filters();
        news_->save_cache();
    });

    // ── 右栏 ──
    connect(board_view_->board(), &QuoteBoardPanel::symbol_activated, this, &MainWindow::open_chart);
    connect(board_view_->board(), &QuoteBoardPanel::remove_symbol_requested, this,
            [this](const QString& group, const QString& symbol) {
                const QuoteGroup g = [&]() {
                    for (const auto& x : Config::instance().quote_groups()) {
                        if (x.id == group) return x;
                    }
                    return QuoteGroup{};
                }();
                QStringList syms = g.symbols;
                syms.removeAll(symbol);
                Config::instance().set_group_symbols(group, syms);
                Config::instance().save();
                quotes_->refresh_now();
            });

    // ── 顶部行情条 ──
    connect(ticker_, &TickerBar::symbol_activated, this, &MainWindow::open_chart);
    connect(ticker_, &TickerBar::pause_toggled, this, [this](bool paused) {
        Config::instance().set_ticker_paused(paused);
        Config::instance().save();
    });
    ticker_->set_paused(Config::instance().ticker_paused());

    // ── 数据 ──
    connect(news_, &NewsAggregator::news_updated, this, [this](int, int, int) {
        apply_filters();
        refresh_hot_clusters();
    });
    connect(news_, &NewsAggregator::feed_health_changed, this, [this]() {
        filter_->update_health(news_->health(), news_->feed_ok_count(), news_->feed_count());
        update_status();
    });
    connect(detail_, &NewsDetailOverlay::symbol_activated, this, &MainWindow::open_chart);

    // ── 中栏脉冲条 ──
    connect(pulse_, &PulseBar::symbol_activated, this, &MainWindow::open_chart);
    connect(pulse_, &PulseBar::item_activated, this, [this](const QString& id) {
        NewsItem it;
        if (list_->item_by_id(id, &it)) open_news(it);
    });
    connect(quotes_, &QuoteService::movers_updated, this,
            [this]() { pulse_->set_movers(quotes_->movers()); });

    connect(quotes_, &QuoteService::quotes_updated, this, [this]() {
        board_view_->board()->set_quotes(quotes_->quotes());
        detail_->set_quotes(quotes_->quotes());
        pulse_->set_movers(quotes_->movers());   // 脉冲条跟着每轮行情走（不依赖变化信号）
        board_view_->board()->set_loading(quotes_->busy());
        ticker_->set_quotes(quotes_->quotes());
        update_status();
    });
    connect(quotes_, &QuoteService::status_changed, this, &MainWindow::update_status);
    connect(quotes_, &QuoteService::history_ready, this, [this](const QString& sym, QVector<Bar> bars) {
        if (chart_->isVisible() && chart_->symbol() == sym) chart_->set_bars(bars, chart_->range());
    });

    // ── 图表区间切换 ──
    connect(chart_, &ChartOverlay::range_changed, this,
            [this](const QString& sym, const QString& range, const QString& interval) {
                if (sym.isEmpty()) return;
                chart_->set_error(QStringLiteral("加载中…"));
                quotes_->fetch_history(sym, range, interval, [this, sym](bool ok, QVector<Bar> bars) {
                    if (!chart_->isVisible() || chart_->symbol() != sym) return;
                    if (!ok) chart_->set_error(QStringLiteral("取不到历史数据（检查行情依赖与网络）"));
                });
            });

    connect(pyenv_, &PyEnv::progress, this, [this](const QString& stage, const QString& detail) {
        status_error_->setText(QStringLiteral("%1：%2").arg(stage, detail));
    });
    connect(pyenv_, &PyEnv::finished, this, [this](bool ok, const QString& msg) {
        status_error_->setText(ok ? QString() : msg);
        if (ok) {
            quotes_->refresh_now();
            quotes_->refresh_sparklines();
        } else {
            QMessageBox::warning(this, QStringLiteral("行情依赖安装失败"), msg);
        }
    });

    schedule_news_timer();
}

void MainWindow::schedule_news_timer() {
    if (!news_timer_) {
        news_timer_ = new QTimer(this);
        connect(news_timer_, &QTimer::timeout, this, &MainWindow::refresh_news);
    }
    news_timer_->start(Config::instance().news_refresh_minutes() * 60 * 1000);
}

void MainWindow::refresh_news() {
    news_->refresh_all(true);
}

void MainWindow::install_python() {
    if (pyenv_->repairing()) return;
    status_error_->setText(QStringLiteral("正在修复行情依赖…"));
    pyenv_->repair();
}

void MainWindow::apply_filters() {
    const QVector<NewsItem> view =
        news_->filtered(current_category_, current_sources_, current_keyword_);
    list_->set_items(view);
    update_status();
}

void MainWindow::refresh_hot_clusters() {
    pulse_->set_clusters(news_->hot_clusters(2, 360));
}

void MainWindow::debug_open_first_news() {
    const auto items = list_->items_shown();
    if (!items.isEmpty()) open_news(items.first());
}

void MainWindow::debug_open_chart(const QString& symbol) { open_chart(symbol); }

int MainWindow::debug_item_count() const { return list_->item_count(); }

void MainWindow::set_left_panel_visible(bool on) {
    if (!filter_ || !splitter_) return;
    if (on) {
        filter_->show();
        if (splitter_saved_sizes_.size() == 3) splitter_->setSizes(splitter_saved_sizes_);
    } else {
        splitter_saved_sizes_ = splitter_->sizes();   // 记住宽度，显示时原样还回去
        filter_->hide();
    }
    if (left_toggle_) {
        left_toggle_->setText(on ? QStringLiteral("◀ 隐藏筛选栏") : QStringLiteral("▶ 显示筛选栏"));
        left_toggle_->setChecked(!on);
    }
    Config::instance().set_left_panel_visible(on);
    Config::instance().save();
    qInfo("[NewsBoard] 左栏%s，三栏宽度 = %d / %d / %d",
          on ? "显示" : "隐藏", splitter_->sizes().value(0), splitter_->sizes().value(1),
          splitter_->sizes().value(2));
}

void MainWindow::update_status() {
    // 状态栏只负责「状态」，不承担操作、也不堆细节：
    //   正常时：318 条 · 12 未读   ● 5s
    //   异常时：才把限流/延迟/依赖缺失用颜色写出来
    const int total = list_->item_count();
    const int unread = list_->unread_count();
    status_news_->setText(unread > 0 ? QStringLiteral("%1 条 · %2 未读").arg(total).arg(unread)
                                    : QStringLiteral("%1 条 · 已读完").arg(total));

    // 美股时段（以标普 500 为代表）
    QString us_session;
    for (const auto& quote : quotes_->quotes()) {
        if (quote.symbol == QLatin1String("^GSPC")) {
            us_session = quote.sessionLabel;
            break;
        }
    }

    // 行情：一个状态点 + 刷新间隔；细节（项数/时段/数据源/倒计时）都放 tooltip
    const bool cooling = quotes_->in_cooldown();
    const bool broken = !pyenv_->is_ready();
    bool any_delay = false, any_sina = false;
    for (const auto& quote : quotes_->quotes()) {
        if (quote.delayed) any_delay = true;
        if (quote.sourceLabel.contains(QStringLiteral("新浪"))) any_sina = true;
    }

    const QString dot = broken ? QStringLiteral("●") : cooling ? QStringLiteral("●")
                         : any_delay ? QStringLiteral("●")
                                     : QStringLiteral("●");
    const QString dot_color = broken ? QStringLiteral("#f04438")
                              : cooling ? QStringLiteral("#f04438")
                              : any_delay ? QStringLiteral("#e08b12")
                                          : QStringLiteral("#12b76a");
    const QString head = cooling ? QStringLiteral("行情限流")
                         : broken ? QStringLiteral("行情环境缺失")
                         : any_delay ? QStringLiteral("部分品种延迟")
                                     : QStringLiteral("行情正常");
    status_quotes_->setText(QStringLiteral("<span style='color:%1'>%2</span> %3 · %4s")
                                .arg(dot_color, dot, head)
                                .arg(Config::instance().quote_refresh_seconds()));
    status_quotes_->setTextFormat(Qt::RichText);

    QStringList detail;
    detail << QStringLiteral("行情 %1 项").arg(quotes_->quotes().size());
    if (!us_session.isEmpty()) detail << QStringLiteral("美股 %1").arg(us_session);
    detail << QStringLiteral("每 %1 秒一轮").arg(Config::instance().quote_refresh_seconds());
    if (any_sina) detail << QStringLiteral("期货走新浪实时源");
    if (any_delay) detail << QStringLiteral("有品种为源端延迟（Yahoo 对 CME/CBOT 期货延迟 10 分钟）");
    if (quotes_->busy()) detail << QStringLiteral("本轮刷新中");
    else if (quotes_->seconds_to_next() > 0)
        detail << QStringLiteral("%1 秒后刷新").arg(quotes_->seconds_to_next());
    if (cooling) detail << QStringLiteral("限流冷却中：%1 秒后自动恢复").arg(quotes_->cooldown_seconds_left());
    if (!quotes_->last_error().isEmpty()) detail << QStringLiteral("最近一次错误：%1").arg(quotes_->last_error().left(120));
    detail << QStringLiteral("资讯：%1 个源").arg(news_->feed_count());
    status_quotes_->setToolTip(detail.join(QStringLiteral("\n")));

    // 只有真出问题时才占用状态栏右侧那块位置
    const QString cur = status_error_->text();
    const bool showing_install = cur.startsWith(QStringLiteral("正在安装")) ||
                                 cur.startsWith(QStringLiteral("准备")) ||
                                 cur.startsWith(QStringLiteral("安装依赖"));
    if (showing_install) return;

    filter_->set_repair_visible(!pyenv_->is_ready());

    if (broken) {
        status_error_->setStyleSheet(QStringLiteral("color:#f04438;font-size:10px;"));
        status_error_->setText(QStringLiteral("行情运行环境（runtime/）缺失"));
    } else if (cooling) {
        status_error_->setStyleSheet(QStringLiteral("color:#f04438;font-size:10px;"));
        status_error_->setText(QStringLiteral("Yahoo 限流 · %1s 后重试").arg(quotes_->cooldown_seconds_left()));
    } else if (!quotes_->last_error().isEmpty()) {
        status_error_->setStyleSheet(QStringLiteral("color:#facc15;font-size:10px;"));
        status_error_->setText(QStringLiteral("行情：%1").arg(quotes_->last_error().left(60)));
    } else {
        status_error_->setText(QString());
    }
}

void MainWindow::open_news(const NewsItem& item) {
    if (!item.read) {
        news_->mark_read(item.id, true);
        news_->save_cache();
        apply_filters();
    }
    detail_->set_quotes(quotes_->quotes());
    detail_->show_item(item);
    layout_overlays();
    detail_->setFocus();
}

void MainWindow::open_chart(const QString& symbol) {
    if (symbol.isEmpty()) return;
    chart_->show_symbol(symbol);
    layout_overlays();
    quotes_->fetch_history(symbol, chart_->range(), "1d", [](bool, QVector<Bar>) {});
}

void MainWindow::layout_overlays() {
    QWidget* central = centralWidget();
    if (!central) return;

    if (detail_ && detail_->isVisible()) {
        const QRect area = list_->geometry();
        detail_->setGeometry(area.adjusted(12, 12, -12, -12));
        detail_->raise();
    }
    if (chart_ && chart_->isVisible()) {
        const QRect area = board_view_->geometry();
        chart_->setGeometry(area.adjusted(8, 8, -8, -8));
        chart_->raise();
    }
}

void MainWindow::resizeEvent(QResizeEvent* e) {
    QMainWindow::resizeEvent(e);
    layout_overlays();
}

void MainWindow::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {
        if (detail_->isVisible()) detail_->hide_overlay();
        if (chart_->isVisible()) chart_->hide_overlay();
        return;
    }
    if (e->key() == Qt::Key_F5) {
        refresh_news();
        quotes_->refresh_now();
        return;
    }
    QMainWindow::keyPressEvent(e);
}

void MainWindow::closeEvent(QCloseEvent* e) {
    // 截图/自检模式（NB_SHOT）是脚本跑的无头验证，窗口尺寸来自离屏平台的默认屏幕，
    // 存下来会把用户真实桌面上的窗口越改越小 —— 这种运行不写几何。
    if (!qEnvironmentVariableIsSet("NB_SHOT"))
        Config::instance().set_window_geometry(saveGeometry());
    Config::instance().save();
    news_->save_cache();
    quotes_->stop();
    QMainWindow::closeEvent(e);
}

} // namespace nb
