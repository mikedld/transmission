// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <QComboBox>

class FilterBarComboBox : public QComboBox
{
    Q_OBJECT

public:
    enum
    {
        CountRole = Qt::UserRole + 1,
        CountStringRole,
        UserRole
    };

    explicit FilterBarComboBox(QWidget* parent = nullptr);
    FilterBarComboBox(FilterBarComboBox&&) = delete;
    FilterBarComboBox(FilterBarComboBox const&) = delete;
    FilterBarComboBox& operator=(FilterBarComboBox&&) = delete;
    FilterBarComboBox& operator=(FilterBarComboBox const&) = delete;

    // QWidget
    [[nodiscard]] QSize minimumSizeHint() const override;
    [[nodiscard]] QSize sizeHint() const override;

protected:
    // QWidget
    void paintEvent(QPaintEvent* e) override;

private:
    [[nodiscard]] QSize calculateSize(QSize const& text_size, QSize const& count_size) const;
};
