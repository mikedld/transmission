/****************************************************************************
**
** Copyright © 2009 Nokia Corporation and/or its subsidiary(-ies).
** Contact: Qt Software Information (qt-info@nokia.com)
**
** This file is part of the demonstration applications of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial Usage
** Licensees holding valid Qt Commercial licenses may use this file in
** accordance with the Qt Commercial License Agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Nokia.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain
** additional rights. These rights are described in the Nokia Qt LGPL
** Exception version 1.0, included in the file LGPL_EXCEPTION.txt in this
** package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
** If you are unsure which license is appropriate for your use, please
** contact the sales department at qt-sales@nokia.com.
** $QT_END_LICENSE$
**
****************************************************************************/

#include <QPainter>
#include <QStyle>
#include <QStyleOption>
#include <QTimer>

#if QT_CONFIG(accessibility)
#include <QAccessible>
#endif

#include "SqueezeLabel.h"

SqueezeLabel::SqueezeLabel(QString const& text, QWidget* parent)
    : QLabel{ text, parent }
{
}

SqueezeLabel::SqueezeLabel(QWidget* parent)
    : QLabel{ parent }
{
}

void SqueezeLabel::paintEvent(QPaintEvent* paint_event)
{
#if QT_CONFIG(accessibility)
    // NOTE: QLabel doesn't notify on text/cursor changes so we're checking for it when repaint is requested
    updateAccessibilityIfNeeded();
#endif

    if (hasFocus() && (textInteractionFlags() & (Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse)) != 0)
    {
        QLabel::paintEvent(paint_event);
        return;
    }

    auto painter = QPainter{ this };
    auto const fm = fontMetrics();
    auto opt = QStyleOption{};
    opt.initFrom(this);
    auto const full_text = text();
    auto const elided_text = fm.elidedText(full_text, Qt::ElideRight, width());
    style()->drawItemText(&painter, contentsRect(), alignment(), opt.palette, isEnabled(), elided_text, foregroundRole());

#ifndef QT_NO_TOOLTIP
    setToolTip(full_text != elided_text ? full_text : QString{});
#endif
}

#if QT_CONFIG(accessibility)

void SqueezeLabel::updateAccessibilityIfNeeded()
{
    // NOTE: Dispatching events asynchronously to avoid blocking the painting

    if (auto const new_text = text(); new_text != old_text_)
    {
        if (QAccessible::isActive())
        {
            QTimer::singleShot(
                0,
                this,
                [this, old_text = old_text_, new_text]()
                {
                    QAccessibleTextUpdateEvent event(this, 0, old_text, new_text);
                    event.setCursorPosition(selectionStart());
                    QAccessible::updateAccessibility(&event);
                });
        }

        old_text_ = new_text;
    }

    // NOTE: Due to QLabel implementation specifics, this block will never be entered :(
    if (auto const new_position = selectionStart(); new_position != old_position_ && !hasSelectedText())
    {
        if (QAccessible::isActive())
        {
            QTimer::singleShot(
                0,
                this,
                [this, new_position]()
                {
                    QAccessibleTextCursorEvent event(this, new_position);
                    QAccessible::updateAccessibility(&event);
                });
        }

        old_position_ = new_position;
    }
}

#endif // QT_CONFIG(accessibility)
