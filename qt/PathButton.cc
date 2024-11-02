// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QStyle>
#include <QStyleOptionToolButton>
#include <QStylePainter>

#include "PathButton.h"
#include "Utils.h"

PathButton::PathButton(QWidget* parent)
    : QToolButton{ parent }
{
    setSizePolicy(QSizePolicy{ QSizePolicy::Preferred, QSizePolicy::Fixed });
    setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    setText(tr("(None)")); // for minimum width

    updateAppearance();

    connect(this, &QAbstractButton::clicked, this, &PathButton::onClicked);
}

void PathButton::setMode(Mode mode)
{
    if (mode_ == mode)
    {
        return;
    }

    mode_ = mode;

    updateAppearance();
}

void PathButton::setTitle(QString const& title)
{
    title_ = title;
}

void PathButton::setNameFilter(QString const& name_filter)
{
    name_filter_ = name_filter;
}

void PathButton::setPath(QString const& path)
{
    if (path_ == path)
    {
        return;
    }

    path_ = QDir::toNativeSeparators(Utils::removeTrailingDirSeparator(path));

    updateAppearance();

    emit pathChanged(path_);
}

QSize PathButton::sizeHint() const
{
    auto const sh = QToolButton::sizeHint();
    return { qMin(sh.width(), 150), sh.height() };
}

void PathButton::paintEvent(QPaintEvent* /*event*/)
{
    auto painter = QStylePainter{ this };
    QStyleOptionToolButton option;
    initStyleOption(&option);

    QSize const fake_content_size(100, 100);
    QSize const fake_size_hint = style()->sizeFromContents(QStyle::CT_ToolButton, &option, fake_content_size, this);

    int text_width = width() - (fake_size_hint.width() - fake_content_size.width()) - iconSize().width() - 6;

    if (popupMode() == MenuButtonPopup)
    {
        text_width -= style()->pixelMetric(QStyle::PM_MenuButtonIndicator, &option, this);
    }

    if (path_.isEmpty())
    {
        option.text = tr("(None)");
    }
    else if (auto const info = QFileInfo{ path_ }; !info.fileName().isEmpty())
    {
        option.text = info.fileName();
    }
    else
    {
        option.text = path_;
    }

    option.text = fontMetrics().elidedText(option.text, Qt::ElideMiddle, text_width);

    painter.drawComplexControl(QStyle::CC_ToolButton, option);
}

void PathButton::onClicked() const
{
    auto* dialog = new QFileDialog{ window(), effectiveTitle() };
    dialog->setFileMode(isDirMode() ? QFileDialog::Directory : QFileDialog::ExistingFile);

    if (isDirMode())
    {
        dialog->setOption(QFileDialog::ShowDirsOnly);
    }

    if (!name_filter_.isEmpty())
    {
        dialog->setNameFilter(name_filter_);
    }

    if (auto const path_info = QFileInfo{ path_ }; !path_.isEmpty() && path_info.exists())
    {
        if (path_info.isDir())
        {
            dialog->setDirectory(path_info.absoluteFilePath());
        }
        else
        {
            dialog->setDirectory(path_info.absolutePath());
            dialog->selectFile(path_info.fileName());
        }
    }

    connect(dialog, &QFileDialog::fileSelected, this, &PathButton::onFileSelected);

    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

void PathButton::onFileSelected(QString const& path)
{
    if (!path.isEmpty())
    {
        setPath(path);
    }
}

void PathButton::updateAppearance()
{
    QFileInfo const path_info{ path_ };

    int const icon_size(style()->pixelMetric(QStyle::PM_SmallIconSize));
    QFileIconProvider const icon_provider;

    QIcon icon;

    if (!path_.isEmpty() && path_info.exists())
    {
        icon = icon_provider.icon(QFileInfo{ path_ });
    }

    if (icon.isNull())
    {
        icon = icon_provider.icon(isDirMode() ? QFileIconProvider::Folder : QFileIconProvider::File);
    }

    setIconSize(QSize{ icon_size, icon_size });
    setIcon(icon);
    setToolTip(path_ == text() ? QString{} : path_);

    update();
}

bool PathButton::isDirMode() const
{
    return mode_ == DirectoryMode;
}

QString PathButton::effectiveTitle() const
{
    if (!title_.isEmpty())
    {
        return title_;
    }

    return isDirMode() ? tr("Select Folder") : tr("Select File");
}
