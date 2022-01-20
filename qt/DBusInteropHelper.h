// This file Copyright (C) 2015-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

class QObject;
class QString;
class QVariant;

class DBusInteropHelper
{
public:
    DBusInteropHelper() = default;

    bool isConnected() const;

    QVariant addMetainfo(QString const& metainfo) const;

    static void registerObject(QObject* parent);
};
