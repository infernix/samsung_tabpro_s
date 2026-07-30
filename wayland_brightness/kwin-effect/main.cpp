/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "tabprobright.h"

namespace KWin
{

KWIN_EFFECT_FACTORY_SUPPORTED(TabProBrightEffect,
                              "metadata.json",
                              return TabProBrightEffect::supported();)

} // namespace KWin

#include "main.moc"