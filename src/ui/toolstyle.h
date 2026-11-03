#pragma once
#include <QString>

namespace ToolStyle
{
// Both windows use the same restrained tool layout; control behavior stays with Qt.
inline QString stylesheet()
{
    return QStringLiteral(
        "QWidget { font-family:'Microsoft YaHei UI','Microsoft YaHei'; font-size:13px; }"
        "QMainWindow { background:#f3f3f3; }"
        "QFrame#masthead { background:transparent; border:0; border-bottom:1px solid #cccccc; }"
        "QLabel#appTitle { color:#222222; font-size:14px; font-weight:600; }"
        "QLabel#connections,QLabel#connectionSummaryLabel,QLabel#simBadge { color:#555555; }"
        "QFrame#guide,QFrame#commandBar { background:transparent; border:0; }"
        "QLabel#guideLabel { color:#555555; }"
        "QGroupBox { border:1px solid #c8c8c8; border-radius:0; margin-top:9px; padding-top:8px; }"
        "QGroupBox::title { subcontrol-origin:margin; left:8px; padding:0 4px; color:#333333; }"
        "QLabel#stationTitle,QLabel#settingsTitle,QLabel#stateLabel { color:#222222; "
        "font-size:13px; font-weight:600; }"
        "QLabel#hint,QLabel#note,QLabel#detail,QLabel#runIdentity,QLabel#historyDetail,QLabel#"
        "runSummaryLabel { color:#555555; }"
        "QPushButton { min-height:23px; padding:2px 8px; }"
        "QLineEdit,QSpinBox,QComboBox { min-height:23px; }"
        "QTableView,QTableWidget { background:white; alternate-background-color:#fafafa; "
        "border:1px solid #c8c8c8; gridline-color:#dddddd; selection-background-color:#dce7f2; "
        "selection-color:#111111; }"
        "QTableView::item,QTableWidget::item { padding:2px 4px; }"
        "QHeaderView::section { background:#eeeeee; border:0; border-right:1px solid #d2d2d2; "
        "border-bottom:1px solid #c8c8c8; padding:4px 6px; color:#333333; font-weight:400; }"
        "QListView,QPlainTextEdit { background:white; color:#222222; border:1px solid #c8c8c8; }"
        "QListView::item { padding:3px; }"
        "QPlainTextEdit { font-family:Consolas,'Microsoft YaHei'; font-size:12px; }"
        "QTabWidget::pane { border:1px solid #c8c8c8; }"
        "QStatusBar { color:#555555; background:#f3f3f3; }");
}
} // namespace ToolStyle
