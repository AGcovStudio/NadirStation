#pragma once
// Application-wide visual theme: neutral graphite, flat panels with hairline borders and 2 px corners,
// the wordmark's blue only for selection and focus, and green / amber / red reserved for link state. Fusion is the
// style underneath; this stylesheet covers the standard widgets and the object names the pages use.
// Widgets that paint themselves (spectrum, constellation, charts) use the same tokens, listed here
// and in nadirstation::ui::tone below.
//
//   window #1c1d20   surface #232427   raised #2b2d31   sunken/plot #141517
//   border #35373c   strong border #45484e   hover #313338
//   text #d6d8db     secondary #9a9ea5   muted #6e737a   bright #eceef0
//   accent #4a8fd0   accent fill #22384d   accent light #8cc0ee
//   ok #3fca85 / #6fd5a1   warn #f0b83c / #e9bd62   error #f0594c / #f08f7f
#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QPolygonF>
#include <QCoreApplication>
#include <QPalette>
#include <QString>
#include <QStyleFactory>

namespace nadirstation::ui {

namespace tone {
inline QColor window() { return QColor(0x1c, 0x1d, 0x20); }
inline QColor surface() { return QColor(0x23, 0x24, 0x27); }
inline QColor raised() { return QColor(0x2b, 0x2d, 0x31); }
inline QColor sunken() { return QColor(0x14, 0x15, 0x17); }
inline QColor border() { return QColor(0x35, 0x37, 0x3c); }
inline QColor text() { return QColor(0xd6, 0xd8, 0xdb); }
inline QColor secondary() { return QColor(0x9a, 0x9e, 0xa5); }
inline QColor muted() { return QColor(0x6e, 0x73, 0x7a); }
inline QColor accent() { return QColor(0x4a, 0x8f, 0xd0); }
inline QColor ok() { return QColor(0x3f, 0xca, 0x85); }
inline QColor warn() { return QColor(0xf0, 0xb8, 0x3c); }
inline QColor error() { return QColor(0xf0, 0x59, 0x4c); }
}  // namespace tone

// Interface face: Bahnschrift, the DIN-style face Windows has shipped since Windows 10 (1709), which reads
// like instrument labelling; Segoe UI and Inter are fallbacks. Numbers use the monospaced face.
inline QString sansFamilies() {
    return QStringLiteral("\"Bahnschrift\", \"Segoe UI\", \"Inter\", \"Microsoft YaHei UI\", sans-serif");
}
inline QString monoFamilies() {
    return QStringLiteral("\"Cascadia Mono\", \"JetBrains Mono\", Consolas, \"DejaVu Sans Mono\", monospace");
}

inline QString styleSheet() {
    QString qss = QStringLiteral(R"qss(
QWidget { color: #d6d8db; font-family: @SANS@; font-size: 13px; }
/* Backgrounds only where a surface is meant: plain container widgets stay transparent, so a group of
   fields inside a panel does not show up as a box of a different shade. */
QMainWindow, QDialog, QMessageBox, QStackedWidget { background: #1c1d20; }
QLabel { background: transparent; }
QToolTip { background: #2b2d31; color: #d6d8db; border: 1px solid #45484e; padding: 5px 7px; }

/* ---- Shell: navigation rail ---- */
#NavRail { background: #141517; border-right: 1px solid #2b2d31; }
#NavRail QLabel { background: transparent; }
#Wordmark { color: #eceef0; font-size: 15px; font-weight: 700; padding: 2px 0 6px 2px; }
#WordmarkSub { color: #6e737a; font-size: 11px; padding-left: 2px; }
#NavGroup { color: #6e737a; font-size: 11px; padding: 12px 0 3px 16px; }
QPushButton#NavItem { background: transparent; border: none; border-left: 2px solid transparent; border-radius: 0;
                      text-align: left; padding: 5px 12px 5px 16px; color: #9a9ea5; font-size: 12px; }
QPushButton#NavItem:hover { color: #d6d8db; background: #1f2023; }
QPushButton#NavItem:checked { color: #eceef0; background: #2b2d31; border-left: 2px solid #4a8fd0; }
#RailClock { color: #9a9ea5; font-family: @MONO@; font-size: 12px; padding: 0 14px; }
#RailFoot { color: #585c63; font-size: 11px; padding: 0 14px 12px 14px; }

/* ---- Shell: link strip ---- */
#LinkStrip { background: #141517; border-bottom: 1px solid #2b2d31; }
#LinkStrip QLabel { background: transparent; }
#ReadoutCaption { color: #6e737a; font-size: 11px; padding-right: 6px; }
#ReadoutValue { color: #d6d8db; font-family: @MONO@; font-size: 12px; }
#ReadoutValue[tone="ok"] { color: #3fca85; }
#ReadoutValue[tone="warn"] { color: #f0b83c; }
#ReadoutValue[tone="error"] { color: #f0594c; }
#ReadoutValue[tone="idle"] { color: #6e737a; }
#ReadoutValue[tone="rec"] { color: #f0594c; }
#StripDivider { background: #2b2d31; min-width: 1px; max-width: 1px; }
#TopTarget { color: #9a9ea5; font-size: 11px; }

/* ---- Pages, cards, sections ---- */
#PageTitle { color: #eceef0; font-size: 14px; font-weight: 600; }
#PageSubtitle { color: #9a9ea5; font-size: 12px; }
QFrame#Card { background: #232427; border: 1px solid #2f3135; border-radius: 2px; }
QFrame#Card > QLabel { background: transparent; }
#CardTitle { color: #c4c7cb; font-size: 12px; font-weight: 600; }
#CardHint { color: #6e737a; font-size: 11px; }
#SectionTitle { color: #c4c7cb; font-size: 12px; font-weight: 600; padding: 8px 0 2px 0; }
#FieldLabel { color: #9a9ea5; }
#Note { color: #9a9ea5; font-size: 11px; }
QFrame#Rule { background: #2f3135; max-height: 1px; min-height: 1px; border: none; }
QScrollArea#PanelScroll, QScrollArea#PanelScroll > QWidget > QWidget { background: transparent; }

/* ---- Buttons ---- */
QPushButton { background: #2b2d31; border: 1px solid #3c3f44; border-radius: 2px; padding: 5px 12px; color: #d6d8db; }
QPushButton:hover { background: #313338; border-color: #4a4d53; }
QPushButton:pressed { background: #1f2023; }
QPushButton:checked { background: #22384d; border-color: #4a8fd0; }
QPushButton:disabled { color: #585c63; background: #1f2023; border-color: #2f3135; }
QPushButton:focus { border-color: #4a8fd0; }
QPushButton#Primary { background: #2c6a45; border: 1px solid #357d52; color: #f2f7f3; font-weight: 600; font-size: 13px; }
QPushButton#Primary:hover { background: #327a4f; border-color: #3f9161; }
QPushButton#Primary:pressed { background: #25593a; }
QPushButton#Primary[running="true"] { background: #7a2e2b; color: #fbeeed; border: 1px solid #93403b; }
QPushButton#Primary[running="true"]:hover { background: #883632; }
QPushButton#Primary:disabled { background: #1f2023; color: #585c63; border-color: #2f3135; }
QPushButton#Secondary { font-size: 13px; }
QPushButton#Secondary[running="true"] { background: #3a2423; color: #f3c7c2; border: 1px solid #7a3a36; }
QPushButton#Secondary[running="true"]:hover { background: #452a28; }
QPushButton#RecordButton { text-align: left; padding-left: 12px; }
QPushButton#RecordButton:checked { background: #2e1c1c; border-color: #f0594c; color: #f08f7f; }
QPushButton#Quiet { background: transparent; border: 1px solid transparent; color: #9a9ea5; padding: 4px 8px; }
QPushButton#Quiet:hover { color: #d6d8db; border-color: #35373c; }

/* ---- Inputs ---- */
QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox { background: #141517; border: 1px solid #35373c; border-radius: 2px; padding: 4px 7px; min-height: 18px; selection-background-color: #22384d; }
QLineEdit:hover, QComboBox:hover, QSpinBox:hover, QDoubleSpinBox:hover { border-color: #45484e; }
QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus { border-color: #4a8fd0; }
QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled { color: #585c63; background: #1c1d20; border-color: #2b2d31; }
QDoubleSpinBox#BigNumber { font-family: @MONO@; font-size: 13px; color: #eceef0; }
QComboBox { padding-right: 24px; }
QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: top right; width: 20px; border: none; }
QComboBox::down-arrow { image: url("@ARROW_DOWN@"); width: 8px; height: 8px; }
QComboBox::down-arrow:on { image: url("@ARROW_UP@"); }
QComboBox#SatellitePicker { background: #2b2d31; border: 1px solid #45484e; font-size: 12px; font-weight: 600; padding: 4px 26px 4px 8px; color: #eceef0; }
QComboBox#SatellitePicker:hover { border-color: #5a5e65; }
QAbstractSpinBox { padding-right: 20px; }
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button { subcontrol-origin: border; width: 16px; border: none; background: transparent; }
QAbstractSpinBox::up-button { subcontrol-position: top right; }
QAbstractSpinBox::down-button { subcontrol-position: bottom right; }
QAbstractSpinBox::up-button:hover, QAbstractSpinBox::down-button:hover { background: #2b2d31; }
QAbstractSpinBox::up-arrow { image: url("@ARROW_UP@"); width: 7px; height: 7px; }
QAbstractSpinBox::down-arrow { image: url("@ARROW_DOWN@"); width: 7px; height: 7px; }
QComboBox QAbstractItemView { background: #232427; border: 1px solid #45484e; selection-background-color: #22384d; outline: 0; padding: 2px; }
QPlainTextEdit, QTextEdit { background: #141517; border: 1px solid #35373c; border-radius: 2px; selection-background-color: #22384d; }
QPlainTextEdit#LogView { color: #9a9ea5; font-family: @MONO@; font-size: 11px; padding: 6px; }

/* ---- Lists, trees, tables ---- */
QListWidget, QTreeWidget, QTableWidget, QTableView, QTreeView, QListView { background: #141517; border: 1px solid #2f3135; border-radius: 2px; alternate-background-color: #18191b; gridline-color: #26272a; outline: 0; }
QListWidget::item { padding: 5px 6px; border-bottom: 1px solid #1f2023; }
QListWidget::item:hover, QTreeWidget::item:hover { background: #1f2023; }
QListWidget::item:selected, QTreeWidget::item:selected, QTableWidget::item:selected, QTableView::item:selected { background: #22384d; color: #eceef0; }
QHeaderView { background: transparent; }
QHeaderView::section { background: #232427; color: #9a9ea5; border: 0; border-right: 1px solid #2f3135; border-bottom: 1px solid #2f3135; padding: 5px 7px; font-weight: 600; }

/* ---- Tabs inside pages ---- */
QTabWidget::pane { border: 1px solid #2f3135; border-radius: 2px; top: -1px; background: #1c1d20; }
QTabBar { background: transparent; }
QTabBar::tab { background: transparent; color: #9a9ea5; border: none; border-bottom: 2px solid transparent; padding: 5px 12px; margin-right: 0; }
QTabBar::tab:selected { color: #eceef0; border-bottom: 2px solid #4a8fd0; }
QTabBar::tab:hover:!selected { color: #d6d8db; }

/* ---- Group boxes (kept for pages that still use them) ---- */
QGroupBox { border: 1px solid #2f3135; border-radius: 2px; margin-top: 16px; padding-top: 8px; background: #232427; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #9a9ea5; font-weight: 600; }

/* ---- Scroll bars, misc ---- */
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: #35373c; min-height: 28px; border-radius: 1px; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:horizontal { background: #35373c; min-width: 28px; border-radius: 1px; }
QScrollBar::handle:hover { background: #45484e; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QCheckBox, QRadioButton { spacing: 7px; background: transparent; }
QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid #45484e; border-radius: 1px; background: #141517; }
QCheckBox::indicator:checked { background: #4a8fd0; border-color: #4a8fd0; image: url("@CHECK@"); }
QCheckBox::indicator:disabled { border-color: #2b2d31; }
QSlider::groove:horizontal { height: 4px; background: #35373c; border-radius: 2px; }
QSlider::handle:horizontal { width: 12px; margin: -5px 0; background: #9a9ea5; border-radius: 2px; }
QProgressBar { background: #141517; border: 1px solid #35373c; border-radius: 2px; text-align: center; }
QProgressBar::chunk { background: #4a8fd0; border-radius: 1px; }
QSplitter::handle { background: #2b2d31; }
QStatusBar { background: #141517; color: #9a9ea5; border-top: 1px solid #2b2d31; }
QStatusBar::item { border: none; }
QMenuBar { background: #141517; color: #c4c7cb; border-bottom: 1px solid #2b2d31; padding: 1px 4px; }
QMenuBar::item { background: transparent; padding: 4px 10px; }
QMenuBar::item:selected { background: #2b2d31; color: #eceef0; }
QMenu { background: #232427; border: 1px solid #45484e; padding: 3px; }
QMenu::separator { height: 1px; background: #35373c; margin: 4px 8px; }
QMenu::item { padding: 5px 16px; }
QMenu::item:selected { background: #22384d; }

/* ---- Live page ---- */
#CenterStage { background: #232427; border: 1px solid #2f3135; border-radius: 2px; }
#BigSatellite { font-size: 28px; font-weight: 600; color: #4a8fd0; }
#DecodeBadge { background: #141517; border: 1px solid #2f3135; border-left: 3px solid #4a8fd0; border-radius: 0; padding: 6px 10px; color: #d6d8db; font-family: @MONO@; font-size: 11px; }
#DecodeBadge[sync="green"] { border-left: 3px solid #3fca85; }
#DecodeBadge[sync="yellow"] { border-left: 3px solid #f0b83c; }
#DecodeBadge[sync="red"] { border-left: 3px solid #f0594c; }
#DecodeBadge[sync="idle"] { border-left: 3px solid #4a4d53; }
#MetricTile { background: transparent; border: none; border-left: 1px solid #35373c; }
#MetricCaption { color: #6e737a; font-size: 11px; }
#DataValue { color: #e2e4e7; font-family: @MONO@; font-size: 12px; }
#MetricValue { color: #eceef0; font-family: @MONO@; font-size: 14px; }
#ConnState { font-size: 12px; padding: 2px 0; }
#ConnState[tone="ok"] { color: #3fca85; }
#ConnState[tone="warn"] { color: #e9bd62; }
#ConnState[tone="idle"] { color: #9a9ea5; }
#Preview { background: #141517; border: 1px solid #2f3135; border-radius: 2px; color: #6e737a; }

/* ---- System page ---- */
#SystemCard { background: #232427; border: 1px solid #2f3135; border-radius: 2px; }
#SystemCard QLabel { background: transparent; }
#SystemTitle { font-size: 14px; font-weight: 600; color: #eceef0; }
#SystemEyebrow { color: #9a9ea5; font-size: 11px; font-weight: 600; }
#SystemValue { color: #eceef0; font-size: 20px; font-weight: 600; font-family: @MONO@; }
#SystemValueSmall { color: #6fd5a1; font-size: 15px; font-weight: 600; }
#SystemDetail { color: #9a9ea5; font-size: 12px; }
#SystemConfig { color: #d6d8db; font-size: 12px; padding-top: 6px; }
#SystemLive { color: #6fd5a1; background: transparent; border: 1px solid #2a4636; border-radius: 2px; padding: 3px 8px; font-size: 11px; font-weight: 600; }
)qss");
    qss.replace(QStringLiteral("@SANS@"), sansFamilies());
    qss.replace(QStringLiteral("@MONO@"), monoFamilies());
    return qss;
}

// Stylesheets cannot draw glyphs themselves, so the arrows and the check mark are rendered at
// start-up into <exe>/resources/ui and referenced by absolute path.
inline QString glyphImage(const QString& name, int kind) {
    const QString dir = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("resources/ui"));
    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath(name);
    QImage img(32, 32, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    if (kind == 2) {
        QPen pen(QColor(0xff, 0xff, 0xff), 4.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.drawPolyline(QPolygonF() << QPointF(7, 17) << QPointF(13, 23) << QPointF(25, 9));
    } else {
        QPen pen(tone::secondary(), 3.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        if (kind == 1) p.drawPolyline(QPolygonF() << QPointF(7, 20) << QPointF(16, 11) << QPointF(25, 20));
        else p.drawPolyline(QPolygonF() << QPointF(7, 12) << QPointF(16, 21) << QPointF(25, 12));
    }
    p.end();
    img.save(path);
    return QDir::fromNativeSeparators(path);
}

inline void apply(QApplication& app) {
    if (auto* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) app.setStyle(fusion);
    QPalette p;
    p.setColor(QPalette::Window, tone::window());
    p.setColor(QPalette::WindowText, tone::text());
    p.setColor(QPalette::Base, tone::sunken());
    p.setColor(QPalette::AlternateBase, QColor(0x18, 0x19, 0x1b));
    p.setColor(QPalette::Text, tone::text());
    p.setColor(QPalette::Button, tone::raised());
    p.setColor(QPalette::ButtonText, tone::text());
    p.setColor(QPalette::Highlight, QColor(0x22, 0x38, 0x4d));
    p.setColor(QPalette::HighlightedText, QColor(0xec, 0xee, 0xf0));
    p.setColor(QPalette::ToolTipBase, tone::raised());
    p.setColor(QPalette::ToolTipText, tone::text());
    p.setColor(QPalette::PlaceholderText, tone::muted());
    p.setColor(QPalette::Link, QColor(0x8c, 0xc0, 0xee));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x58, 0x5c, 0x63));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x58, 0x5c, 0x63));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x58, 0x5c, 0x63));
    app.setPalette(p);
    // Widgets that paint their own text use the application font, so set it to the same face.
    QFont uiFont(QStringLiteral("Bahnschrift"), 10);
    uiFont.setFamilies({QStringLiteral("Bahnschrift"), QStringLiteral("Segoe UI"), QStringLiteral("Inter"), QStringLiteral("Microsoft YaHei UI")});
    app.setFont(uiFont);
    QString qss = styleSheet();
    qss.replace(QStringLiteral("@ARROW_DOWN@"), glyphImage(QStringLiteral("chevron_down.png"), 0));
    qss.replace(QStringLiteral("@ARROW_UP@"), glyphImage(QStringLiteral("chevron_up.png"), 1));
    qss.replace(QStringLiteral("@CHECK@"), glyphImage(QStringLiteral("check.png"), 2));
    app.setStyleSheet(qss);
}

}  // namespace nadirstation::ui
