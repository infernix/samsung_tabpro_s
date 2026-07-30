#pragma once
/*
    Samsung TabPro S OLED brightness effect for KWin
    SPDX-License-Identifier: GPL-2.0-or-later

    Dims the whole screen via a brightness-multiplier shader, driven by
    /sys/class/backlight/intel_backlight/brightness. The OLED panel on this
    tablet ignores the i915 PWM backlight, so this effect is the only way
    to dim the panel under KDE/KWin (which does not expose mutter SetCrtcGamma
    and applies its own "dimming" field only on HDR-capable outputs).
*/
#include "effect/effect.h"

#include <QSocketNotifier>

namespace KWin
{

class GLShader;
struct GLFramebuffer;
struct GLTexture;

class TabProBrightEffect : public Effect
{
    Q_OBJECT

public:
    TabProBrightEffect();
    ~TabProBrightEffect() override;

    void prePaintScreen(ScreenPrePaintData &data) override;
    void paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const Region &deviceRegion, LogicalOutput *screen) override;

    bool isActive() const override;
    int requestedEffectChainPosition() const override { return 99; }
    static bool supported();

private:
    void readBrightness();
    void ensureShader();

    struct Snapshot
    {
        std::shared_ptr<GLTexture> texture;
        std::shared_ptr<GLFramebuffer> framebuffer;
    };

    std::unique_ptr<GLShader> m_shader;
    int m_brightnessLocation = -1;
    int m_modelViewProjectionLocation = -1;
    int m_samplerLocation = -1;

    Snapshot m_offscreen;

    qreal m_brightness = 1.0;     // 0..1, applied multiplier
    int m_backlightMax = 1;
    int m_backlightFd = -1;
    QSocketNotifier *m_notifier = nullptr;
};

} // namespace KWin