#pragma once
// NewsBoard — 左栏：分类 / 源管理 / 关键词 / 刷新设置

#include "core/Models.h"

#include <QSet>
#include <QWidget>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;

namespace nb {

class FilterPanel : public QWidget {
    Q_OBJECT
  public:
    explicit FilterPanel(QWidget* parent = nullptr);

    void reload_sources();
    void update_health(const QVector<FeedHealth>& health, int ok, int total);
    /// 运行环境缺失时才显示“修复”按钮
    void set_repair_visible(bool visible);

  signals:
    void category_changed(const QString& category);          // 空 = 全部
    void source_filter_changed(const QSet<QString>& source_ids);
    void keyword_changed(const QString& keyword);
    void highlight_keywords_changed(const QStringList& keywords);
    void feed_toggled(const QString& feed_id, bool enabled);
    void refresh_requested();
    void install_python_requested();
    void news_interval_changed(int minutes);
    void quote_interval_changed(int seconds);

  private:
    void emit_source_filter();

    QButtonGroup* category_group_ = nullptr;
    QLineEdit* search_ = nullptr;
    QLineEdit* highlight_ = nullptr;
    QListWidget* source_list_ = nullptr;
    QLabel* health_label_ = nullptr;
    QLabel* head_hint_ = nullptr;
    QComboBox* news_interval_ = nullptr;
    QComboBox* quote_interval_ = nullptr;
    QPushButton* install_btn_ = nullptr;
    QString current_category_;
};

} // namespace nb
