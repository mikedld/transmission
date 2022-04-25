// This file Copyright © 2007-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

/**
 * @addtogroup port_forwarding Port Forwarding
 * @{
 */

#include "transmission.h"

#include "net.h" // tr_port

struct tr_upnp;

tr_upnp* tr_upnpInit(void);

void tr_upnpClose(tr_upnp*);

tr_port_forwarding tr_upnpPulse(tr_upnp*, tr_port port, bool isEnabled, bool doPortCheck, char const*);

/* @} */
