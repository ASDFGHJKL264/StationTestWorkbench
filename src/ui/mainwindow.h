#pragma once
#include <QMainWindow>
#include "runtime/workbenchcontroller.h"

class StationModel;
class StepModel;
class HistoryModel;
class BoundedLogModel;
class QTableView;
class QListView;
class QLineEdit;
class QComboBox;
class QPushButton;
class QLabel;
class QSortFilterProxyModel;

class MainWindow final : public QMainWindow
{
    Q_OBJECT
  public:
    MainWindow(const QString &dataDirectory, const QList<StationEndpoint> &endpoints,
               QWidget *parent = nullptr);
    ~MainWindow() override;
    WorkbenchController *controller() const
    {
        return m_controller;
    }

  private:
    void buildUi();
    void showSelectedStation();
    void syncRecipePreview();
    void showHistorySelection();
    void updateCommandAvailability();
    void queryHistory(bool older);
    void importRecipe();
    void exportSelectedReport();
    int selectedStationId() const;
    WorkbenchController *m_controller;
    StationModel *m_stations;
    StepModel *m_steps, *m_historySteps;
    HistoryModel *m_history;
    BoundedLogModel *m_logs;
    QSortFilterProxyModel *m_historyProxy;
    QTableView *m_stationView, *m_stepView, *m_historyView, *m_historyStepView;
    QListView *m_logView;
    QLineEdit *m_snInput, *m_historyFilter;
    QComboBox *m_recipe;
    QPushButton *m_start, *m_cancel, *m_query, *m_older, *m_import, *m_export;
    QLabel *m_stationTitle, *m_detail, *m_runIdentity, *m_connectionSummary, *m_historySummary,
        *m_historyDetail;
    QJsonArray m_recipes;
    bool m_backendReady = false, m_historyBusy = false, m_hasOlder = false, m_reportBusy = false;
    QString m_queryFilter;
};
