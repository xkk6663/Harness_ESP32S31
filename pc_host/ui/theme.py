"""暗色主题 QSS（Korvo 统一上位机）。

色板：底 #14181f / 卡 #1c2330 / 线 #273041 / 主色青 #2dd4bf /
警示 #f0a45d / 错误 #ef6b6b / 文 #e7edf5 / 次 #8b96a8
"""
from __future__ import annotations

QSS = """
* { font-family: "Microsoft YaHei UI", "Segoe UI", sans-serif; font-size: 13px; }

QMainWindow, QWidget#Root { background: #14181f; }
QWidget#Sidebar { background: #171c26; border-right: 1px solid #273041; }

QLabel { color: #e7edf5; background: transparent; }
QLabel#AppTitle   { font-size: 17px; font-weight: 700; color: #e7edf5; }
QLabel#AppSub     { font-size: 11px; color: #8b96a8; }
QLabel#PageTitle  { font-size: 20px; font-weight: 700; }
QLabel#Secondary  { color: #8b96a8; font-size: 12px; }
QLabel#H2         { font-size: 14px; font-weight: 600; color: #cfd8e3; }
QLabel#StatusDotOn  { color: #2dd4bf; font-size: 15px; }
QLabel#StatusDotOff { color: #5a6478; font-size: 15px; }
QLabel#Badge { color: #2dd4bf; background: rgba(45,212,191,.10);
               border: 1px solid rgba(45,212,191,.35); border-radius: 9px;
               padding: 2px 10px; font-size: 11px; }
QLabel#BadgeWarn { color: #f0a45d; background: rgba(240,164,93,.10);
                   border: 1px solid rgba(240,164,93,.35); }
QLabel#TrackTitle  { font-size: 17px; font-weight: 700; }
QLabel#TrackArtist { color: #8b96a8; }

/* ---- 导航 ---- */
QPushButton#Nav {
    color: #aab4c4; background: transparent; border: none; border-radius: 8px;
    padding: 10px 14px; text-align: left; font-size: 14px;
}
QPushButton#Nav:hover { background: #1d2432; color: #e7edf5; }
QPushButton#Nav:checked {
    background: rgba(45,212,191,.12); color: #2dd4bf; font-weight: 600;
    border-left: 3px solid #2dd4bf; padding-left: 11px;
}

/* ---- 按钮 ---- */
QPushButton {
    background: #232c3b; color: #e7edf5; border: 1px solid #2e3949;
    border-radius: 8px; padding: 7px 16px;
}
QPushButton:hover { background: #2a3446; border-color: #3b4960; }
QPushButton:pressed { background: #202836; }
QPushButton:disabled { color: #5a6478; background: #1b2130; }
QPushButton#Primary {
    background: #14b8a6; border: none; color: #04211d; font-weight: 700;
}
QPushButton#Primary:hover { background: #17c9b5; }
QPushButton#Primary:disabled { background: #1d3a37; color: #53726d; }
QPushButton#Danger { color: #ef6b6b; border-color: #4a2b31; }
QPushButton#Danger:hover { background: #33222a; }
QPushButton#Toggled[checked="true"] { background: #14b8a6; color: #04211d; font-weight: 700; border: none; }

/* ---- 卡片 ---- */
QFrame#Card {
    background: #1c2330; border: 1px solid #273041; border-radius: 10px;
}
QFrame#Card QPlainTextEdit, QFrame#Card QListWidget, QFrame#Card QTableWidget {
    background: #171c26; border: 1px solid #273041; border-radius: 8px;
}

/* ---- 输入 / 列表 / 表格 ---- */
QPlainTextEdit, QListWidget, QTableWidget, QLineEdit {
    background: #171c26; color: #dfe6ef; border: 1px solid #273041;
    border-radius: 8px; padding: 4px;
}
QTableWidget::item, QListWidget::item { padding: 5px; }
QTableWidget::item:selected, QListWidget::item:selected { background: rgba(45,212,191,.16); color: #e7edf5; }
QHeaderView::section {
    background: #1c2330; color: #8b96a8; border: none;
    border-bottom: 1px solid #273041; padding: 6px;
}
QTableCornerButton::section { background: #1c2330; border: none; }

/* ---- 进度条（电平表）---- */
QProgressBar {
    background: #171c26; border: 1px solid #273041; border-radius: 7px;
    text-align: center; color: #8b96a8; font-size: 11px; min-height: 16px;
}
QProgressBar::chunk { background: #2dd4bf; border-radius: 6px; }

/* ---- 滚动条 ---- */
QScrollBar:vertical { background: transparent; width: 9px; margin: 2px; }
QScrollBar::handle:vertical { background: #2e3949; border-radius: 4px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: #3b4960; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
QScrollBar:horizontal { background: transparent; height: 9px; margin: 2px; }
QScrollBar::handle:horizontal { background: #2e3949; border-radius: 4px; min-width: 30px; }

/* ---- 其它 ---- */
QCheckBox { color: #cfd8e3; spacing: 8px; }
QCheckBox::indicator { width: 16px; height: 16px; border-radius: 4px;
                       border: 1px solid #3b4960; background: #171c26; }
QCheckBox::indicator:checked { background: #2dd4bf; border-color: #2dd4bf; }
QComboBox { background: #232c3b; color: #e7edf5; border: 1px solid #2e3949; border-radius: 8px; padding: 5px 10px; }
QComboBox QAbstractItemView { background: #1c2330; color: #e7edf5; selection-background-color: #2dd4bf; }
"""
