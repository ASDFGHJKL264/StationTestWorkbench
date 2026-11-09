#pragma once
#include <QApplication>
#include <QPalette>
#include <QString>

namespace ToolStyle
{
inline QPalette lightPalette()
{
    QPalette palette;
    palette.setColor(QPalette::All, QPalette::Window, QColor("#f3f3f3"));
    palette.setColor(QPalette::All, QPalette::WindowText, QColor("#222222"));
    palette.setColor(QPalette::All, QPalette::Base, Qt::white);
    palette.setColor(QPalette::All, QPalette::AlternateBase, QColor("#fafafa"));
    palette.setColor(QPalette::All, QPalette::Text, QColor("#222222"));
    palette.setColor(QPalette::All, QPalette::Button, QColor("#f3f3f3"));
    palette.setColor(QPalette::All, QPalette::ButtonText, QColor("#222222"));
    palette.setColor(QPalette::All, QPalette::BrightText, Qt::white);
    palette.setColor(QPalette::All, QPalette::Highlight, QColor("#dce7f2"));
    palette.setColor(QPalette::All, QPalette::HighlightedText, QColor("#111111"));
    palette.setColor(QPalette::All, QPalette::PlaceholderText, QColor("#666666"));
    palette.setColor(QPalette::All, QPalette::ToolTipBase, QColor("#ffffdc"));
    palette.setColor(QPalette::All, QPalette::ToolTipText, QColor("#222222"));
    palette.setColor(QPalette::All, QPalette::Link, QColor("#0659a3"));
    palette.setColor(QPalette::All, QPalette::LinkVisited, QColor("#663c90"));
    palette.setColor(QPalette::All, QPalette::Light, Qt::white);
    palette.setColor(QPalette::All, QPalette::Midlight, QColor("#f7f7f7"));
    palette.setColor(QPalette::All, QPalette::Mid, QColor("#c8c8c8"));
    palette.setColor(QPalette::All, QPalette::Dark, QColor("#999999"));
    palette.setColor(QPalette::All, QPalette::Shadow, QColor("#777777"));
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    palette.setColor(QPalette::All, QPalette::Accent, QColor("#346ca3"));
#endif
    for (auto role :
         {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::PlaceholderText})
        palette.setColor(QPalette::Disabled, role, QColor("#666666"));
    palette.setColor(QPalette::Disabled, QPalette::Base, QColor("#f3f3f3"));
    palette.setColor(QPalette::Disabled, QPalette::Button, QColor("#eeeeee"));
    return palette;
}

inline void configureApplication(QApplication &application)
{
    // Native dark controls cannot be mixed with the interface's fixed light surfaces.
    application.setStyle(QStringLiteral("Fusion"));
    application.setPalette(lightPalette());
}

// Control behavior stays with Qt; the light palette covers every color group.
inline QString stylesheet()
{
    return QStringLiteral(
        "QWidget { color:#222222; background:#f3f3f3; "
        "font-family:'Microsoft YaHei UI','Microsoft YaHei'; "
        "font-size:13px; }"
        "QWidget:disabled { color:#666666; }"
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
        "QPushButton,QToolButton { min-height:23px; padding:2px 8px; background:#f3f3f3; "
        "border:1px solid #b8b8b8; border-radius:2px; }"
        "QPushButton:hover,QToolButton:hover { background:#e6e6e6; }"
        "QPushButton:pressed,QToolButton:pressed { background:#dce7f2; }"
        "QPushButton:disabled,QToolButton:disabled { background:#eeeeee; border-color:#d0d0d0; }"
        "QLineEdit,QSpinBox,QComboBox { min-height:23px; color:#222222; background:white; "
        "placeholder-text-color:#666666; selection-background-color:#dce7f2; "
        "selection-color:#111111; }"
        "QLineEdit:disabled,QSpinBox:disabled,QComboBox:disabled { color:#666666; "
        "background:#eeeeee; }"
        "QTableView,QTableWidget { background:white; alternate-background-color:#fafafa; "
        "color:#222222; "
        "border:1px solid #c8c8c8; gridline-color:#dddddd; selection-background-color:#dce7f2; "
        "selection-color:#111111; }"
        "QTableView::item,QTableWidget::item { padding:2px 4px; }"
        "QHeaderView::section { background:#eeeeee; border:0; border-right:1px solid #d2d2d2; "
        "border-bottom:1px solid #c8c8c8; padding:4px 6px; color:#333333; font-weight:400; }"
        "QListView,QPlainTextEdit { background:white; color:#222222; border:1px solid #c8c8c8; "
        "selection-background-color:#dce7f2; selection-color:#111111; }"
        "QListView::item { padding:3px; }"
        "QPlainTextEdit { font-family:Consolas,'Microsoft YaHei'; font-size:12px; }"
        "QTabWidget::pane { border:1px solid #c8c8c8; }"
        "QTabBar::tab { color:#222222; background:#eeeeee; border:1px solid #c8c8c8; "
        "padding:4px 8px; }"
        "QTabBar::tab:selected { background:white; }"
        "QTabBar::tab:disabled { color:#666666; }"
        "QSplitter::handle { background:#dddddd; }"
        "QStatusBar { color:#555555; background:#f3f3f3; }");
}
} // namespace ToolStyle
