// This file Copyright © 2013-2023 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <QDir>

#include <libtransmission/transmission.h>
#include <libtransmission/variant.h>

#include "Formatter.h"
#include "FreeSpaceLabel.h"
#include "RpcQueue.h"
#include "Session.h"
#include "VariantHelpers.h"

using ::trqt::variant_helpers::dictAdd;
using ::trqt::variant_helpers::dictFind;

namespace
{

int const IntervalMSec = 15000;

} // namespace

FreeSpaceLabel::FreeSpaceLabel(QWidget* parent)
    : QLabel(parent)
    , timer_(this)
{
    timer_.setSingleShot(true);
    timer_.setInterval(IntervalMSec);

    connect(&timer_, &QTimer::timeout, this, &FreeSpaceLabel::onTimer);
}

void FreeSpaceLabel::setSession(Session& session)
{
    if (session_ == &session)
    {
        return;
    }

    session_ = &session;
    onTimer();
}

void FreeSpaceLabel::setPath(QString const& path)
{
    if (path_ != path)
    {
        setText(tr("<i>Calculating Free Space…</i>"));
        path_ = path;
        onTimer();
    }
}

void FreeSpaceLabel::onTimer()
{
    timer_.stop();

    if (session_ == nullptr || path_.isEmpty())
    {
        return;
    }

    auto args = tr_variant::make_map(1U);
    dictAdd(&args, TR_KEY_path, path_);

    auto* q = new RpcQueue{ this };

    q->add([this, &args]() { return session_->exec("free-space", &args); });

    q->add(
        [this](RpcResponse const& r)
        {
            // update the label
            if (auto const bytes = dictFind<int64_t>(r.args.get(), TR_KEY_size_bytes); bytes && *bytes > 1)
            {
                setText(tr("%1 free").arg(Formatter::get().sizeToString(*bytes)));
            }
            else
            {
                setText(QString{});
            }

            // update the tooltip
            auto const path = dictFind<QString>(r.args.get(), TR_KEY_path);
            setToolTip(QDir::toNativeSeparators(path.value_or(QString{})));

            timer_.start();
        });

    q->run();
}
