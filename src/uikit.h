#pragma once
// Small builders for the layout vocabulary the pages share: cards, labelled sections, form rows,
// read-outs. The look lives in uitheme.h (object names); these only assemble widgets, so a page
// reads as a list of what it contains rather than as layout plumbing.
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QStyle>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>

namespace nadirstation::ui {

inline QLabel* label(const QString& text, const char* objectName) {
    auto* l = new QLabel(text);
    l->setObjectName(QLatin1String(objectName));
    return l;
}

// Re-applies the stylesheet after a dynamic property used in a selector changed.
inline void setTone(QWidget* w, const char* property, const QString& value) {
    if (!w || w->property(property).toString() == value) return;
    w->setProperty(property, value);
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

// A surface with an optional title row (title on the left, `trailing` on the right) and a body.
struct Card {
    QFrame* frame = nullptr;
    QVBoxLayout* body = nullptr;
    QHBoxLayout* header = nullptr;
};

inline Card card(const QString& title = QString(), QWidget* trailing = nullptr, const QString& hint = QString()) {
    Card c;
    c.frame = new QFrame;
    c.frame->setObjectName(QStringLiteral("Card"));
    c.body = new QVBoxLayout(c.frame);
    c.body->setContentsMargins(10, 8, 10, 10);
    c.body->setSpacing(6);
    if (!title.isEmpty() || trailing) {
        c.header = new QHBoxLayout;
        c.header->setSpacing(8);
        if (!title.isEmpty()) c.header->addWidget(label(title, "CardTitle"));
        if (!hint.isEmpty()) c.header->addWidget(label(hint, "CardHint"));
        c.header->addStretch(1);
        if (trailing) c.header->addWidget(trailing);
        c.body->addLayout(c.header);
    }
    return c;
}

// A thin horizontal rule between groups inside one card or panel.
inline QFrame* rule() {
    auto* f = new QFrame;
    f->setObjectName(QStringLiteral("Rule"));
    f->setFrameShape(QFrame::NoFrame);
    return f;
}

// Two-column form inside a section: muted label on the left, field on the right.
class Form {
public:
    explicit Form(QWidget* parent = nullptr) : grid_(new QGridLayout(parent)) {
        grid_->setContentsMargins(0, 0, 0, 0);
        grid_->setHorizontalSpacing(12);
        grid_->setVerticalSpacing(7);
        grid_->setColumnStretch(1, 1);
    }
    QGridLayout* layout() const { return grid_; }
    QLabel* row(const QString& name, QWidget* field) {
        QLabel* l = nullptr;
        if (!name.isEmpty()) {
            l = label(name, "FieldLabel");
            grid_->addWidget(l, row_, 0, Qt::AlignVCenter);
            grid_->addWidget(field, row_++, 1);
        } else {
            grid_->addWidget(field, row_++, 0, 1, 2);
        }
        return l;
    }
private:
    QGridLayout* grid_;
    int row_ = 0;
};

// A vertical scroll area for a side panel that may outgrow a short window.
inline QScrollArea* panelScroll(QWidget* content) {
    auto* s = new QScrollArea;
    s->setObjectName(QStringLiteral("PanelScroll"));
    s->setWidget(content);
    s->setWidgetResizable(true);
    s->setFrameShape(QFrame::NoFrame);
    s->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    s->viewport()->setAutoFillBackground(false);
    content->setAutoFillBackground(false);
    return s;
}

// The page heading every content page opens with.
inline QWidget* pageHeader(const QString& title, const QString& subtitle, QWidget* trailing = nullptr) {
    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(2, 0, 2, 4);
    auto* text = new QVBoxLayout;
    text->setSpacing(2);
    text->addWidget(label(title, "PageTitle"));
    if (!subtitle.isEmpty()) text->addWidget(label(subtitle, "PageSubtitle"));
    h->addLayout(text, 1);
    if (trailing) h->addWidget(trailing, 0, Qt::AlignBottom);
    return w;
}

}  // namespace nadirstation::ui
