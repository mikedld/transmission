// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <QToolButton>

class PathButton : public QToolButton
{
    Q_OBJECT

public:
    enum Mode
    {
        DirectoryMode,
        FileMode
    };

    explicit PathButton(QWidget* parent = nullptr);
    PathButton(PathButton&&) = delete;
    PathButton(PathButton const&) = delete;
    PathButton& operator=(PathButton&&) = delete;
    PathButton& operator=(PathButton const&) = delete;

    void setMode(Mode mode);
    void setTitle(QString const& title);
    void setNameFilter(QString const& name_filter);

    void setPath(QString const& path);

    [[nodiscard]] constexpr auto const& path() const noexcept
    {
        return path_;
    }

    // QWidget
    [[nodiscard]] QSize sizeHint() const override;

signals:
    void pathChanged(QString const& path);

protected:
    // QWidget
    void paintEvent(QPaintEvent* event) override;

private slots:
    void onClicked() const;
    void onFileSelected(QString const& path);

private:
    void updateAppearance();

    [[nodiscard]] bool isDirMode() const;
    [[nodiscard]] QString effectiveTitle() const;

    QString name_filter_;
    QString path_;
    QString title_;
    Mode mode_ = DirectoryMode;
};
