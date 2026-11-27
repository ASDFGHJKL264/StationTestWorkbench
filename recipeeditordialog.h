#pragma once
#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>

class WorkbenchController;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

class RecipeEditorDialog final : public QDialog
{
    Q_OBJECT
  public:
    // 创建复用后台控制器的独立配方编辑窗口。
    explicit RecipeEditorDialog(WorkbenchController *controller, QWidget *parent = nullptr);
    // 首次打开时复制指定已发布配方，已有草稿保持原样。
    void prepare(const QString &sourceVersion);
    // 校验并导入JSON草稿，失败或取消确认时保留当前内容。
    bool importFile(const QString &path);
    // 原子导出有效草稿，导出操作不发布或覆盖数据库配方。
    bool exportFile(const QString &path);
    // 工作台退出前等待发布结果，并确认是否放弃手动编辑的草稿。
    bool confirmApplicationClose();

  signals:
    // 仅在数据库确认发布成功后通知主窗口选用该版本。
    void recipeCommitted(const QString &version);

  private:
    // 刷新来源列表，不改变正在编辑的草稿。
    void setRecipes(const QJsonArray &recipes);
    // 清空名称和版本，建立包含一个测试项目的新草稿。
    void resetDraft();
    // 将已验证的配方内容填入表单，dirty标明是否需要保护该草稿。
    void loadRecipe(const QJsonObject &recipe, bool dirty);
    // 将来源列表中的配方复制成一个未发布的新版本。
    void copySelectedRecipe();
    // 为来源版本生成未被使用且长度有效的新版本号。
    QString suggestedVersion(const QString &sourceVersion) const;
    // 查询版本号是否已经发布。
    bool versionExists(const QString &version) const;
    // 增加拥有独立项目编号的测试行。
    void addStep();
    // 删除选中的测试行，至少保留一个项目。
    void removeStep();
    // 按direction移动选中行，保持该行的字段完整。
    void moveStep(int direction);
    // 汇总表单并校验配方，error接收首个失败原因。
    bool buildRecipe(QJsonObject *recipe, QString *error) const;
    // 将有效的新版本提交后台，等待明确的发布结果。
    void publish();
    // 在覆盖手动编辑的未发布内容前询问用户。
    bool confirmReplace();
    // 标记手动编辑，刷新可用动作并清除过期的操作提示。
    void markDirty();
    // 根据后台状态、发布在途状态和当前选择更新操作按钮。
    void updateActions();
    // 显示真实操作结果，error为错误提示样式。
    void showMessage(const QString &message, bool error = false);

    // 指向与主窗口共享的后台控制器。
    WorkbenchController *m_controller;
    // 填写显示给操作员的配方名称。
    QLineEdit *m_name;
    // 填写不可覆盖的配方版本号。
    QLineEdit *m_version;
    // 选择用于复制的已发布配方。
    QComboBox *m_source;
    // 按执行顺序编辑项目编号、名称、单位与数值规则。
    QTableWidget *m_steps;
    // 显示发布、文件操作或校验的实际结果。
    QLabel *m_status;
    // 创建新的空白草稿。
    QPushButton *m_new;
    // 复制选中的已发布配方。
    QPushButton *m_copy;
    // 增加一个测试项目。
    QPushButton *m_add;
    // 删除选中的测试项目。
    QPushButton *m_remove;
    // 将选中的测试项目上移。
    QPushButton *m_up;
    // 将选中的测试项目下移。
    QPushButton *m_down;
    // 从JSON文件载入配方草稿。
    QPushButton *m_import;
    // 将有效草稿保存为JSON文件。
    QPushButton *m_export;
    // 提交配方并等待数据库回报。
    QPushButton *m_publish;
    // 缓存已发布配方，供复制和版本冲突提示使用。
    QJsonArray m_recipes;
    // 记录本窗口正在等待确认的完整配方对象。
    QJsonObject m_pending;
    // 在批量填表时抑制草稿变更标记。
    bool m_loading = false;
    // 保护尚未发布的手动编辑和已导入草稿。
    bool m_dirty = false;
    // 发布期间冻结表单，避免结果回报对应到另一个草稿。
    bool m_busy = false;
    // 标记窗口已建立草稿，重开时不重新覆盖来源内容。
    bool m_initialized = false;
    // 标记后台可用，离线编辑和导出仍可继续。
    bool m_backendReady = false;
};
