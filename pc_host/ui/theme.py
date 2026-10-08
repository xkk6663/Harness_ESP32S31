"""深色主题 QSS（Korvo 统一上位机）—— 法拉利官方风格（§74）。

色板：底 #181818 / 侧栏 #141414 / 卡 #1F1F1F / 输入 #161616 /
边框 #2A2A2A / 主色法拉利红 #DA291C / 文 #E8E8E8 / 次 #8A8A8A。
★ 0px 直角（用户拍板的品牌风格）：全局不写 border-radius。
对象名约定（Nav/Card/Primary/Danger/Badge/StatusDot* 等）与旧版完全一致，
本文件只动皮肤、不动任何页面代码。
"""
from __future__ import annotations

QSS = """
* { font-family: "Microsoft YaHei UI", "Segoe UI", sans-serif; font-size: 13px; }

QMainWindow, QWidget#Root { background: #181818; }
QWidget#Sidebar { background: #141414; border-right: 1px solid #2A2A2A; }
QDialog { background: #181818; color: #E8E8E8; }

QLabel { color: #E8E8E8; background: transparent; }
QLabel#AppTitle   { font-size: 17px; font-weight: 700; color: #FFFFFF; }
QLabel#AppSub     { font-size: 11px; color: #8A8A8A; }
QLabel#PageTitle  { font-size: 20px; font-weight: 700; color: #FFFFFF; }
QLabel#Secondary  { color: #8A8A8A; font-size: 12px; }
QLabel#H2         { font-size: 14px; font-weight: 600; color: #D6D6D6; }
QLabel#StatusDotOn  { color: #DA291C; font-size: 15px; }
QLabel#StatusDotOff { color: #4A4A4A; font-size: 15px; }
QLabel#Badge { color: #FF6B5E; background: rgba(218,41,28,.12);
               border: 1px solid rgba(218,41,28,.45); padding: 2px 10px;
               font-size: 11px; }
QLabel#BadgeWarn { color: #F0A45D; background: rgba(240,164,93,.10);
                   border: 1px solid rgba(240,164,93,.35); }
QLabel#TrackTitle  { font-size: 17px; font-weight: 700; color: #FFFFFF; }
QLabel#TrackArtist { color: #8A8A8A; }

/* ---- 导航 ---- */
QPushButton#Nav {
    color: #9A9A9A; background: transparent; border: none;
    padding: 10px 14px; text-align: left; font-size: 14px;
}
QPushButton#Nav:hover { background: #1F1F1F; color: #E8E8E8; }
QPushButton#Nav:checked {
    background: #1F1F1F; color: #DA291C; font-weight: 600;
    border-left: 3px solid #DA291C; padding-left: 11px;
}

/* ---- 按钮 ---- */
QPushButton {
    background: #232323; color: #E8E8E8; border: 1px solid #2A2A2A;
    padding: 7px 16px;
}
QPushButton:hover { background: #2A2A2A; border-color: #3A3A3A; }
QPushButton:pressed { background: #1A1A1A; }
QPushButton:disabled { color: #5A5A5A; background: #1C1C1C; border-color: #232323; }
QPushButton#Primary {
    background: #DA291C; border: 1px solid #DA291C; color: #FFFFFF; font-weight: 700;
}
QPushButton#Primary:hover { background: #E8351F; border-color: #E8351F; }
QPushButton#Primary:pressed { background: #B21F14; border-color: #B21F14; }
QPushButton#Primary:disabled { background: #3A1B18; color: #7A5049; border-color: #3A1B18; }
QPushButton#Danger { color: #FF6B5E; border-color: #4A2622; }
QPushButton#Danger:hover { background: #2A1B19; }
QPushButton#Toggled[checked="true"] {
    background: #DA291C; color: #FFFFFF; font-weight: 700; border: 1px solid #DA291C;
}

/* ---- 卡片 ---- */
QFrame#Card {
    background: #1F1F1F; border: 1px solid #2A2A2A;
}
QFrame#Card QPlainTextEdit, QFrame#Card QListWidget, QFrame#Card QTableWidget {
    background: #161616; border: 1px solid #2A2A2A;
}

/* ---- 输入 / 列表 / 表格 ---- */
QPlainTextEdit, QListWidget, QTableWidget, QLineEdit, QSpinBox {
    background: #161616; color: #E8E8E8; border: 1px solid #2A2A2A;
    padding: 4px; selection-background-color: #DA291C; selection-color: #FFFFFF;
}
QTableWidget::item, QListWidget::item { padding: 5px; }
QTableWidget::item:selected, QListWidget::item:selected {
    background: rgba(218,41,28,.30); color: #FFFFFF;
}
QHeaderView::section {
    background: #1A1A1A; color: #8A8A8A; border: none;
    border-bottom: 1px solid #2A2A2A; padding: 6px;
}
QTableCornerButton::section { background: #1A1A1A; border: none; }

/* ---- 进度条（电平表 / OTA 进度）---- */
QProgressBar {
    background: #161616; border: 1px solid #2A2A2A;
    text-align: center; color: #8A8A8A; font-size: 11px; min-height: 16px;
}
QProgressBar::chunk { background: #DA291C; }

/* ---- 滚动条 ---- */
QScrollBar:vertical { background: transparent; width: 9px; margin: 2px; }
QScrollBar::handle:vertical { background: #333333; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: #454545; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
QScrollBar:horizontal { background: transparent; height: 9px; margin: 2px; }
QScrollBar::handle:horizontal { background: #333333; min-width: 30px; }

/* ---- 其它 ---- */
QCheckBox { color: #D6D6D6; spacing: 8px; }
QCheckBox::indicator { width: 16px; height: 16px;
                       border: 1px solid #3A3A3A; background: #161616; }
QCheckBox::indicator:checked { background: #DA291C; border-color: #DA291C; }
QComboBox { background: #232323; color: #E8E8E8; border: 1px solid #2A2A2A; padding: 5px 10px; }
QComboBox QAbstractItemView { background: #1F1F1F; color: #E8E8E8; selection-background-color: #DA291C; }
QToolTip { background: #1F1F1F; color: #E8E8E8; border: 1px solid #2A2A2A; padding: 4px; }
"""
