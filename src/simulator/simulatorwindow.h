#pragma once
#include "stationserver.h"
#include <QMainWindow>
#include <QMap>
#include <QSet>

class SimulatorController;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

class SimulatorWindow final : public QMainWindow
{
    Q_OBJECT
  public:
    explicit SimulatorWindow(const QList<StationOptions> &options, bool autoStart = false,
                             QWidget *parent = nullptr);
    SimulatorController *controller() const;
    QJsonObject stationSnapshot(int stationId) const;

  private:
    void buildUi();
    void selectStation();
    int selectedStationId() const;
    StationOptions editedOptions() const;
    void startSelected();
    void stopSelected();
    void applySelected();
    void startAll();
    void stopAll();
    void receiveSnapshot(const QJsonObject &snapshot);
    void finishOperation(int stationId, const QString &operation, bool ok, const QString &error);
    void loadSettings();
    void refreshFaultChoices(const QString &selectedStep);
    void refreshTable();
    void refreshDetails();
    void refreshAvailability();
    void appendLog(int stationId, const QString &text, bool error = false);

    SimulatorController *m_controller;
    QMap<int, StationOptions> m_options;
    QMap<int, StationOptions> m_requestedOptions;
    QMap<int, QJsonObject> m_snapshots;
    QMap<int, QString> m_errors;
    QSet<int> m_pending;
    QTableWidget *m_stationTable = nullptr;
    QTableWidget *m_stepTable = nullptr;
    QLabel *m_connections = nullptr;
    QLabel *m_settingsTitle = nullptr;
    QLabel *m_faultRecipeHint = nullptr;
    QLabel *m_state = nullptr;
    QLabel *m_runSummary = nullptr;
    QLineEdit *m_directory = nullptr;
    QSpinBox *m_port = nullptr;
    QSpinBox *m_stepMs = nullptr;
    QComboBox *m_failStep = nullptr;
    QCheckBox *m_dropAck = nullptr;
    QPushButton *m_startAll = nullptr;
    QPushButton *m_stopAll = nullptr;
    QPushButton *m_start = nullptr;
    QPushButton *m_stop = nullptr;
    QPushButton *m_apply = nullptr;
    QPushButton *m_disconnect = nullptr;
    QPlainTextEdit *m_logs = nullptr;
};
