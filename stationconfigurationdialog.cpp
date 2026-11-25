#include "stationconfigurationdialog.h"
#include "runtimeconfiguration.h"
#include "toolstyle.h"
#include <QGuiApplication>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

StationConfigurationDialog::StationConfigurationDialog(const QString &dataDirectory,
                                                       const QList<StationEndpoint> &endpoints,
                                                       QWidget *parent)
    : QDialog(parent), m_dataDirectory(dataDirectory)
{
    setObjectName("stationConfigurationDialog");
    setWindowTitle(QStringLiteral("工位连接配置"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setPalette(ToolStyle::lightPalette());
    setStyleSheet(ToolStyle::stylesheet());
    resize(620, 310);
    if (auto *screen = QGuiApplication::primaryScreen())
        resize(qMin(620, screen->availableGeometry().width() - 40),
               qMin(310, screen->availableGeometry().height() - 60));
    // 排列配置表格、动态状态和保存操作。
    auto *layout = new QVBoxLayout(this);
    // 供无工位连接的修复界面填入初始本机配置。
    auto settings = endpoints;
    if (settings.isEmpty())
        for (int index = 0; index < 4; ++index)
            settings.append({index + 1, "127.0.0.1", quint16(46501 + index)});
    m_table = new QTableWidget(settings.size(), 3, this);
    m_table->setObjectName("stationConnectionTable");
    m_table->setHorizontalHeaderLabels(
        {QStringLiteral("工位"), QStringLiteral("IP / 主机名"), QStringLiteral("端口")});
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setFocusPolicy(Qt::NoFocus);
    m_table->verticalHeader()->setDefaultSectionSize(40);
    for (int row = 0; row < settings.size(); ++row)
    {
        // 保存当前行对应的固定业务工位编号。
        auto *station = new QTableWidgetItem(
            QStringLiteral("工位 %1").arg(settings[row].stationId, 2, 10, QChar('0')));
        station->setData(Qt::UserRole, settings[row].stationId);
        m_table->setItem(row, 0, station);
        // 编辑该工位的独立连接地址。
        auto *host = new QLineEdit(settings[row].host, m_table);
        host->setObjectName(QString("stationHost%1").arg(settings[row].stationId));
        host->setMaxLength(253);
        host->setClearButtonEnabled(true);
        m_table->setCellWidget(row, 1, host);
        // 限制TCP端口为1至65535，不接收截断或溢出数值。
        auto *port = new QSpinBox(m_table);
        port->setObjectName(QString("stationPort%1").arg(settings[row].stationId));
        port->setRange(1, 65535);
        port->setValue(settings[row].port);
        m_table->setCellWidget(row, 2, port);
    }
    layout->addWidget(m_table, 1);
    m_status = new QLabel(this);
    m_status->setObjectName("stationConfigurationStatus");
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    m_status->hide();
    layout->addWidget(m_status);
    // 只保存下次启动使用的配置，不中断正在进行的测试。
    auto *saveButton = new QPushButton(QStringLiteral("保存（重启后生效）"), this);
    saveButton->setObjectName("saveStationConfigurationButton");
    layout->addWidget(saveButton, 0, Qt::AlignRight);
    connect(saveButton, &QPushButton::clicked, this, &StationConfigurationDialog::save);
}

void StationConfigurationDialog::showError(const QString &message)
{
    m_status->setText(message);
    m_status->setStyleSheet("QLabel { color:#a82432; }");
    m_status->setVisible(!message.isEmpty());
}

void StationConfigurationDialog::save()
{
    // 收集所有输入，整体校验成功后才替换原文件。
    QList<StationEndpoint> endpoints;
    for (int row = 0; row < m_table->rowCount(); ++row)
        endpoints.append({m_table->item(row, 0)->data(Qt::UserRole).toInt(),
                          qobject_cast<QLineEdit *>(m_table->cellWidget(row, 1))->text(),
                          quint16(qobject_cast<QSpinBox *>(m_table->cellWidget(row, 2))->value())});
    // 接收校验或磁盘保存失败的具体原因。
    QString error;
    if (!RuntimeConfiguration::saveStationEndpoints(m_dataDirectory, endpoints, &error))
    {
        showError(error);
        return;
    }
    m_status->setStyleSheet("QLabel { color:#333333; }");
    m_status->setText(QStringLiteral("配置已保存，重新启动工作台后生效。"));
    m_status->show();
}
