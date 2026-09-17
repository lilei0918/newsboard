#pragma once
// NewsBoard — 自带 Python 运行环境的检查与修复
//
// 正常情况下什么都不用做：解释器和 yfinance 都已经打进 <项目根>/runtime/。
// 这个类只负责：
//   · is_ready()  报告运行环境是否可用（界面据此决定是否显示提示）
//   · repair()    万一 site-packages 被删坏，用自带解释器重装一次 yfinance
//                 （需要联网；不需要用户另装 Python 或 uv）

#include <QObject>
#include <QString>

namespace nb {

class PyEnv : public QObject {
    Q_OBJECT
  public:
    explicit PyEnv(QObject* parent = nullptr);

    /// 自带解释器是否就位
    bool is_ready() const;

    /// 解释器在、但 yfinance 缺失或损坏时用：调用自带 pip 重装 yfinance
    void repair();

    bool repairing() const { return repairing_; }

  signals:
    void progress(const QString& stage, const QString& detail);
    void finished(bool ok, const QString& message);

  private:
    bool repairing_ = false;
};

} // namespace nb
