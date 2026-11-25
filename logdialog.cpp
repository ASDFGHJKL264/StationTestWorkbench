#include "logdialog.h"
#include "models.h"
#include "toolstyle.h"
#include "workbenchcontroller.h"
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QScreen>
#include <QScrollBar>
#include <QVBoxLayout>

LogDialog::LogDialog(WorkbenchController *controller, QWidget *parent)
    : QDialog(parent), m_logs(new BoundedLogModel(1000, this))
{
    setObjectName("logDialog");
    setWindowTitle(QStringLiteral("运行日志"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setPalette(ToolStyle::lightPalette());
    setStyleSheet(ToolStyle::stylesheet());
    resize(920, 600);
    // 限制窗口尺寸，适配当前屏幕可用区域。
    if (auto *screen = QGuiApplication::primaryScreen())
        resize(qMin(920, screen->availableGeometry().width() - 40),
               qMin(600, screen->availableGeometry().height() - 60));
    // 排列日志列表和条数。
    auto *layout = new QVBoxLayout(this);
    m_logView = new QListView(this);
    m_logView->setObjectName("logView");
    m_logView->setModel(m_logs);
    m_logView->setUniformItemSizes(false);
    m_logView->setWordWrap(true);
    m_logView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    layout->addWidget(m_logView, 1);
    // 在列表下方显示当前缓存条数。
    auto *footer = new QHBoxLayout;
    m_count = new QLabel(QStringLiteral("0 条"), this);
    m_count->setObjectName("logCountLabel");
    footer->addWidget(m_count);
    footer->addStretch();
    layout->addLayout(footer);
    connect(controller, &WorkbenchController::logMessage, this,
            [this](const QString &text, bool warning)
            {
                // 用户停在末尾时才跟随新日志。
                const bool atBottom = m_logView->verticalScrollBar()->value() >=
                                      m_logView->verticalScrollBar()->maximum() - 2;
                m_logs->append(text, warning);
                m_count->setText(QStringLiteral("%1 条").arg(m_logs->rowCount()));
                if (atBottom)
                    m_logView->scrollToBottom();
            });
}
