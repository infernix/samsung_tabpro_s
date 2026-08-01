/*
    Samsung TabPro S OLED brightness effect for KWin
    SPDX-License-Identifier: GPL-2.0-or-later

    Approach: capture the rendered screen into an offscreen texture, then
    redraw it through a fragment shader that multiplies RGB by a brightness
    factor (read from /sys/class/backlight/intel_backlight/brightness).

    Pattern adapted from src/plugins/screentransform/screentransform.cpp
    (crossfade via offscreen FBO), simplified to a single-pass brightness
    multiplier.

    The intel_backlight PWM register is a no-op on this OLED panel; the panel
    also does not expose DPCD AUX brightness (DPCD 0x220 caps = 0x04). KWin
    does not expose mutter-style SetCrtcGamma, and its own kscreen
    "dimming"/"brightness" output properties are metadata-only on non-HDR
    outputs (verified on kernel 7.1.4 / KWin 6.7.3). A compositor-level
    shader is the only remaining software dimming path on KDE.
*/
#include "tabprobright.h"

#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "opengl/glframebuffer.h"
#include "opengl/glshader.h"
#include "opengl/glshadermanager.h"
#include "opengl/gltexture.h"
#include "opengl/glutils.h"
#include "opengl/glvertexbuffer.h"

#include <QFile>
#include <QSocketNotifier>

#include <fcntl.h>
#include <sys/inotify.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(KWIN_TABPROBRIGHT, "kwin_effect_tabprobright", QtWarningMsg)

static void ensureResources()
{
    Q_INIT_RESOURCE(tabprobright);
}

namespace KWin
{

TabProBrightEffect::TabProBrightEffect()
{
    ensureResources();

    // Read max brightness once.
    QFile maxFile(QStringLiteral("/sys/class/backlight/intel_backlight/max_brightness"));
    if (maxFile.open(QIODevice::ReadOnly)) {
        m_backlightMax = maxFile.readAll().trimmed().toInt();
        if (m_backlightMax <= 0) {
            m_backlightMax = 1;
        }
    }

    // Open the brightness sysfs node for inotify.
    m_backlightFd = ::open("/sys/class/backlight/intel_backlight/brightness", O_RDONLY | O_NONBLOCK);
    if (m_backlightFd < 0) {
        qCWarning(KWIN_TABPROBRIGHT) << "could not open intel_backlight/brightness";
    }

    // Set up inotify watch for the brightness file.
    int infd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (infd < 0) {
        qCWarning(KWIN_TABPROBRIGHT) << "inotify_init1 failed";
    } else {
        int wd = ::inotify_add_watch(infd, "/sys/class/backlight/intel_backlight/brightness", IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB);
        if (wd < 0) {
            qCWarning(KWIN_TABPROBRIGHT) << "inotify_add_watch failed";
            ::close(infd);
        } else {
            m_notifier = new QSocketNotifier(infd, QSocketNotifier::Read, this);
            connect(m_notifier, &QSocketNotifier::activated, this, [this]() {
                // drain inotify events
                char buf[4096];
                while (::read(m_notifier->socket(), buf, sizeof(buf)) > 0) {
                    // ignore contents
                }
                readBrightness();
            });
        }
    }

    readBrightness();
    // Pre-load the shader so isActive() can return true on the first frame;
    // otherwise kwin skips our paintScreen and the shader never loads.
    ensureShader();
    qCInfo(KWIN_TABPROBRIGHT) << "started, max=" << m_backlightMax
                              << "brightness=" << m_brightness
                              << "shader=" << (m_shader ? "ok" : "FAILED");
}

TabProBrightEffect::~TabProBrightEffect()
{
    if (m_backlightFd >= 0) {
        ::close(m_backlightFd);
    }
}

bool TabProBrightEffect::supported()
{
    return effects->compositingType() == OpenGLCompositing;
}

void TabProBrightEffect::readBrightness()
{
    if (m_backlightFd < 0) {
        return;
    }
    ::lseek(m_backlightFd, 0, SEEK_SET);
    char buf[32];
    ssize_t n = ::read(m_backlightFd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        return;
    }
    buf[n] = 0;
    bool ok = false;
    int level = QByteArray(buf, n).trimmed().toInt(&ok);
    if (!ok) {
        return;
    }
    qreal b = qreal(level) / qreal(m_backlightMax);
    if (b < 0.0) b = 0.0;
    if (b > 1.0) b = 1.0;
    // Perceptual curve: slider fraction f -> brightness f^1.6 (finer low-end control).
    b = std::pow(b, 1.6);
    if (qAbs(b - m_brightness) > 0.001) {
        m_brightness = b;
        effects->addRepaintFull();
    }
}

void TabProBrightEffect::ensureShader()
{
    if (m_shader) {
        return;
    }
    m_shader = ShaderManager::instance()->generateShaderFromFile(
        ShaderTrait::MapTexture,
        QString(),
        QStringLiteral(":/effects/tabprobright/shaders/brightness.frag"));
    if (!m_shader) {
        qCCritical(KWIN_TABPROBRIGHT) << "Failed to load the brightness shader.";
        return;
    }
    m_modelViewProjectionLocation = m_shader->uniformLocation("modelViewProjectionMatrix");
    m_samplerLocation = m_shader->uniformLocation("sampler");
    m_brightnessLocation = m_shader->uniformLocation("brightness");
}

void TabProBrightEffect::prePaintScreen(ScreenPrePaintData &data)
{
    // Re-read in case inotify missed something.
    readBrightness();
    effects->prePaintScreen(data);
    // Force a repaint as long as we are dimming so the shader runs every frame.
    if (m_brightness < 0.999) {
        data.paint = Region(); // infinite region -> full repaint
    }
}

void TabProBrightEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const Region &deviceRegion, LogicalOutput *screen)
{
    if (m_brightness >= 0.999) {
        // No dimming needed - let kwin paint normally.
        effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen);
        return;
    }

    ensureShader();
    if (!m_shader) {
        effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen);
        return;
    }

    // Render the screen into an offscreen texture.
    const QSize nativeSize = screen->geometry().size() * screen->scale();
    if (!m_offscreen.texture || m_offscreen.texture->size() != nativeSize
        || m_offscreen.texture->internalFormat() != renderTarget.texture()->internalFormat()) {
        m_offscreen.texture = GLTexture::allocate(renderTarget.texture()->internalFormat(), nativeSize);
        if (!m_offscreen.texture) {
            qCWarning(KWIN_TABPROBRIGHT) << "could not allocate offscreen texture";
            effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen);
            return;
        }
        m_offscreen.framebuffer = std::make_unique<GLFramebuffer>(m_offscreen.texture.get());
    }

    RenderTarget fboTarget(m_offscreen.framebuffer.get(), renderTarget.colorDescription());
    RenderViewport fboViewport(viewport.renderRect(), viewport.scale(), fboTarget, QPoint());

    GLFramebuffer::pushFramebuffer(m_offscreen.framebuffer.get());
    effects->paintScreen(fboTarget, fboViewport, mask, deviceRegion, screen);
    GLFramebuffer::popFramebuffer();

    // Draw the offscreen texture through the brightness shader.
    GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
    vbo->reset();
    vbo->setAttribLayout(std::span(GLVertexBuffer::GLVertex2DLayout), sizeof(GLVertex2D));
    auto opt = vbo->map<GLVertex2D>(6);
    if (!opt) {
        return;
    }
    auto map = *opt;
    const auto geom = screen->geometryF();
    const qreal scale = viewport.scale();
    const auto dg = geom.scaled(scale);
    // first triangle
    map[0] = GLVertex2D{QVector2D(dg.left(), dg.top()),    QVector2D(0.0, 1.0)};
    map[1] = GLVertex2D{QVector2D(dg.right(), dg.bottom()), QVector2D(1.0, 0.0)};
    map[2] = GLVertex2D{QVector2D(dg.left(), dg.bottom()), QVector2D(0.0, 0.0)};
    // second triangle
    map[3] = GLVertex2D{QVector2D(dg.left(), dg.top()),    QVector2D(0.0, 1.0)};
    map[4] = GLVertex2D{QVector2D(dg.right(), dg.top()),    QVector2D(1.0, 1.0)};
    map[5] = GLVertex2D{QVector2D(dg.right(), dg.bottom()), QVector2D(1.0, 0.0)};
    vbo->unmap();

    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    m_offscreen.texture->bind();

    ShaderManager *sm = ShaderManager::instance();
    sm->pushShader(m_shader.get());
    m_shader->setUniform(m_modelViewProjectionLocation, viewport.projectionMatrix());
    m_shader->setUniform(m_samplerLocation, 0);
    m_shader->setUniform(m_brightnessLocation, float(m_brightness));

    vbo->bindArrays();
    vbo->draw(GL_TRIANGLES, 0, 6);
    vbo->unbindArrays();

    sm->popShader();
    m_offscreen.texture->unbind();

    effects->addRepaintFull();
}

bool TabProBrightEffect::isActive() const
{
    // kwin only calls paintScreen when isActive() is true, so we must report
    // active whenever dimming is needed — the shader is pre-loaded in the ctor.
    return m_shader && m_brightness < 0.999;
}

} // namespace KWin

#include "moc_tabprobright.cpp"