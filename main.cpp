#include <QApplication>
#include <QCloseEvent>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMainWindow>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QProcess>
#include <QPushButton>
#include <QSize>
#include <QTcpSocket>
#include <QTextStream>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QVariant>
#include <QWebEngineHistory>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>
#include <QWebEngineFullScreenRequest>

namespace {
constexpr auto WAITING_HTML = R"HTML(
<!doctype html>
<html lang="ja">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Waiting</title>
  <style>
    body {
      margin: 0;
      min-height: 100vh;
      display: grid;
      place-items: center;
      font-family: sans-serif;
      background: linear-gradient(135deg, #0f172a, #1e293b);
      color: #e2e8f0;
    }
    .card {
      text-align: center;
      padding: 40px 48px;
      border: 1px solid rgba(255,255,255,0.16);
      border-radius: 18px;
      background: rgba(15, 23, 42, 0.7);
      box-shadow: 0 20px 60px rgba(0,0,0,0.35);
    }
    h1 { font-size: 42px; margin: 0 0 12px; }
    p { font-size: 20px; margin: 0; opacity: 0.85; }
  </style>
</head>
<body>
  <div class="card">
    <h1>接続待機中...</h1>
    <p>ネットワーク疎通を確認しています</p>
  </div>
</body>
</html>
)HTML";

enum class LogMode {
    Off,
    Normal,
    Debug,
};

QString boolString(bool value) {
    return value ? "true" : "false";
}

QString logModeName(LogMode mode) {
    switch (mode) {
    case LogMode::Off:
        return "off";
    case LogMode::Normal:
        return "normal";
    case LogMode::Debug:
        return "debug";
    }
    return "normal";
}

LogMode parseLogMode(const QString &value) {
    const QString normalized = value.trimmed().toLower();
    if (normalized == "off") {
        return LogMode::Off;
    }
    if (normalized == "debug") {
        return LogMode::Debug;
    }
    return LogMode::Normal;
}
}

class BrowserWindow : public QMainWindow {
public:
    struct PageEntry {
        QUrl url;
        int durationSec = 60;
    };

    class KioskPage : public QWebEnginePage {
    public:
        explicit KioskPage(BrowserWindow *window)
            : QWebEnginePage(window), window_(window) {}

    protected:
        QWebEnginePage *createWindow(QWebEnginePage::WebWindowType type) override;
        bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame) override;

    private:
        BrowserWindow *window_ = nullptr;
    };

    class DomainPolicyInterceptor : public QWebEngineUrlRequestInterceptor {
    public:
        DomainPolicyInterceptor(const QStringList &allowRules,
                                const QStringList &denyRules,
                                BrowserWindow *window)
            : allowRules_(allowRules), denyRules_(denyRules), window_(window) {}

        void interceptRequest(QWebEngineUrlRequestInfo &info) override;

    private:
        static bool matchesRule(const QString &host, const QString &rule) {
            if (rule.isEmpty()) {
                return false;
            }
            if (rule.startsWith("*.")) {
                const QString suffix = rule.mid(1);
                return host.endsWith(suffix);
            }
            return host == rule;
        }

        bool isAllowedHost(const QString &host, QString *reason) const {
            for (const QString &rule : denyRules_) {
                if (matchesRule(host, rule)) {
                    *reason = "deny rule=" + rule;
                    return false;
                }
            }

            if (!allowRules_.isEmpty()) {
                for (const QString &rule : allowRules_) {
                    if (matchesRule(host, rule)) {
                        *reason = "allow rule=" + rule;
                        return true;
                    }
                }
                *reason = "not matched in allow list";
                return false;
            }

            *reason = "default allow";
            return true;
        }

        QStringList allowRules_;
        QStringList denyRules_;
        BrowserWindow *window_ = nullptr;
    };

    BrowserWindow(const QUrl &homepage,
                  const QString &checkType,
                  const QString &checkTarget,
                  quint16 checkPort,
                  bool showUrlBox,
                  bool showHomeButton,
                  bool showBackButton,
                  int autoHomeSec,
                  int toolbarHeight,
                  bool disableContextMenu,
                  const QString &pageListJson,
                  const QString &domainPolicyJson,
                  const QString &userAgent,
                  const QString &logDir,
                  LogMode logMode,
                  const QString &startupSummary)
        : homepage_(homepage),
          checkType_(checkType),
          checkTarget_(checkTarget),
          checkPort_(checkPort),
          autoHomeMs_(autoHomeSec > 0 ? autoHomeSec * 1000 : 0),
          toolbarHeight_(toolbarHeight > 0 ? toolbarHeight : 56),
          disableContextMenu_(disableContextMenu),
          pageListJson_(pageListJson),
          domainPolicyJson_(domainPolicyJson),
          logDir_(logDir),
          logMode_(logMode) {
        initializeLogFile();
        logNormal("startup " + startupSummary + " log_dir=" + logDir_ + " log_mode=" + logModeName(logMode_));

        view_ = new QWebEngineView(this);
        page_ = new KioskPage(this);
        view_->setPage(page_);
        if (!userAgent.trimmed().isEmpty()) {
            view_->page()->profile()->setHttpUserAgent(userAgent.trimmed());
        }
        setCentralWidget(view_);
        if (disableContextMenu_) {
            view_->setContextMenuPolicy(Qt::NoContextMenu);
        }

        loadDomainPolicy();
        if (domainPolicyEnabled_) {
            interceptor_ = new DomainPolicyInterceptor(allowRules_, denyRules_, this);
            view_->page()->profile()->setUrlRequestInterceptor(interceptor_);
        }
        loadPageList();

        auto *toolbar = addToolBar("Navigation");
        toolbar->setMovable(false);
        toolbar->setMinimumHeight(toolbarHeight_);
        toolbar->setMaximumHeight(toolbarHeight_);
        toolbar->setIconSize(QSize(toolbarHeight_ - 16, toolbarHeight_ - 16));

        const int controlHeight = qMax(24, toolbarHeight_ - 8);
        const int lineEditHeight = qMax(22, toolbarHeight_ - 12);
        const int fontPixelSize = qMax(14, toolbarHeight_ * 35 / 100);

        homeButton_ = new QPushButton("Home", this);
        backButton_ = new QPushButton("Back", this);
        urlBox_ = new QLineEdit(this);
        goButton_ = new QPushButton("Go", this);
        urlBox_->setPlaceholderText("URL");

        if (showHomeButton) {
            toolbar->addWidget(homeButton_);
        }
        if (showBackButton) {
            toolbar->addWidget(backButton_);
        }
        if (showUrlBox) {
            toolbar->addWidget(urlBox_);
            toolbar->addWidget(goButton_);
        }

        homeButton_->setMinimumHeight(controlHeight);
        homeButton_->setMaximumHeight(controlHeight);
        backButton_->setMinimumHeight(controlHeight);
        backButton_->setMaximumHeight(controlHeight);
        goButton_->setMinimumHeight(controlHeight);
        goButton_->setMaximumHeight(controlHeight);
        urlBox_->setMinimumHeight(lineEditHeight);
        urlBox_->setMaximumHeight(lineEditHeight);

        QFont buttonFont = homeButton_->font();
        buttonFont.setPixelSize(fontPixelSize);
        homeButton_->setFont(buttonFont);
        backButton_->setFont(buttonFont);
        goButton_->setFont(buttonFont);

        QFont lineEditFont = urlBox_->font();
        lineEditFont.setPixelSize(fontPixelSize);
        urlBox_->setFont(lineEditFont);
        urlBox_->setStyleSheet(QString("QLineEdit { padding: 0 12px; font-size: %1px; }").arg(fontPixelSize));

        if (disableContextMenu_) {
            urlBox_->setContextMenuPolicy(Qt::NoContextMenu);
            homeButton_->setContextMenuPolicy(Qt::NoContextMenu);
            backButton_->setContextMenuPolicy(Qt::NoContextMenu);
            goButton_->setContextMenuPolicy(Qt::NoContextMenu);
        }

        connect(homeButton_, &QPushButton::clicked, this, [this]() {
            logNormal("ui homebutton clicked");
            loadHomepage();
            if (pageListEnabled_) {
                currentPageIndex_ = 0;
                schedulePageCycle(pageList_.isEmpty() ? 60 : pageList_.first().durationSec);
            }
        });
        connect(backButton_, &QPushButton::clicked, this, [this]() {
            if (view_->history()->canGoBack()) {
                logNormal("ui backbutton clicked");
                view_->back();
                return;
            }
            logNormal("ui backbutton ignored");
        });
        connect(goButton_, &QPushButton::clicked, this, [this]() {
            markUserActivity();
            const auto target = QUrl::fromUserInput(urlBox_->text());
            logNormal("ui gobutton clicked url=" + target.toString());
            view_->load(target);
        });
        connect(urlBox_, &QLineEdit::returnPressed, this, [this]() {
            markUserActivity();
            const auto target = QUrl::fromUserInput(urlBox_->text());
            logNormal("ui urlbox navigate url=" + target.toString());
            view_->load(target);
        });
        connect(view_, &QWebEngineView::urlChanged, this, [this](const QUrl &url) {
            if (urlBox_->isVisible()) {
                urlBox_->setText(url.toString());
            }
            logDebug("view urlChanged url=" + url.toString());
        });
        connect(view_->page(), &QWebEnginePage::fullScreenRequested, this, [this](QWebEngineFullScreenRequest request) {
            request.reject();
            logNormal("fullscreen request rejected origin=" + request.origin().toString());
        });

        probeTimer_ = new QTimer(this);
        probeTimer_->setInterval(3000);
        connect(probeTimer_, &QTimer::timeout, this, [this]() { runProbe(); });

        autoHomeTimer_ = new QTimer(this);
        autoHomeTimer_->setSingleShot(true);
        connect(autoHomeTimer_, &QTimer::timeout, this, [this]() {
            logNormal("timer autohome timeout online=" + boolString(isOnline_));
            if (isOnline_) {
                loadHomepage();
            }
        });

        pageCycleTimer_ = new QTimer(this);
        pageCycleTimer_->setSingleShot(true);
        connect(pageCycleTimer_, &QTimer::timeout, this, [this]() { advancePageCycle(); });

        installEventFilter(this);
        view_->installEventFilter(this);
        urlBox_->installEventFilter(this);
        homeButton_->installEventFilter(this);
        backButton_->installEventFilter(this);
        goButton_->installEventFilter(this);

        resize(1280, 800);
        showFullScreen();
        showWaitingPage();
        probeTimer_->start();
        QTimer::singleShot(0, this, [this]() { runProbe(); });
    }

    void logDomainDecision(const QString &url, const QString &host, bool allowed, const QString &reason) {
        const QString status = allowed ? "allow" : "block";
        logNormal("domain policy " + status + " host=" + host + " url=" + url + " reason=" + reason);
    }

    void logNormal(const QString &message) { writeLog(message); }
    bool isAllowedNavigationUrl(const QUrl &url, QString *reason) const {
        if (!url.isValid() || url.isEmpty()) {
            *reason = "invalid or empty url";
            return false;
        }

        const QString scheme = url.scheme().trimmed().toLower();
        if (scheme != "http" && scheme != "https") {
            *reason = "blocked scheme=" + scheme;
            return false;
        }

        *reason = "allowed scheme=" + scheme;
        return true;
    }
    void loadUrlInCurrentView(const QUrl &url, const QString &source) {
        QString reason;
        if (!isAllowedNavigationUrl(url, &reason)) {
            logNormal(source + " blocked url=" + url.toString() + " reason=" + reason);
            return;
        }
        logNormal(source + " url=" + url.toString());
        view_->load(url);
    }

protected:
    void closeEvent(QCloseEvent *event) override {
        logNormal("window close blocked");
        event->ignore();
    }

    void changeEvent(QEvent *event) override {
        QMainWindow::changeEvent(event);
        if (event->type() == QEvent::WindowStateChange && !isFullScreen()) {
            logNormal("window state changed -> restore fullscreen");
            QTimer::singleShot(0, this, [this]() { showFullScreen(); });
        }
    }

    bool eventFilter(QObject *obj, QEvent *event) override {
        Q_UNUSED(obj);
        if (disableContextMenu_ && event->type() == QEvent::ContextMenu) {
            logDebug("ui context menu blocked");
            return true;
        }

        if (event->type() == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            const Qt::KeyboardModifiers mods = keyEvent->modifiers();
            const int key = keyEvent->key();
            const bool blocked =
                key == Qt::Key_Escape ||
                key == Qt::Key_F11 ||
                (mods.testFlag(Qt::ControlModifier) && (key == Qt::Key_Q || key == Qt::Key_W)) ||
                (mods.testFlag(Qt::AltModifier) && (key == Qt::Key_Left || key == Qt::Key_Right)) ||
                key == Qt::Key_Back ||
                key == Qt::Key_Forward;
            if (blocked) {
                logNormal("key blocked key=" + QString::number(key) + " modifiers=" + QString::number(int(mods)));
                return true;
            }
        }

        switch (event->type()) {
        case QEvent::KeyPress:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseMove:
        case QEvent::Wheel:
            markUserActivity();
            break;
        default:
            break;
        }
        return QMainWindow::eventFilter(obj, event);
    }

private:
    void loadPageList() {
        if (pageListJson_.isEmpty()) {
            logNormal("page list disabled");
            return;
        }

        QFile file(pageListJson_);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            logNormal("page list load failed path=" + pageListJson_ + " error=" + file.errorString());
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            logNormal("page list parse failed path=" + pageListJson_ + " error=" + parseError.errorString());
            return;
        }

        const QJsonObject object = doc.object();
        const int defaultDurationSec = qMax(1, object.value("default_duration_sec").toInt(60));
        const QJsonArray pages = object.value("pages").toArray();

        for (const auto &entryValue : pages) {
            const QJsonObject entryObject = entryValue.toObject();
            const QUrl url = QUrl::fromUserInput(entryObject.value("url").toString().trimmed());
            if (!url.isValid() || url.isEmpty()) {
                logNormal("page list skipped invalid url");
                continue;
            }

            PageEntry entry;
            entry.url = url;
            entry.durationSec = qMax(1, entryObject.value("duration_sec").toInt(defaultDurationSec));
            pageList_.append(entry);
        }

        if (pageList_.isEmpty()) {
            logNormal("page list loaded but empty path=" + QFileInfo(pageListJson_).fileName());
            return;
        }

        pageListEnabled_ = true;
        if (autoHomeMs_ > 0) {
            autoHomeMs_ = 0;
            logNormal("autohomesec ignored because page-list-json is enabled");
        }
        logNormal("page list loaded path=" + QFileInfo(pageListJson_).fileName() +
                  " page_count=" + QString::number(pageList_.size()) +
                  " default_duration_sec=" + QString::number(defaultDurationSec));
    }

    void loadDomainPolicy() {
        if (domainPolicyJson_.isEmpty()) {
            logNormal("domain policy disabled");
            return;
        }

        QFile file(domainPolicyJson_);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            logNormal("domain policy load failed path=" + domainPolicyJson_ + " error=" + file.errorString());
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            logNormal("domain policy parse failed path=" + domainPolicyJson_ + " error=" + parseError.errorString());
            return;
        }

        const auto object = doc.object();
        const auto loadList = [](const QJsonValue &value) {
            QStringList list;
            for (const auto &entry : value.toArray()) {
                const QString rule = entry.toString().trimmed().toLower();
                if (!rule.isEmpty()) {
                    list << rule;
                }
            }
            return list;
        };

        allowRules_ = loadList(object.value("allow"));
        denyRules_ = loadList(object.value("deny"));
        domainPolicyEnabled_ = true;
        logNormal("domain policy loaded path=" + QFileInfo(domainPolicyJson_).fileName() +
                  " allow_count=" + QString::number(allowRules_.size()) +
                  " deny_count=" + QString::number(denyRules_.size()));
    }

    void initializeLogFile() {
        if (logMode_ == LogMode::Off) {
            return;
        }
        QDir().mkpath(logDir_);
        logFile_.setFileName(logDir_ + "/kiosk-browser.log");
        logFile_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
        logStream_.setDevice(&logFile_);
    }

    void writeLog(const QString &message) {
        if (logMode_ == LogMode::Off || !logFile_.isOpen()) {
            return;
        }
        const QString ts = QDateTime::currentDateTime().toString(Qt::ISODate);
        logStream_ << ts << " " << message << "\n";
        logStream_.flush();
    }

    void logDebug(const QString &message) {
        if (logMode_ == LogMode::Debug) {
            writeLog(message);
        }
    }

    void schedulePageCycle(int durationSec) {
        if (!pageListEnabled_ || !pageCycleTimer_) {
            return;
        }
        const int safeDurationSec = qMax(1, durationSec);
        pageCycleTimer_->start(safeDurationSec * 1000);
        logNormal("page cycle scheduled index=" + QString::number(currentPageIndex_) +
                  " duration_sec=" + QString::number(safeDurationSec));
    }

    void loadPageEntry(int index) {
        if (!pageListEnabled_ || pageList_.isEmpty()) {
            return;
        }
        currentPageIndex_ = index % pageList_.size();
        const PageEntry &entry = pageList_.at(currentPageIndex_);
        isOnline_ = true;
        pageCycleActive_ = true;
        view_->load(entry.url);
        logNormal("page cycle load index=" + QString::number(currentPageIndex_) +
                  " url=" + entry.url.toString() +
                  " duration_sec=" + QString::number(entry.durationSec));
        schedulePageCycle(entry.durationSec);
    }

    void startPageCycle() {
        if (!pageListEnabled_ || pageList_.isEmpty()) {
            return;
        }
        loadPageEntry(0);
    }

    void advancePageCycle() {
        if (!pageListEnabled_ || pageList_.isEmpty()) {
            return;
        }
        loadPageEntry((currentPageIndex_ + 1) % pageList_.size());
    }

    void markUserActivity() {
        if (autoHomeMs_ > 0 && isOnline_) {
            autoHomeTimer_->start(autoHomeMs_);
            logDebug("activity restart-autohome ms=" + QString::number(autoHomeMs_));
        }
    }

    void showWaitingPage() {
        isOnline_ = false;
        successCount_ = 0;
        pageCycleActive_ = false;
        if (pageCycleTimer_) {
            pageCycleTimer_->stop();
        }
        view_->setHtml(QString::fromUtf8(WAITING_HTML), QUrl("about:blank"));
        if (urlBox_->isVisible()) {
            urlBox_->setText("about:blank");
        }
        autoHomeTimer_->stop();
        logNormal("state waiting success_count=0");
    }

    void loadHomepage() {
        isOnline_ = true;
        pageCycleActive_ = false;
        if (pageCycleTimer_) {
            pageCycleTimer_->stop();
        }
        view_->load(homepage_);
        logNormal("state homepage url=" + homepage_.toString());
        markUserActivity();
    }

    void recordProbeResult(bool ok, const QString &detail) {
        if (ok) {
            successCount_++;
            logNormal("probe success count=" + QString::number(successCount_) + " detail=" + detail);
            if (!isOnline_ && successCount_ >= 5) {
                if (pageListEnabled_) {
                    logNormal("probe threshold reached -> page cycle");
                    startPageCycle();
                } else {
                    logNormal("probe threshold reached -> homepage");
                    loadHomepage();
                }
            }
            return;
        }

        logNormal("probe failure detail=" + detail);
        successCount_ = 0;
        if (isOnline_) {
            logNormal("probe failure while online -> waiting");
            showWaitingPage();
        }
    }

    void runProbe() {
        logDebug("probe start type=" + checkType_ + " target=" + checkTarget_ + " port=" + QString::number(checkPort_));

        if (checkType_ == "icmp") {
            auto *process = new QProcess(this);
            connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                    [this, process](int exitCode, QProcess::ExitStatus status) {
                        const bool ok = status == QProcess::NormalExit && exitCode == 0;
                        recordProbeResult(ok, "icmp exit_code=" + QString::number(exitCode) + " status=" + QString::number(int(status)));
                        process->deleteLater();
                    });
            process->start("ping", {"-c", "1", "-W", "1", checkTarget_});
            return;
        }

        if (checkType_ == "tcp") {
            auto *socket = new QTcpSocket(this);
            auto *timeout = new QTimer(socket);
            timeout->setSingleShot(true);
            connect(timeout, &QTimer::timeout, this, [this, socket]() {
                socket->abort();
                recordProbeResult(false, "tcp timeout host=" + checkTarget_ + " port=" + QString::number(checkPort_));
                socket->deleteLater();
            });
            connect(socket, &QTcpSocket::connected, this, [this, socket, timeout]() {
                timeout->stop();
                socket->disconnectFromHost();
                recordProbeResult(true, "tcp connected host=" + checkTarget_ + " port=" + QString::number(checkPort_));
                socket->deleteLater();
            });
            connect(socket, &QTcpSocket::errorOccurred, this, [this, socket, timeout](QAbstractSocket::SocketError) {
                timeout->stop();
                recordProbeResult(false, "tcp error=" + socket->errorString());
                socket->deleteLater();
            });
            timeout->start(1500);
            socket->connectToHost(checkTarget_, checkPort_);
            return;
        }

        auto *manager = new QNetworkAccessManager(this);
        QNetworkRequest request{QUrl(checkTarget_)};
        request.setTransferTimeout(1500);
        auto *reply = manager->get(request);
        connect(reply, &QNetworkReply::finished, this, [this, manager, reply]() {
            const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool ok = reply->error() == QNetworkReply::NoError;
            recordProbeResult(ok,
                              "http status=" + QString::number(statusCode) +
                              " error=" + QString::number(int(reply->error())) +
                              " error_string=" + reply->errorString());
            reply->deleteLater();
            manager->deleteLater();
        });
    }

    QUrl homepage_;
    QString checkType_;
    QString checkTarget_;
    quint16 checkPort_;
    int autoHomeMs_ = 0;
    int toolbarHeight_ = 56;
    int successCount_ = 0;
    bool isOnline_ = false;
    bool disableContextMenu_ = false;
    bool pageListEnabled_ = false;
    bool pageCycleActive_ = false;
    int currentPageIndex_ = 0;
    QString pageListJson_;
    QList<PageEntry> pageList_;
    bool domainPolicyEnabled_ = false;
    QString domainPolicyJson_;
    QStringList allowRules_;
    QStringList denyRules_;
    QString logDir_;
    LogMode logMode_ = LogMode::Normal;
    QFile logFile_;
    QTextStream logStream_;
    DomainPolicyInterceptor *interceptor_ = nullptr;
    KioskPage *page_ = nullptr;
    QWebEngineView *view_ = nullptr;
    QPushButton *homeButton_ = nullptr;
    QPushButton *backButton_ = nullptr;
    QLineEdit *urlBox_ = nullptr;
    QPushButton *goButton_ = nullptr;
    QTimer *probeTimer_ = nullptr;
    QTimer *autoHomeTimer_ = nullptr;
    QTimer *pageCycleTimer_ = nullptr;
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("kiosk-browser-poc");

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption(QCommandLineOption(QStringList() << "homepage", "Homepage URL", "url", "https://example.com"));
    parser.addOption(QCommandLineOption(QStringList() << "check-type", "Probe type: icmp|tcp|http", "type", "http"));
    parser.addOption(QCommandLineOption(QStringList() << "check-target", "Probe target or URL", "target", "https://example.com"));
    parser.addOption(QCommandLineOption(QStringList() << "check-port", "TCP port", "port", "80"));
    parser.addOption(QCommandLineOption(QStringList() << "urlbox", "Show URL box"));
    parser.addOption(QCommandLineOption(QStringList() << "homebutton", "Show Home button"));
    parser.addOption(QCommandLineOption(QStringList() << "backbutton", "Show Back button"));
    parser.addOption(QCommandLineOption(QStringList() << "autohomesec", "Return home after N seconds", "seconds", "0"));
    parser.addOption(QCommandLineOption(QStringList() << "toolbar-height", "Toolbar height in pixels", "px", "56"));
    parser.addOption(QCommandLineOption(QStringList() << "disable-context-menu", "Disable right-click context menu"));
    parser.addOption(QCommandLineOption(QStringList() << "page-list-json", "Page list JSON path", "path", ""));
    parser.addOption(QCommandLineOption(QStringList() << "domain-policy-json", "Domain policy JSON path", "path", ""));
    parser.addOption(QCommandLineOption(QStringList() << "log-dir", "Log directory", "dir", QDir::homePath() + "/.local/log/make-rocky-bootable"));
    parser.addOption(QCommandLineOption(QStringList() << "log-mode", "Log mode: off|normal|debug", "mode", "normal"));
    parser.addOption(QCommandLineOption(QStringList() << "user-agent", "Override User-Agent string", "ua", ""));
    parser.process(app);

    const QUrl homepage = QUrl::fromUserInput(parser.value("homepage"));
    const QString checkType = parser.value("check-type").trimmed().toLower();
    const QString checkTarget = parser.value("check-target").trimmed();
    const quint16 checkPort = parser.value("check-port").toUShort();
    const int autoHomeSec = parser.value("autohomesec").toInt();
    const int toolbarHeight = parser.value("toolbar-height").toInt();
    const bool disableContextMenu = parser.isSet("disable-context-menu");
    const QString pageListJson = parser.value("page-list-json").trimmed();
    const QString domainPolicyJson = parser.value("domain-policy-json").trimmed();
    const QString logDir = parser.value("log-dir").trimmed();
    const LogMode logMode = parseLogMode(parser.value("log-mode"));
    const QString userAgent = parser.value("user-agent");
    const QString startupSummary =
        "homepage=" + homepage.toString() +
        " check_type=" + checkType +
        " check_target=" + checkTarget +
        " check_port=" + QString::number(checkPort) +
        " urlbox=" + boolString(parser.isSet("urlbox")) +
        " homebutton=" + boolString(parser.isSet("homebutton")) +
        " backbutton=" + boolString(parser.isSet("backbutton")) +
        " disable_context_menu=" + boolString(disableContextMenu) +
        " page_list_json=" + pageListJson +
        " domain_policy_json=" + domainPolicyJson +
        " user_agent=" + (userAgent.isEmpty() ? QString("<default>") : userAgent) +
        " toolbar_height=" + QString::number(toolbarHeight) +
        " autohomesec=" + QString::number(autoHomeSec);

    BrowserWindow window(homepage,
                         checkType,
                         checkTarget,
                         checkPort,
                         parser.isSet("urlbox"),
                         parser.isSet("homebutton"),
                         parser.isSet("backbutton"),
                         autoHomeSec,
                         toolbarHeight,
                         disableContextMenu,
                         pageListJson,
                         domainPolicyJson,
                         userAgent,
                         logDir,
                         logMode,
                         startupSummary);
    window.show();
    return app.exec();
}

void BrowserWindow::DomainPolicyInterceptor::interceptRequest(QWebEngineUrlRequestInfo &info) {
    const QString host = info.requestUrl().host().trimmed().toLower();
    if (host.isEmpty()) {
        return;
    }

    QString reason;
    const bool allowed = isAllowedHost(host, &reason);
    window_->logDomainDecision(info.requestUrl().toString(), host, allowed, reason);
    if (!allowed) {
        info.block(true);
    }
}

QWebEnginePage *BrowserWindow::KioskPage::createWindow(QWebEnginePage::WebWindowType type) {
    Q_UNUSED(type);
    if (!window_) {
        return nullptr;
    }

    class PopupRelayPage : public QWebEnginePage {
    public:
        explicit PopupRelayPage(BrowserWindow *window)
            : QWebEnginePage(window), window_(window) {
            connect(this, &QWebEnginePage::urlChanged, this, [this](const QUrl &url) {
                if (!window_ || url.isEmpty()) {
                    return;
                }
                window_->loadUrlInCurrentView(url, "popup redirect");
                deleteLater();
            });
        }

    protected:
        bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame) override {
            Q_UNUSED(type);
            Q_UNUSED(isMainFrame);
            if (window_) {
                window_->loadUrlInCurrentView(url, "popup navigation");
            }
            deleteLater();
            return false;
        }

    private:
        BrowserWindow *window_ = nullptr;
    };

    window_->logNormal("popup redirected to current view");
    return new PopupRelayPage(window_);
}

bool BrowserWindow::KioskPage::acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame) {
    Q_UNUSED(type);
    Q_UNUSED(isMainFrame);
    if (!window_) {
        return QWebEnginePage::acceptNavigationRequest(url, type, isMainFrame);
    }
    QString reason;
    if (!window_->isAllowedNavigationUrl(url, &reason)) {
        window_->logNormal("navigation blocked url=" + url.toString() + " reason=" + reason);
        return false;
    }
    return QWebEnginePage::acceptNavigationRequest(url, type, isMainFrame);
}
