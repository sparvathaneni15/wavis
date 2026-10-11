#include "VisualView.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace
{
constexpr int kHistory = 2048;

int triggerIndex(const std::vector<float>& samples, float threshold)
{
    const int limit = static_cast<int>(samples.size());
    for (int i = 1; i < limit; ++i)
        if (samples[static_cast<size_t>(i - 1)] < threshold && samples[static_cast<size_t>(i)] >= threshold)
            return i;
    return 0;
}

void logBars(const std::vector<float>& mags, int count, std::vector<float>& bars)
{
    bars.assign(static_cast<size_t>(count), 0.0f);
    const int n = static_cast<int>(mags.size());
    if (n <= 1 || count <= 0)
        return;
    std::vector<int> edges(static_cast<size_t>(count + 1), 0);
    edges.back() = n;
    for (int i = 1; i < count; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(count);
        const float curved = (std::pow(static_cast<float>(n), t) - 1.0f) / static_cast<float>(n - 1) * static_cast<float>(n);
        edges[static_cast<size_t>(i)] = std::clamp(static_cast<int>(std::round(curved)), edges[static_cast<size_t>(i - 1)] + 1, n);
    }
    edges.back() = n;
    for (int i = 0; i < count; ++i)
    {
        const int from = std::min(edges[static_cast<size_t>(i)], n - 1);
        const int to = std::max(from + 1, std::min(n, edges[static_cast<size_t>(i + 1)]));
        float peak = 0.0f;
        for (int j = from; j < to; ++j)
            peak = std::max(peak, mags[static_cast<size_t>(j)]);
        bars[static_cast<size_t>(i)] = peak;
    }
}

void addVert(std::vector<float>& v, float x, float y, float r, float g, float b, float a)
{
    v.insert(v.end(), { x, y, r, g, b, a });
}

void addQuad(std::vector<float>& v, float x0, float y0, float x1, float y1, float r, float g, float b, float a)
{
    addVert(v, x0, y0, r, g, b, a);
    addVert(v, x1, y0, r, g, b, a);
    addVert(v, x1, y1, r, g, b, a);
    addVert(v, x0, y0, r, g, b, a);
    addVert(v, x1, y1, r, g, b, a);
    addVert(v, x0, y1, r, g, b, a);
}
} // namespace

VisualView::VisualView(AudioEngine& engine) : audio(engine)
{
    setOpaque(true);
    history.assign(kHistory, 0.0f);
    fft = std::make_unique<juce::dsp::FFT>(11);
    fftBuffer.assign(static_cast<size_t>(fft->getSize() * 2), 0.0f);
    window.resize(static_cast<size_t>(fft->getSize()));
    for (size_t i = 0; i < window.size(); ++i)
        window[i] = 0.5f * (1.0f - std::cos(6.2831853f * static_cast<float>(i) / static_cast<float>(window.size())));

    context.setRenderer(this);
    context.setContinuousRepainting(true);
    context.setComponentPaintingEnabled(false);
    context.setSwapInterval(1);
    context.attachTo(*this);
}

VisualView::~VisualView()
{
    context.detach();
}

void VisualView::setMode(const std::string& next)
{
    mode = next == "spectrum" || next == "radial" ? next : "oscilloscope";
    peaks.clear();
}

std::string VisualView::getMode() const { return mode; }

void VisualView::requestCapture(const juce::File& file)
{
    captureFile = file;
    capturePending.store(true);
}

void VisualView::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);
}

void VisualView::newOpenGLContextCreated()
{
    using namespace juce::gl;
    std::cerr << "Wavis OpenGL " << reinterpret_cast<const char*>(glGetString(GL_VERSION)) << "\n";
    program.reset();
    if (vertexArray == 0)
        glGenVertexArrays(1, &vertexArray);
    ensureProgram();
}

void VisualView::openGLContextClosing()
{
    using namespace juce::gl;
    if (vertexArray != 0)
    {
        glDeleteVertexArrays(1, &vertexArray);
        vertexArray = 0;
    }
    program.reset();
}

bool VisualView::ensureProgram()
{
    if (program != nullptr)
        return true;

    program = std::make_unique<juce::OpenGLShaderProgram>(context);
    const char* vertex = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColour;
out vec4 vColour;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); vColour = aColour; }
)";
    const char* fragment = R"(#version 330 core
in vec4 vColour;
out vec4 fragColour;
void main() { fragColour = vColour; }
)";

    if (!program->addVertexShader(vertex) || !program->addFragmentShader(fragment) || !program->link())
    {
        std::cerr << "Wavis shader error: " << program->getLastError().toStdString() << "\n";
        program.reset();
        return false;
    }
    return true;
}

void VisualView::renderOpenGL()
{
    using namespace juce::gl;
    const float scale = static_cast<float>(context.getRenderingScale());
    const int width = std::max(1, juce::roundToInt(getWidth() * scale));
    const int height = std::max(1, juce::roundToInt(getHeight() * scale));
    glViewport(0, 0, width, height);

    float incoming[512];
    const int got = audio.popVisual(incoming, 512);
    if (got > 0)
    {
        if (static_cast<int>(history.size()) > kHistory)
            history.erase(history.begin(), history.begin() + (static_cast<int>(history.size()) - kHistory));
        history.insert(history.end(), incoming, incoming + got);
        if (static_cast<int>(history.size()) > kHistory)
            history.erase(history.begin(), history.begin() + (static_cast<int>(history.size()) - kHistory));
    }

    if (mode == "spectrum")
        drawSpectrum();
    else if (mode == "radial")
        drawRadial();
    else
        drawOscilloscope();

    const double now = juce::Time::getMillisecondCounterHiRes();
    ++frameCounter;
    if (fpsStamp <= 0.0)
        fpsStamp = now;
    if (now - fpsStamp >= 500.0)
    {
        fps.store(juce::roundToInt(frameCounter * 1000.0 / (now - fpsStamp)));
        frameCounter = 0;
        fpsStamp = now;
    }

    captureIfRequested(width, height);
}

void VisualView::drawVertices(GLenum drawMode, const std::vector<float>& verts)
{
    using namespace juce::gl;
    if (!ensureProgram() || verts.size() < 6)
        return;
    program->use();
    if (vertexArray != 0)
        glBindVertexArray(vertexArray);
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)), verts.data(), GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
    glDrawArrays(drawMode, 0, static_cast<GLsizei>(verts.size() / 6));
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteBuffers(1, &buffer);
    if (vertexArray != 0)
        glBindVertexArray(0);
}

void VisualView::drawOscilloscope()
{
    using namespace juce::gl;
    glClearColor(0.02f, 0.015f, 0.04f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    vertices.clear();
    addQuad(vertices, -1.0f, -1.0f, 1.0f, 1.0f, 0.02f, 0.015f, 0.045f, 1.0f);
    const int n = static_cast<int>(history.size());
    const int windowSize = std::min(n, 1024);
    const int newest = std::max(0, n - windowSize);
    const int triggered = triggerIndex(std::vector<float>(history.begin() + newest, history.end()), 0.05f);
    const int start = newest + std::min(triggered, std::max(0, windowSize - 256));
    const int count = std::min(256, n - start);
    if (count > 2)
    {
        for (int i = 0; i < count - 1; ++i)
        {
            const float x0 = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(count - 1);
            const float x1 = -1.0f + 2.0f * static_cast<float>(i + 1) / static_cast<float>(count - 1);
            const float y0 = std::clamp(history[static_cast<size_t>(start + i)] * 1.6f, -1.0f, 1.0f) * 0.82f;
            const float y1 = std::clamp(history[static_cast<size_t>(start + i + 1)] * 1.6f, -1.0f, 1.0f) * 0.82f;
            addVert(vertices, x0, 0.0f, 0.18f, 0.08f, 0.28f, 0.45f);
            addVert(vertices, x1, 0.0f, 0.18f, 0.08f, 0.28f, 0.45f);
            addVert(vertices, x1, y1, 0.24f, 0.94f, 1.0f, 1.0f);
            addVert(vertices, x0, 0.0f, 0.18f, 0.08f, 0.28f, 0.45f);
            addVert(vertices, x1, y1, 0.24f, 0.94f, 1.0f, 1.0f);
            addVert(vertices, x0, y0, 0.24f, 0.94f, 1.0f, 1.0f);
        }
    }
    drawVertices(GL_TRIANGLES, vertices);
}

void VisualView::drawSpectrum()
{
    using namespace juce::gl;
    glClearColor(0.03f, 0.02f, 0.06f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    const int size = fft != nullptr ? fft->getSize() : 0;
    if (size > 0 && static_cast<int>(history.size()) >= size)
    {
        std::fill(fftBuffer.begin(), fftBuffer.end(), 0.0f);
        const int offset = static_cast<int>(history.size()) - size;
        for (int i = 0; i < size; ++i)
            fftBuffer[static_cast<size_t>(i)] = history[static_cast<size_t>(offset + i)] * window[static_cast<size_t>(i)];
        fft->performRealOnlyForwardTransform(fftBuffer.data(), false);
        magnitudes.assign(static_cast<size_t>(size / 2), 0.0f);
        for (int i = 0; i < size / 2; ++i)
        {
            const float re = fftBuffer[static_cast<size_t>(2 * i)];
            const float im = fftBuffer[static_cast<size_t>(2 * i + 1)];
            magnitudes[static_cast<size_t>(i)] = std::sqrt(re * re + im * im) / static_cast<float>(size);
        }
    }

    constexpr int bars = 72;
    std::vector<float> levels;
    logBars(magnitudes, bars, levels);
    if (peaks.size() != levels.size())
        peaks.assign(levels.size(), 0.0f);
    for (size_t i = 0; i < levels.size(); ++i)
        peaks[i] = levels[i] >= peaks[i] ? levels[i] : std::max(0.0f, peaks[i] - 0.02f);

    vertices.clear();
    addQuad(vertices, -1.0f, -1.0f, 1.0f, 1.0f, 0.03f, 0.02f, 0.06f, 1.0f);
    for (int i = 0; i < bars; ++i)
    {
        const float level = i < static_cast<int>(levels.size()) ? std::clamp(levels[static_cast<size_t>(i)] * 8.0f, 0.0f, 1.0f) : 0.0f;
        const float x0 = -0.96f + 1.92f * static_cast<float>(i) / bars;
        const float x1 = x0 + 1.92f / bars * 0.72f;
        const float y0 = -0.9f;
        const float y1 = y0 + level * 1.75f;
        const float hue = static_cast<float>(i) / bars;
        addQuad(vertices, x0, y0, x1, y1, 0.35f + 0.6f * hue, 0.25f + 0.55f * (1.0f - hue), 1.0f, 1.0f);
    }
    drawVertices(GL_TRIANGLES, vertices);
}

void VisualView::drawRadial()
{
    using namespace juce::gl;
    glClearColor(0.04f, 0.02f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    rotation += 0.01f;

    const int size = fft != nullptr ? fft->getSize() : 0;
    if (size > 0 && static_cast<int>(history.size()) >= size)
    {
        std::fill(fftBuffer.begin(), fftBuffer.end(), 0.0f);
        const int offset = static_cast<int>(history.size()) - size;
        for (int i = 0; i < size; ++i)
            fftBuffer[static_cast<size_t>(i)] = history[static_cast<size_t>(offset + i)] * window[static_cast<size_t>(i)];
        fft->performRealOnlyForwardTransform(fftBuffer.data(), false);
        magnitudes.assign(static_cast<size_t>(size / 2), 0.0f);
        for (int i = 0; i < size / 2; ++i)
        {
            const float re = fftBuffer[static_cast<size_t>(2 * i)];
            const float im = fftBuffer[static_cast<size_t>(2 * i + 1)];
            magnitudes[static_cast<size_t>(i)] = std::sqrt(re * re + im * im) / static_cast<float>(size);
        }
    }

    constexpr int rays = 96;
    std::vector<float> levels;
    logBars(magnitudes, rays, levels);
    vertices.clear();
    addQuad(vertices, -1.0f, -1.0f, 1.0f, 1.0f, 0.05f, 0.02f, 0.09f, 1.0f);
    drawVertices(GL_TRIANGLES, vertices);

    vertices.clear();
    for (int i = 0; i < rays; ++i)
    {
        const float level = i < static_cast<int>(levels.size()) ? std::clamp(levels[static_cast<size_t>(i)] * 10.0f, 0.05f, 1.0f) : 0.05f;
        const float angle = rotation + 6.2831853f * static_cast<float>(i) / rays;
        const float half = 0.018f;
        const float inner = 0.18f;
        const float outer = inner + level * 0.72f;
        const float a0 = angle - half;
        const float a1 = angle + half;
        addVert(vertices, std::cos(a0) * inner, std::sin(a0) * inner, 0.55f, 0.28f, 1.0f, 0.85f);
        addVert(vertices, std::cos(a0) * outer, std::sin(a0) * outer, 0.24f, 0.94f, 1.0f, 1.0f);
        addVert(vertices, std::cos(a1) * outer, std::sin(a1) * outer, 1.0f, 0.17f, 0.84f, 1.0f);
        addVert(vertices, std::cos(a0) * inner, std::sin(a0) * inner, 0.55f, 0.28f, 1.0f, 0.85f);
        addVert(vertices, std::cos(a1) * outer, std::sin(a1) * outer, 1.0f, 0.17f, 0.84f, 1.0f);
        addVert(vertices, std::cos(a1) * inner, std::sin(a1) * inner, 0.55f, 0.28f, 1.0f, 0.85f);
    }
    drawVertices(GL_TRIANGLES, vertices);
}

void VisualView::captureIfRequested(int width, int height)
{
    if (!capturePending.exchange(false))
        return;

    using namespace juce::gl;
    std::vector<unsigned char> pixels(static_cast<size_t>(width * height * 4));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    juce::Image image(juce::Image::RGB, width, height, false);
    double sum = 0.0;
    double sumSq = 0.0;
    int samples = 0;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const auto* px = pixels.data() + static_cast<size_t>((y * width + x) * 4);
            image.setPixelAt(x, height - 1 - y, juce::Colour(px[0], px[1], px[2]));
            if ((x & 3) == 0 && (y & 3) == 0)
            {
                const double lum = static_cast<double>(px[0] + px[1] + px[2]);
                sum += lum;
                sumSq += lum * lum;
                ++samples;
            }
        }
    }

    lastVariance = samples > 0 ? sumSq / samples - (sum / samples) * (sum / samples) : 0.0;
    captureFile.getParentDirectory().createDirectory();
    juce::FileOutputStream stream(captureFile);
    if (stream.openedOk())
    {
        juce::PNGImageFormat format;
        format.writeImageToStream(image, stream);
    }
    std::cout << "WAVIS_FRAME_VARIANCE " << lastVariance << " " << captureFile.getFullPathName() << "\n";
}
