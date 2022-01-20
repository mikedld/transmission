// This file Copyright (C) 2015-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifdef ENABLE_COM_INTEROP
#include "ComInteropHelper.h"
#endif
#ifdef ENABLE_DBUS_INTEROP
#include "DBusInteropHelper.h"
#endif

class QString;

class InteropHelper
{
public:
    bool isConnected() const;

    bool addMetainfo(QString const& metainfo) const;

    static void initialize();
    static void registerObject(QObject* parent);

private:
#ifdef ENABLE_DBUS_INTEROP
    DBusInteropHelper dbus_client_ = {};
#endif
#ifdef ENABLE_COM_INTEROP
    ComInteropHelper com_client_ = {};
#endif
};
