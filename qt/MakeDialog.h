// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <optional>

#include <libtransmission/makemeta.h>

#include "BaseDialog.h"
#include "ui_MakeDialog.h"

class QAbstractButton;
class Session;

class MakeDialog : public BaseDialog
{
    Q_OBJECT

public:
    MakeDialog(Session&, QWidget* parent = nullptr);
    MakeDialog(MakeDialog&&) = delete;
    MakeDialog(MakeDialog const&) = delete;
    MakeDialog& operator=(MakeDialog&&) = delete;
    MakeDialog& operator=(MakeDialog const&) = delete;

protected:
    // QWidget
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;

private slots:
    void onSourceChanged();
    void makeTorrent();
    void onPieceSizeUpdated(int);

private:
    [[nodiscard]] QString getSource() const;
    void updatePiecesLabel();
    Session& session_;

    Ui::MakeDialog ui_ = {};

    std::optional<tr_metainfo_builder> builder_;
};
