// NewsBoard — 入口
#include "MainWindow.h"
#include "core/Config.h"
#include "core/QuoteService.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QIcon>
#include <QTimer>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("NewsBoard"));
    app.setApplicationDisplayName(QStringLiteral("NewsBoard"));
    app.setOrganizationName(QStringLiteral("NewsBoard"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));

    // 中文优先的字体链，避免中文出现方框
    QFont f = app.font();
    const QStringList preferred{QStringLiteral("Noto Sans CJK SC"), QStringLiteral("Source Han Sans SC"),
                                QStringLiteral("WenQuanYi Micro Hei"), QStringLiteral("Microsoft YaHei")};
    const QStringList families = QFontDatabase::families();
    for (const auto& name : preferred) {
        if (families.contains(name)) {
            f.setFamily(name);
            break;
        }
    }
    f.setPointSize(10);
    app.setFont(f);
    app.setStyleSheet(nb::theme::global_qss());

    // 行情自检模式：NB_SELFTEST=1 ./NewsBoard
    // 跑完「报价 / 迷你走势 / 历史K线」三条路径，结果写到
    //   ~/.local/share/newsboard/selftest.json
    // 同时打印到终端后退出。用于快速判断“行情到底通没通”。
    if (qEnvironmentVariableIsSet("NB_SELFTEST")) {
        nb::Config::instance().load();
        auto* svc = new nb::QuoteService(&app);
        svc->run_selftest(nb::Config::instance().data_dir() + "/selftest.json");
        return app.exec();
    }

    nb::MainWindow w;
    w.show();

    // 截图模式：NB_SHOT=/tmp/x.png [NB_SHOT_DELAY=9000] ./run.sh
    // 等界面把数据画出来之后抓一张窗口图再退出。用于排查界面问题（比如列重叠）。
    const QByteArray shot = qgetenv("NB_SHOT");
    if (!shot.isEmpty()) {
        const int delay = qEnvironmentVariableIsSet("NB_SHOT_DELAY")
                              ? qgetenv("NB_SHOT_DELAY").toInt()
                              : 9000;
        QTimer::singleShot(delay, &app, [&w, shot]() {
            const bool ok = w.grab().save(QString::fromUtf8(shot));
            qInfo("screenshot %s -> %s", ok ? "ok" : "FAILED", shot.constData());
            QCoreApplication::quit();
        });
    }

    return app.exec();
}
