// SD Studio (Qt 6) - GUI + embedded OpenAI-style HTTP server wrapping sd-cli.
//
//   * GUI: prompt / negative prompt, backend settings, model picker, model
//     downloader (Hugging Face), live preview, optional native background cutout.
//   * HTTP server (QTcpServer, no extra deps): OpenAI-style
//       POST /v1/images/generations   -> {"created":N,"data":[{"b64_json":...}]}
//       POST /generate                -> {"image_b64":..., "seconds":...}
//       GET  /health                  -> {"status":"ok"}
//     Both the GUI and the server share one serialized generation engine.
//
// Build: see CMakeLists.txt (Qt 6 Widgets + Network).

#include <QtWidgets>
#include <QtNetwork>
#include <atomic>
#include <functional>
#include <memory>

// ---------------------------------------------------------------------------
// Shared settings + engine
// ---------------------------------------------------------------------------
struct Settings {
    QString modelsDir = "D:/sd-models";
    QString sdRoot    = QDir::homePath() + "/.cache/lemonade/bin/sd-cpp";
    QString backend   = "vulkan";
    QString defaultModel;        // filename selected in the GUI
    QString negative  = "scenery, landscape, background, gradient, shadow, floor, sky, text, blurry";
    int     steps = 20, width = 512, height = 512;
    double  cfg = 7.0;
    int     port = 8801;
};
static Settings              g_cfg;
static std::atomic<bool>     g_busy{false};
static std::function<void(const QString&)>  g_log = [](const QString&){};
static std::function<void(const QImage&)>    g_showImage = [](const QImage&){};

static QString findSdCli(const QString& backend) {
    QString base = g_cfg.sdRoot + "/" + backend;
#ifdef Q_OS_WIN
    const QStringList pats = { "sd*.exe" };
#else
    const QStringList pats = { "sd-cli", "sd" };   // Linux/macOS: no .exe
#endif
    QDirIterator it(base, pats, QDir::Files, QDirIterator::Subdirectories);
    return it.hasNext() ? it.next() : QString();
}

static QString resolveModel(const QString& name) {
    if (name.isEmpty()) return {};
    if (QFileInfo::exists(name)) return name;                       // absolute path
    QString p = g_cfg.modelsDir + "/" + name;
    if (QFileInfo::exists(p)) return p;                            // filename in models dir
    for (const QString& ext : {".safetensors", ".ckpt"}) {        // bare name
        QString q = g_cfg.modelsDir + "/" + name + ext;
        if (QFileInfo::exists(q)) return q;
    }
    return {};
}

struct GenReq {
    QString prompt, negative, model, backend, trigger, lora;
    int steps = 0, width = 0, height = 0;
    double cfg = 0, loraWeight = 0.8;
    long seed = -1;
    bool cutout = false;
    bool triggerSet = false;
};
struct GenRes { bool ok = false; QString error, prompt; QByteArray png; double seconds = 0; };

// Native background removal: flood-fill from the image edges, clearing pixels
// whose colour matches the corner/background colour (within tolerance). Only
// edge-connected background is removed, so interior detail is preserved. Ideal
// for pixel-art sprites on a uniform background; no Python/rembg dependency.
static QImage cutoutBackground(QImage img, int tol = 42) {
    img = img.convertToFormat(QImage::Format_ARGB32);
    const int w = img.width(), h = img.height();
    if (w < 2 || h < 2) return img;

    int br = 0, bg = 0, bb = 0;                              // background = avg of 4 corners
    const int cx[4] = { 0, w - 1, 0, w - 1 }, cy[4] = { 0, 0, h - 1, h - 1 };
    for (int k = 0; k < 4; ++k) { QRgb p = img.pixel(cx[k], cy[k]); br += qRed(p); bg += qGreen(p); bb += qBlue(p); }
    br /= 4; bg /= 4; bb /= 4;
    auto isBg = [&](QRgb p) {
        return qAbs(qRed(p) - br) <= tol && qAbs(qGreen(p) - bg) <= tol && qAbs(qBlue(p) - bb) <= tol;
    };

    std::vector<char> mask(size_t(w) * h, 0);
    std::vector<int> stack;
    stack.reserve(size_t(w) * h / 4);
    auto tryPush = [&](int x, int y) {
        size_t i = size_t(y) * w + x;
        if (!mask[i] && isBg(img.pixel(x, y))) { mask[i] = 1; stack.push_back(int(i)); }
    };
    for (int x = 0; x < w; ++x) { tryPush(x, 0); tryPush(x, h - 1); }
    for (int y = 0; y < h; ++y) { tryPush(0, y); tryPush(w - 1, y); }
    while (!stack.empty()) {
        int i = stack.back(); stack.pop_back();
        int x = i % w, y = i / w;
        if (x > 0)     tryPush(x - 1, y);
        if (x < w - 1) tryPush(x + 1, y);
        if (y > 0)     tryPush(x, y - 1);
        if (y < h - 1) tryPush(x, y + 1);
    }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (mask[size_t(y) * w + x]) {
                QRgb p = img.pixel(x, y);
                img.setPixel(x, y, qRgba(qRed(p), qGreen(p), qBlue(p), 0));
            }
    return img;
}

static GenRes generate(GenReq r) {
    GenRes res;
    if (g_busy.exchange(true)) { res.error = "server busy (a generation is already running)"; return res; }
    struct Guard { ~Guard() { g_busy = false; } } guard;

    const QString backend = r.backend.isEmpty() ? g_cfg.backend : r.backend;
    const QString sd = findSdCli(backend);
    if (sd.isEmpty()) { res.error = "sd-cli not found for backend '" + backend + "' under " + g_cfg.sdRoot; return res; }
    const QString model = resolveModel(r.model.isEmpty() ? g_cfg.defaultModel : r.model);
    if (model.isEmpty()) { res.error = "model not found: " + (r.model.isEmpty() ? g_cfg.defaultModel : r.model); return res; }

    QString prompt = r.prompt.trimmed();
    if (prompt.isEmpty()) { res.error = "prompt is required"; return res; }
    // Only prepend a trigger when one is explicitly supplied (don't force
    // "pixelsprite" onto SDXL/other models).
    if (r.triggerSet && !r.trigger.isEmpty() && !prompt.contains(r.trigger, Qt::CaseInsensitive))
        prompt = r.trigger + ", " + prompt;
    // LoRA: sd.cpp applies it via <lora:name:weight> in the prompt + --lora-model-dir.
    if (!r.lora.isEmpty())
        prompt += QString(" <lora:%1:%2>").arg(r.lora).arg(r.loraWeight > 0 ? r.loraWeight : 0.8);
    res.prompt = prompt;

    const QString outDir = g_cfg.modelsDir + "/studio-out";
    QDir().mkpath(outDir);
    QString out = outDir + "/out.png";

    QStringList args = { "-m", model, "--vae-tiling", "--diffusion-fa",
        "-p", prompt,
        "-n", r.negative.isEmpty() ? g_cfg.negative : r.negative,
        "--cfg-scale", QString::number(r.cfg > 0 ? r.cfg : g_cfg.cfg),
        "--steps", QString::number(r.steps > 0 ? r.steps : g_cfg.steps),
        "-W", QString::number(r.width > 0 ? r.width : g_cfg.width),
        "-H", QString::number(r.height > 0 ? r.height : g_cfg.height),
        "-o", out };
    // LCM-distilled models require the LCM scheduler to produce good output.
    if (QFileInfo(model).fileName().contains("lcm", Qt::CaseInsensitive))
        args << "--sampling-method" << "lcm";
    if (r.seed >= 0) args << "-s" << QString::number(r.seed);
    if (!r.lora.isEmpty()) args << "--lora-model-dir" << (g_cfg.modelsDir + "/loras");

    g_log("> " + sd + " " + args.join(' '));
    QElapsedTimer timer; timer.start();
    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    // Inside a Flatpak the sandbox can't exec the host GPU binary; run it on the
    // host via flatpak-spawn (needs --talk-name=org.freedesktop.Flatpak).
    QString program = sd;
    QStringList runArgs = args;
    if (QFileInfo::exists("/.flatpak-info")) {
        runArgs.prepend(sd);
        runArgs.prepend("--host");
        program = "flatpak-spawn";
    }
    proc.start(program, runArgs);
    if (!proc.waitForStarted(5000)) { res.error = "failed to start sd-cli"; return res; }
    while (proc.state() != QProcess::NotRunning) {
        proc.waitForFinished(100);
        QCoreApplication::processEvents();   // keep the GUI alive during generation
    }
    const QString sdout = QString::fromLocal8Bit(proc.readAll());
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0 || !QFileInfo::exists(out)) {
        // A LoRA whose architecture doesn't match the base model trips an assert
        // in sd-cli's lora.hpp (tensor element-count mismatch) -> hard crash.
        if (!r.lora.isEmpty() && (sdout.contains("lora.hpp") || sdout.contains("ggml_nelements"))) {
            res.error = "LoRA '" + r.lora + "' doesn't match this model's architecture.\n"
                        "Use an SDXL LoRA (e.g. pixel-art-xl) with an SDXL model "
                        "(e.g. DreamShaper XL Turbo), and an SD-1.5 LoRA (e.g. PixelArtRedmond) "
                        "with an SD-1.5 model.";
        } else {
            res.error = "sd-cli failed (rc=" + QString::number(proc.exitCode()) + "): " + sdout.right(500);
        }
        return res;
    }

    if (r.cutout) {                          // native flood-fill background removal -> transparent PNG
        g_log("cutout (edge flood-fill)...");
        QImage src(out, "PNG");
        if (!src.isNull()) {
            QString cut = out; cut.replace(".png", "_cut.png");
            if (cutoutBackground(src).save(cut, "PNG")) out = cut;
            else g_log("cutout: save failed");
        } else g_log("cutout: could not load generated image");
    }

    QFile f(out);
    if (!f.open(QIODevice::ReadOnly)) { res.error = "cannot read output"; return res; }
    res.png = f.readAll();
    res.seconds = timer.elapsed() / 1000.0;
    res.ok = true;
    g_showImage(QImage::fromData(res.png, "PNG"));
    return res;
}

// ---------------------------------------------------------------------------
// Minimal HTTP server on QTcpServer (OpenAI-style image endpoint)
// ---------------------------------------------------------------------------
static GenReq reqFromJson(const QJsonObject& o) {
    GenReq r;
    r.prompt   = o.value("prompt").toString();
    r.negative = o.value("negative_prompt").toString();
    r.model    = o.value("model").toString();
    r.backend  = o.value("backend").toString();
    r.steps    = o.value("steps").toInt(0);
    r.cfg      = o.value("cfg_scale").toDouble(0);
    r.width    = o.value("width").toInt(0);
    r.height   = o.value("height").toInt(0);
    r.seed     = o.contains("seed") ? (long)o.value("seed").toInt(-1) : -1;
    r.cutout   = o.value("cutout").toBool(false);
    if (o.contains("trigger")) { r.trigger = o.value("trigger").toString(); r.triggerSet = true; }
    r.lora = o.value("lora").toString();
    r.loraWeight = o.value("lora_weight").toDouble(0.8);
    if (o.contains("size")) {                            // OpenAI "512x512"
        const QStringList wh = o.value("size").toString().toLower().split('x');
        if (wh.size() == 2) { r.width = wh[0].toInt(); r.height = wh[1].toInt(); }
    }
    return r;
}

static void httpReply(QTcpSocket* s, int code, const QByteArray& json) {
    QByteArray r = "HTTP/1.1 " + QByteArray::number(code) + " OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Length: " + QByteArray::number(json.size()) + "\r\n"
        "Connection: close\r\n\r\n" + json;
    s->write(r); s->flush();
    // Delete only AFTER the reply is sent. We deliberately don't wire deleteLater
    // on disconnect earlier, so a client closing mid-generation can't free the
    // socket while generate() is pumping the event loop (that caused a crash).
    QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
    s->disconnectFromHost();
}

static void handleHttp(QTcpSocket* sock, const QByteArray& buf) {
    int hdrEnd = buf.indexOf("\r\n\r\n");
    const QByteArray head = buf.left(hdrEnd);
    const QByteArray body = buf.mid(hdrEnd + 4);
    const QByteArray firstLine = head.left(head.indexOf('\r'));
    const QList<QByteArray> p = firstLine.split(' ');
    const QString method = p.value(0), path = p.value(1);

    if (method == "GET" && path.startsWith("/health")) {
        httpReply(sock, 200, R"({"status":"ok"})"); return;
    }
    if (method == "POST" && (path.startsWith("/generate") || path.startsWith("/v1/images/generations"))) {
        const QJsonObject in = QJsonDocument::fromJson(body).object();
        GenRes g = generate(reqFromJson(in));
        if (!g.ok) {
            QJsonObject err{ { "error", QJsonObject{ { "message", g.error }, { "type", "backend_error" } } } };
            httpReply(sock, g.error.startsWith("server busy") ? 503 : 500,
                      QJsonDocument(err).toJson(QJsonDocument::Compact));
            return;
        }
        const QString b64 = QString::fromLatin1(g.png.toBase64());
        QJsonObject out;
        if (path.startsWith("/v1/images/generations")) {
            out = QJsonObject{ { "created", (qint64)QDateTime::currentSecsSinceEpoch() },
                               { "data", QJsonArray{ QJsonObject{ { "b64_json", b64 } } } } };
        } else {
            out = QJsonObject{ { "image_b64", b64 }, { "seconds", g.seconds }, { "prompt", g.prompt } };
        }
        httpReply(sock, 200, QJsonDocument(out).toJson(QJsonDocument::Compact));
        return;
    }
    httpReply(sock, 404, R"({"error":"not found"})");
}

static QTcpServer* startServer(quint16 port) {
    auto* srv = new QTcpServer();
    if (!srv->listen(QHostAddress::Any, port)) {
        g_log("HTTP listen failed on " + QString::number(port) + ": " + srv->errorString());
        return srv;
    }
    QObject::connect(srv, &QTcpServer::newConnection, [srv]() {
        while (srv->hasPendingConnections()) {
            QTcpSocket* sock = srv->nextPendingConnection();
            auto buf = std::make_shared<QByteArray>();
            auto handled = std::make_shared<bool>(false);
            QObject::connect(sock, &QTcpSocket::readyRead, [sock, buf, handled]() {
                if (*handled) return;                                 // already dispatched (generate() pumps events)
                buf->append(sock->readAll());
                int he = buf->indexOf("\r\n\r\n");
                if (he < 0) return;                                   // headers incomplete
                int cl = 0;
                for (const QByteArray& line : buf->left(he).split('\n'))
                    if (line.toLower().startsWith("content-length:")) cl = line.mid(15).trimmed().toInt();
                if (buf->size() - (he + 4) < cl) return;              // body incomplete
                *handled = true;
                handleHttp(sock, *buf);
            });
        }
    });
    g_log(srv->isListening() ? "HTTP server on :" + QString::number(port) +
          "  (POST /generate | /v1/images/generations)" : "HTTP server NOT started");
    return srv;
}

// ---------------------------------------------------------------------------
// Preview widget (nearest-neighbour scaling -> crisp pixels)
// ---------------------------------------------------------------------------
class ImageView : public QWidget {
public:
    explicit ImageView(QWidget* p = nullptr) : QWidget(p) { setMinimumSize(360, 360); }
    void setImage(const QImage& i) { m_img = i; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        // Checkerboard so transparency is obvious: transparent pixels show the
        // pattern (= background removed); an opaque background covers it.
        const int cs = 12;
        for (int y = 0; y < height(); y += cs)
            for (int x = 0; x < width(); x += cs)
                p.fillRect(x, y, cs, cs, ((x / cs + y / cs) & 1) ? QColor(64, 64, 68) : QColor(42, 42, 46));
        if (m_img.isNull()) { p.setPen(Qt::lightGray); p.drawText(12, 24, "preview"); return; }
        QSize s = m_img.size().scaled(size(), Qt::KeepAspectRatio);
        QImage scaled = m_img.scaled(s, Qt::KeepAspectRatio, Qt::FastTransformation);
        p.drawImage((width() - s.width()) / 2, (height() - s.height()) / 2, scaled);
    }
private:
    QImage m_img;
};

// ---------------------------------------------------------------------------
// Stream a download straight to disk. reply->readAll() buffers the whole file
// in RAM, which corrupts/fails for multi-GB models (e.g. SDXL ~6.6 GB).
static void startDownload(QNetworkAccessManager* nam, const QUrl& url, const QString& dest,
                          QProgressBar* bar, std::function<void()> onDone) {
    QFile* file = new QFile(dest);
    if (!file->open(QIODevice::WriteOnly)) { g_log("Cannot open " + dest); delete file; return; }
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* rep = nam->get(req);
    bar->setRange(0, 100); bar->setValue(0);
    QObject::connect(rep, &QNetworkReply::readyRead, [rep, file]() { file->write(rep->readAll()); });
    QObject::connect(rep, &QNetworkReply::downloadProgress, [bar](qint64 a, qint64 b) { if (b > 0) bar->setValue(int(100 * a / b)); });
    QObject::connect(rep, &QNetworkReply::finished, [rep, file, dest, onDone]() {
        file->write(rep->readAll());
        file->close();
        const bool ok = rep->error() == QNetworkReply::NoError;
        const QString err = rep->errorString();
        delete file;
        rep->deleteLater();
        if (!ok) { g_log("Download failed: " + err); QFile::remove(dest); return; }
        g_log("Saved " + dest);
        if (onDone) onDone();
    });
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("xLights");
    QApplication::setApplicationName("SDStudio");
    QApplication::setWindowIcon(QIcon(":/sd_studio.png"));

#ifndef Q_OS_WIN
    g_cfg.modelsDir = QDir::homePath() + "/sd-models";   // sensible non-Windows default
#endif

    // Portable mode: if sd-cpp/ and models/ are shipped next to the exe, prefer
    // them over saved settings so the bundle is self-contained on any machine.
    const QString appDir = QCoreApplication::applicationDirPath();
    const bool bundledSd     = QDir(appDir + "/sd-cpp").exists();
    const bool bundledModels = QDir(appDir + "/models").exists();
    QSettings st;
    g_cfg.sdRoot    = bundledSd     ? appDir + "/sd-cpp" : st.value("sdRoot", g_cfg.sdRoot).toString();
    g_cfg.modelsDir = bundledModels ? appDir + "/models" : st.value("modelsDir", g_cfg.modelsDir).toString();
    g_cfg.backend   = st.value("backend", g_cfg.backend).toString();
    g_cfg.port      = st.value("port", g_cfg.port).toInt();

    QWidget win;
    win.setWindowTitle("SD Studio");
    win.resize(1120, 740);
    auto* outer = new QHBoxLayout(&win);

    // ---- left column ----
    auto* left = new QVBoxLayout();
    auto* form = new QFormLayout();

    auto* modelsDir = new QLineEdit(g_cfg.modelsDir);
    auto* browse = new QPushButton("...");  browse->setMaximumWidth(32);
    auto* mdRow = new QHBoxLayout(); mdRow->addWidget(modelsDir); mdRow->addWidget(browse);
    form->addRow("Models dir", mdRow);

    auto* model = new QComboBox();
    auto* refresh = new QPushButton("Refresh"); refresh->setMaximumWidth(72);
    auto* moRow = new QHBoxLayout(); moRow->addWidget(model, 1); moRow->addWidget(refresh);
    form->addRow("Model", moRow);

    auto* backend = new QComboBox(); backend->addItems({ "vulkan", "cuda", "cpu" });
    backend->setCurrentText(g_cfg.backend);
    form->addRow("Backend", backend);

    auto* sdRoot = new QLineEdit(g_cfg.sdRoot);
    form->addRow("sd-cpp dir", sdRoot);

    auto* steps = new QSpinBox(); steps->setRange(1, 150); steps->setValue(g_cfg.steps);
    form->addRow("Steps", steps);
    auto* cfg = new QDoubleSpinBox(); cfg->setRange(0, 30); cfg->setSingleStep(0.5); cfg->setValue(g_cfg.cfg);
    form->addRow("CFG scale", cfg);
    auto* width = new QSpinBox(); width->setRange(64, 2048); width->setSingleStep(64); width->setValue(g_cfg.width);
    form->addRow("Width", width);
    auto* height = new QSpinBox(); height->setRange(64, 2048); height->setSingleStep(64); height->setValue(g_cfg.height);
    form->addRow("Height", height);
    auto* seed = new QLineEdit("-1");
    form->addRow("Seed (-1=rnd)", seed);
    auto* lora = new QComboBox();
    auto* loraW = new QDoubleSpinBox(); loraW->setRange(0, 2); loraW->setSingleStep(0.1); loraW->setValue(0.8);
    auto* loraDL = new QPushButton("DL"); loraDL->setMaximumWidth(40);
    auto* loraRow = new QHBoxLayout(); loraRow->addWidget(lora, 1); loraRow->addWidget(loraW); loraRow->addWidget(loraDL);
    form->addRow("LoRA", loraRow);
    auto* cutout = new QCheckBox("Cutout background (transparent PNG)");
    form->addRow("", cutout);
    auto* port = new QSpinBox(); port->setRange(1, 65535); port->setValue(g_cfg.port);
    auto* portLbl = new QLabel();
    auto* portRow = new QHBoxLayout(); portRow->addWidget(port); portRow->addWidget(portLbl, 1);
    form->addRow("HTTP port", portRow);

    left->addLayout(form);

    left->addWidget(new QLabel("Prompt"));
    auto* prompt = new QPlainTextEdit("pixelsprite, a decorated christmas tree with a star");
    prompt->setFixedHeight(60); left->addWidget(prompt);
    left->addWidget(new QLabel("Negative prompt"));
    auto* negative = new QPlainTextEdit(g_cfg.negative);
    negative->setFixedHeight(48); left->addWidget(negative);

    auto* genBtn = new QPushButton("Generate");
    auto* dlBtn  = new QPushButton("Download model...");
    auto* btnRow = new QHBoxLayout(); btnRow->addWidget(genBtn, 1); btnRow->addWidget(dlBtn, 1);
    left->addLayout(btnRow);

    auto* bar = new QProgressBar(); bar->setRange(0, 100); bar->setValue(0);
    left->addWidget(bar);
    auto* log = new QPlainTextEdit(); log->setReadOnly(true); log->setMaximumBlockCount(500);
    log->setFixedHeight(150); left->addWidget(log);

    outer->addLayout(left, 0);

    // ---- right: preview ----
    auto* preview = new ImageView();
    outer->addWidget(preview, 1);

    // ---- wiring ----
    g_log = [log](const QString& s) { log->appendPlainText(s); };
    g_showImage = [preview](const QImage& i) { if (!i.isNull()) preview->setImage(i); };

    auto syncCfg = [&]() {
        g_cfg.modelsDir = modelsDir->text();
        g_cfg.sdRoot = sdRoot->text();
        g_cfg.backend = backend->currentText();
        g_cfg.steps = steps->value();
        g_cfg.cfg = cfg->value();
        g_cfg.width = width->value();
        g_cfg.height = height->value();
        g_cfg.negative = negative->toPlainText();
        if (model->currentData().isValid()) g_cfg.defaultModel = model->currentData().toString();
    };

    auto refreshModels = [=]() {
        model->clear();
        QDir d(modelsDir->text());
        if (!d.exists()) { g_log("Models dir missing: " + modelsDir->text()); return; }
        QStringList files = d.entryList({ "*.safetensors", "*.ckpt" }, QDir::Files, QDir::Name);
        for (const QString& f : files) model->addItem(f, d.absoluteFilePath(f));
        if (model->count()) model->setCurrentIndex(0);
    };
    auto refreshLoras = [=]() {
        lora->clear();
        lora->addItem("(none)", QString());
        QDir d(modelsDir->text() + "/loras");
        for (const QString& f : d.entryList({ "*.safetensors" }, QDir::Files, QDir::Name))
            lora->addItem(QFileInfo(f).completeBaseName(), QFileInfo(f).completeBaseName());
    };

    // Turbo/XL-aware defaults: pick sensible CFG + steps for the selected model.
    QObject::connect(model, QOverload<int>::of(&QComboBox::currentIndexChanged), [=](int) {
        const QString n = model->currentText().toLower();
        if (n.isEmpty()) return;
        if      (n.contains("lcm"))       { steps->setValue(6);  cfg->setValue(1.5); }
        else if (n.contains("turbo"))     { steps->setValue(8);  cfg->setValue(2.0); }
        else if (n.contains("lightning")) { steps->setValue(6);  cfg->setValue(1.5); }
        else if (n.contains("xl"))        { steps->setValue(26); cfg->setValue(6.0); }
        else                              { steps->setValue(20); cfg->setValue(7.0); }
    });

    refreshModels();
    refreshLoras();
    syncCfg();   // initialize g_cfg (incl. defaultModel) so the HTTP server works before any GUI click

    QObject::connect(refresh, &QPushButton::clicked, [=]() { refreshModels(); refreshLoras(); });
    QObject::connect(browse, &QPushButton::clicked, [&]() {
        QString dir = QFileDialog::getExistingDirectory(&win, "Models directory", modelsDir->text());
        if (!dir.isEmpty()) { modelsDir->setText(dir); refreshModels(); refreshLoras(); }
    });

    QObject::connect(genBtn, &QPushButton::clicked, [&]() {
        syncCfg();
        GenReq r;
        r.prompt = prompt->toPlainText();
        r.negative = negative->toPlainText();
        if (model->currentData().isValid()) r.model = model->currentData().toString();
        r.backend = backend->currentText();
        r.steps = steps->value(); r.cfg = cfg->value();
        r.width = width->value(); r.height = height->value();
        r.seed = seed->text().toLong();
        r.lora = lora->currentData().toString();
        r.loraWeight = loraW->value();
        r.cutout = cutout->isChecked();
        genBtn->setEnabled(false); bar->setRange(0, 0);   // busy indicator
        GenRes g = generate(r);
        bar->setRange(0, 100); bar->setValue(g.ok ? 100 : 0);
        genBtn->setEnabled(true);
        if (!g.ok) { g_log("ERROR: " + g.error); QMessageBox::warning(&win, "Generate failed", g.error); }
        else g_log(QString("done in %1s").arg(g.seconds));
    });

    // model download (Hugging Face) via QNetworkAccessManager
    auto* nam = new QNetworkAccessManager(&win);
    QObject::connect(dlBtn, &QPushButton::clicked, [&, nam]() {
        const QList<QPair<QString, QString>> presets = {
            { "DreamShaper XL Turbo  (best quality; CFG~2, ~8 steps)", "Lykon/dreamshaper-xl-v2-turbo:DreamShaperXL_Turbo_v2_1.safetensors" },
            { "DreamShaper 8 LCM  (SD1.5 turbo, ~6 steps)",            "Lykon/dreamshaper-8-lcm:DreamShaper8_LCM.safetensors" },
            { "SD-Turbo  (small/fast, ~4 steps, low CFG)",             "stabilityai/sd-turbo:sd_turbo.safetensors" },
            { "All-In-One Pixel  (pixelsprite / 16bitscene)", "PublicPrompts/All-In-One-Pixel-Model:Public-Prompts-Pixel-Model.ckpt" },
            { "Pixel-Art Style  (pixelartstyle)",             "kohbanye/pixel-art-style:pixel-art-style.ckpt" },
            { "Pixel SpriteSheet  (PixelartFSS)",             "Onodofthenorth/SD_PixelArt_SpriteSheet_Generator:PixelartSpritesheet_V.1.ckpt" },
            { "Voxel Art  (VoxelArt)",                        "Fictiverse/Stable_Diffusion_VoxelArt_Model:VoxelArt_v1.safetensors" },
            { "Paper Cut  (PaperCut)",                        "Fictiverse/Stable_Diffusion_PaperCut_Model:PaperCut_v1.safetensors" },
            { "Custom (repo:file)...",                        "" },
        };
        QStringList names; for (auto& p : presets) names << p.first;
        bool ok = false;
        QString choice = QInputDialog::getItem(&win, "Download model", "Model:", names, 0, false, &ok);
        if (!ok) return;
        QString ref = presets[names.indexOf(choice)].second;
        if (ref.isEmpty()) ref = QInputDialog::getText(&win, "Custom model", "repo:file", QLineEdit::Normal, "", &ok);
        if (!ok || !ref.contains(':')) return;
        const QString repo = ref.section(':', 0, 0), file = ref.section(':', 1);
        const QUrl url("https://huggingface.co/" + repo + "/resolve/main/" + file + "?download=true");
        const QString dest = QDir(modelsDir->text()).absoluteFilePath(QFileInfo(file).fileName());

        g_log("Downloading " + url.toString());
        startDownload(nam, url, dest, bar, [=]() { refreshModels(); model->setCurrentText(QFileInfo(dest).fileName()); });
    });

    // LoRA download (Hugging Face) -> <modelsDir>/loras
    QObject::connect(loraDL, &QPushButton::clicked, [&, nam]() {
        const QList<QPair<QString, QString>> presets = {
            { "PixelArtRedmond  (SD1.5, trigger PIXARFK)", "artificialguybr/pixelartredmond-1-5v-pixel-art-loras-for-sd-1-5:PixelArtRedmond15V-PixelArt-PIXARFK.safetensors" },
            { "Pixel Art XL  (SDXL, trigger pixel)",       "nerijs/pixel-art-xl:pixel-art-xl.safetensors" },
            { "Custom (repo:file)...",                     "" },
        };
        QStringList names; for (auto& p : presets) names << p.first;
        bool ok = false;
        QString choice = QInputDialog::getItem(&win, "Download LoRA", "LoRA:", names, 0, false, &ok);
        if (!ok) return;
        QString ref = presets[names.indexOf(choice)].second;
        if (ref.isEmpty()) ref = QInputDialog::getText(&win, "Custom LoRA", "repo:file", QLineEdit::Normal, "", &ok);
        if (!ok || !ref.contains(':')) return;
        const QString repo = ref.section(':', 0, 0), file = ref.section(':', 1);
        const QUrl url("https://huggingface.co/" + repo + "/resolve/main/" + file + "?download=true");
        const QString ldir = modelsDir->text() + "/loras";
        QDir().mkpath(ldir);
        const QString dest = QDir(ldir).absoluteFilePath(QFileInfo(file).fileName());

        g_log("Downloading LoRA " + url.toString());
        startDownload(nam, url, dest, bar, [=]() { refreshLoras(); lora->setCurrentText(QFileInfo(dest).completeBaseName()); });
    });

    // start the HTTP server
    QTcpServer* server = nullptr;
    auto restartServer = [&]() {
        if (server) { server->close(); server->deleteLater(); }
        g_cfg.port = port->value();
        server = startServer((quint16)g_cfg.port);
        portLbl->setText(server->isListening() ? "listening" : "FAILED");
    };
    QObject::connect(port, QOverload<int>::of(&QSpinBox::valueChanged), [&](int){ restartServer(); });
    restartServer();

    QObject::connect(&app, &QApplication::aboutToQuit, [&]() {
        QSettings s;
        syncCfg();
        s.setValue("modelsDir", g_cfg.modelsDir); s.setValue("sdRoot", g_cfg.sdRoot);
        s.setValue("backend", g_cfg.backend); s.setValue("port", g_cfg.port);
    });

    win.show();
    return app.exec();
}
