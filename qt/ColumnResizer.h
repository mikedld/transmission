// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <set>

#include <QObject>
#include <QTimer>

#include <libtransmission/tr-macros.h>

class QGridLayout;

class ColumnResizer : public QObject
{
    Q_OBJECT
    TR_DISABLE_COPY_MOVE(ColumnResizer)

public:
    explicit ColumnResizer(QObject* parent = nullptr);

    void addLayout(QGridLayout* layout);

    // QObject
    bool eventFilter(QObject* object, QEvent* event) override;

public slots:
    void update() const;

private:
    void scheduleUpdate();

    QTimer timer_;
    std::set<QGridLayout*> layouts_;
};
