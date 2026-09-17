#include "core/PyEnv.h"

#include "core/Config.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>

namespace nb {

PyEnv::PyEnv(QObject* parent) : QObject(parent) {}

bool PyEnv::is_ready() const {
    return QFileInfo::exists(Config::instance().python_path());
}

void PyEnv::repair() {
    if (repairing_) return;

    const QString py = Config::instance().python_path();
    if (!QFileInfo::exists(py)) {
        emit finished(false, QStringLiteral("自带 Python 运行环境缺失：%1\n"
                                            "请重新拷贝整个 newsboard 目录（runtime/ 必须一起拷）")
                                 .arg(py));
        return;
    }

    repairing_ = true;
    emit progress(QStringLiteral("修复"), QStringLiteral("正在用自带 pip 重装 yfinance…"));

    auto* p = new QProcess(this);
    p->setProcessChannelMode(QProcess::MergedChannels);

    // 自带解释器是 python-build-standalone，pip 需要 --break-system-packages 才肯装
    QStringList args{QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install"),
                     QStringLiteral("--break-system-packages"), QStringLiteral("--upgrade"),
                     QStringLiteral("yfinance")};

    connect(p, &QProcess::readyReadStandardOutput, this, [this, p]() {
        const QString line = QString::fromUtf8(p->readAllStandardOutput()).trimmed();
        if (!line.isEmpty()) emit progress(QStringLiteral("修复"), line.right(120));
    });
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus) {
        repairing_ = false;
        emit finished(code == 0, code == 0 ? QStringLiteral("行情依赖已修复")
                                           : QStringLiteral("修复失败（退出码 %1），检查网络后重试").arg(code));
        p->deleteLater();
    });

    p->start(py, args);
}

} // namespace nb
