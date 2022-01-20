// This file Copyright (C) 2009-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <QTimer>

#include <libtransmission/tr-macros.h>

#include "BaseDialog.h"
#include "ui_StatsDialog.h"

class Session;

class StatsDialog : public BaseDialog
{
    Q_OBJECT
    TR_DISABLE_COPY_MOVE(StatsDialog)

public:
    explicit StatsDialog(Session&, QWidget* parent = nullptr);

    // QWidget
    void setVisible(bool visible) override;

private slots:
    void updateStats();

private:
    Session& session_;

    Ui::StatsDialog ui_ = {};

    QTimer timer_;
};
