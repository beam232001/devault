// Copyright (c) 2011-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/bitcoingui.h>

#include <chainparams.h>
#include <config.h>
#include <httprpc.h>
#include <init.h>
#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <qt/clientmodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/intro.h>
#include <qt/networkstyle.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/splashscreen.h>
#include <qt/utilitydialog.h>
#include <qt/winshutdownmonitor.h>
#include <rpc/server.h>
#include <ui_interface.h>
#include <uint256.h>
#include <util.h>
#include <walletinitinterface.h>
#include <warnings.h>

#include <setpassphrasedialog.h>
#include <walletmodel.h>

#include <init.h>
#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <rpc/server.h>
#include <ui_interface.h>
#include <uint256.h>
#include <fs_util.h>
#include <util.h>
#include <warnings.h>

#include <wallet/wallet.h>
#include <walletinitinterface.h>

#include <cstdint>

#include <boost/filesystem/operations.hpp>
#include <boost/thread.hpp>

#include <QApplication>
#include <QDebug>
#include <QLibraryInfo>
#include <QLocale>
#include <QMessageBox>
#include <QSettings>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QTranslator>

#if defined(QT_STATICPLUGIN)
#include <QtPlugin>
#if defined(QT_QPA_PLATFORM_XCB)
Q_IMPORT_PLUGIN(QXcbIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_WINDOWS)
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_COCOA)
Q_IMPORT_PLUGIN(QCocoaIntegrationPlugin);
#endif
#endif

#include <cstdint>

// Declare meta types used for QMetaObject::invokeMethod
Q_DECLARE_METATYPE(bool *)
Q_DECLARE_METATYPE(Amount)
Q_DECLARE_METATYPE(uint256)

// Config is non-copyable so we can only register pointers to it
Q_DECLARE_METATYPE(Config *)

static void InitMessage(const std::string &message) {
    LogPrintf("init message: %s\n", message);
}

/**
 * Translate string to current locale using Qt.
 */
static std::string Translate(const char *psz) {
    return QCoreApplication::translate("DeVault", psz).toStdString();
}

static QString GetLangTerritory() {
    QSettings settings;
    // Get desired locale (e.g. "de_DE")
    // 1) System default language
    QString lang_territory = QLocale::system().name();
    // 2) Language from QSettings
    QString lang_territory_qsettings =
        settings.value("language", "").toString();
    if (!lang_territory_qsettings.isEmpty()) {
        lang_territory = lang_territory_qsettings;
    }
    // 3) -lang command line argument
    lang_territory = QString::fromStdString(
        gArgs.GetArg("-lang", lang_territory.toStdString()));
    return lang_territory;
}

/** Set up translations */
static void initTranslations(QTranslator &qtTranslatorBase,
                             QTranslator &qtTranslator,
                             QTranslator &translatorBase,
                             QTranslator &translator) {
    // Remove old translators
    QApplication::removeTranslator(&qtTranslatorBase);
    QApplication::removeTranslator(&qtTranslator);
    QApplication::removeTranslator(&translatorBase);
    QApplication::removeTranslator(&translator);

    // Get desired locale (e.g. "de_DE")
    // 1) System default language
    QString lang_territory = GetLangTerritory();

    // Convert to "de" only by truncating "_DE"
    QString lang = lang_territory;
    lang.truncate(lang_territory.lastIndexOf('_'));

    // Load language files for configured locale:
    // - First load the translator for the base language, without territory
    // - Then load the more specific locale translator

    // Load e.g. qt_de.qm
    if (qtTranslatorBase.load(
            "qt_" + lang,
            QLibraryInfo::location(QLibraryInfo::TranslationsPath))) {
        QApplication::installTranslator(&qtTranslatorBase);
    }

    // Load e.g. qt_de_DE.qm
    if (qtTranslator.load(
            "qt_" + lang_territory,
            QLibraryInfo::location(QLibraryInfo::TranslationsPath))) {
        QApplication::installTranslator(&qtTranslator);
    }

    // Load e.g. bitcoin_de.qm (shortcut "de" needs to be defined in
    // bitcoin.qrc)
    if (translatorBase.load(lang, ":/translations/")) {
        QApplication::installTranslator(&translatorBase);
    }

    // Load e.g. bitcoin_de_DE.qm (shortcut "de_DE" needs to be defined in
    // bitcoin.qrc)
    if (translator.load(lang_territory, ":/translations/")) {
        QApplication::installTranslator(&translator);
    }
}

/* qDebug() message handler --> debug.log */
void DebugMessageHandler(QtMsgType type, const QMessageLogContext &context,
                         const QString &msg) {
    Q_UNUSED(context);
    if (type == QtDebugMsg) {
        LogPrint(BCLog::QT, "GUI: %s\n", msg.toStdString());
    } else {
        LogPrintf("GUI: %s\n", msg.toStdString());
    }
}

/**
 * Class encapsulating DeVault startup and shutdown.
 * Allows running startup and shutdown in a different thread from the UI thread.
 */
class DeVault : public QObject {
    Q_OBJECT
public:
    explicit DeVault(interfaces::Node& node, SecureString& strWalletPassphrase,
                     std::vector<std::string>& wordlist);

    /**
     * Basic initialization, before starting initialization/shutdown thread.
     * Return true on success.
     */
    static bool baseInitialize(Config &config, RPCServer &rpcServer);

public Q_SLOTS:
    void initialize(Config *config, RPCServer *rpcServer,
                    HTTPRPCRequestProcessor *httpRPCRequestProcessor);
    void shutdown();

Q_SIGNALS:
    void initializeResult(bool success);
    void shutdownResult();
    void runawayException(const QString &message);

private:
    /// Pass fatal exception message to UI thread
    void handleRunawayException(const std::exception *e);
    SecureString walletPassphrase;
    // TODO: Set this for it for be used
    std::vector<std::string> words;
    interfaces::Node &m_node;
};

/** Main Bitcoin application object */
class BitcoinApplication : public QApplication {
    Q_OBJECT
public:
    explicit BitcoinApplication(interfaces::Node &node, int &argc, char **argv);
    ~BitcoinApplication();

    void initPlatformStyle();
    /// parameter interaction/setup based on rules
    void parameterSetup();
    /// Create options model
    void createOptionsModel(bool resetSettings);
    /// Create main window
    bool createWindow(const Config *, const NetworkStyle *networkStyle);
    /// Create splash screen
    void createSplashScreen(const NetworkStyle *networkStyle);
    /// Get wallet password from user for Wallet Encryption or return true if wallet pre-exists
    bool setupPassword(SecureString& password);

    /// Request core initialization
    void requestInitialize(Config &config, RPCServer &rpcServer,
                           HTTPRPCRequestProcessor &httpRPCRequestProcessor);
    /// Request core shutdown
    void requestShutdown(Config &config);

    /// Get process return value
    int getReturnValue() const { return returnValue; }

    /// Get window identifier of QMainWindow (BitcoinGUI)
    WId getMainWinId() const;

public Q_SLOTS:
    void initializeResult(bool success);
    void shutdownResult();
    /// Handle runaway exceptions. Shows a message box with the problem and
    /// quits the program.
    void handleRunawayException(const QString &message);

Q_SIGNALS:
    void requestedInitialize(Config *config, RPCServer *rpcServer,
                             HTTPRPCRequestProcessor *httpRPCRequestProcessor);
    void requestedShutdown();
    void stopThread();
    void splashFinished(QWidget *window);

private:
    QThread *coreThread;
    interfaces::Node &m_node;
    OptionsModel *optionsModel;
    ClientModel *clientModel;
    BitcoinGUI *window;
    QTimer *pollShutdownTimer;
#ifdef ENABLE_WALLET
    std::vector<WalletModel *> m_wallet_models;
#endif
    SecureString pss;
    std::vector<std::string> wordlist;
    int returnValue;
    const PlatformStyle *platformStyle;
    std::unique_ptr<QWidget> shutdownWindow;

    void startThread();
};

#include <bitcoin.moc>

DeVault::DeVault(interfaces::Node &node, SecureString& strWalletPassphrase, std::vector<std::string>& wordlist)
    : QObject(), walletPassphrase(strWalletPassphrase), words(wordlist), m_node(node) {}

void DeVault::handleRunawayException(const std::exception *e) {
    PrintExceptionContinue(e, "Runaway exception");
          Q_EMIT runawayException(QString::fromStdString(m_node.getWarnings("gui")));
}

bool DeVault::baseInitialize(Config &config, RPCServer &rpcServer) {
    if (!AppInitBasicSetup()) {
        return false;
    }
    if (!AppInitParameterInteraction(config)) {
        return false;
    }
    if (!AppInitSanityChecks()) {
        return false;
    }
    if (!AppInitLockDataDirectory()) {
        return false;
    }
    return true;
}

void DeVault::initialize(Config *config, RPCServer *rpcServer,
                            HTTPRPCRequestProcessor *httpRPCRequestProcessor) {
    try {
        qDebug() << __func__ << ": Running initialization in thread";
        bool rv = m_node.appInitMain(*config, *rpcServer, *httpRPCRequestProcessor, walletPassphrase, words);
        walletPassphrase.clear();
        Q_EMIT initializeResult(rv);
    } catch (const std::exception &e) {
        handleRunawayException(&e);
    } catch (...) {
        handleRunawayException(nullptr);
    }
}

void DeVault::shutdown() {
    try {
        qDebug() << __func__ << ": Running Shutdown in thread";
        m_node.appShutdown();
        qDebug() << __func__ << ": Shutdown finished";
        Q_EMIT shutdownResult();
    } catch (const std::exception &e) {
        handleRunawayException(&e);
    } catch (...) {
        handleRunawayException(nullptr);
    }
}

BitcoinApplication::BitcoinApplication(interfaces::Node& node, int &argc, char **argv)
    : QApplication(argc, argv), coreThread(nullptr), m_node(node),optionsModel(nullptr), clientModel(nullptr),
      window(nullptr), pollShutdownTimer(nullptr), 
#ifdef ENABLE_WALLET
      m_wallet_models(),
#endif
      returnValue(0) {
    setQuitOnLastWindowClosed(false);

    // UI per-platform customization.
    // This must be done inside the BitcoinApplication constructor, or after it,
    // because PlatformStyle::instantiate requires a QApplication.

}

BitcoinApplication::~BitcoinApplication() {
    if (coreThread) {
        qDebug() << __func__ << ": Stopping thread";
        Q_EMIT stopThread();
        coreThread->wait();
        qDebug() << __func__ << ": Stopped thread";
    }

    delete window;
    window = nullptr;
    delete optionsModel;
    optionsModel = nullptr;
    delete platformStyle;
    platformStyle = nullptr;
}

void BitcoinApplication::initPlatformStyle() 
{ 
    std::string platformName; 
    platformName = gArgs.GetArg("-uiplatform", BitcoinGUI::DEFAULT_UIPLATFORM); 
    platformStyle = PlatformStyle::instantiate(QString::fromStdString(platformName)); 
    if (!platformStyle) // Fall back to "other" if specified name not found 
        platformStyle = PlatformStyle::instantiate("other"); 
    assert(platformStyle); 
} 

void BitcoinApplication::createOptionsModel(bool resetSettings) {
    optionsModel = new OptionsModel(m_node, nullptr, resetSettings);
}


bool BitcoinApplication::setupPassword(SecureString& password) {
  if (gArgs.GetBoolArg("-disablewallet", DEFAULT_DISABLE_WALLET)) {
    LogPrintf("Wallet disabled!\n");
  } else {
    bool is_multiwallet = gArgs.GetArgs("-wallet").size() > 1;
    if (is_multiwallet) {
      return InitError(
                       strprintf("%s is only allowed with a single wallet file",
                                 "-wallet"));
    }
    // If we get here, only single wallet file
    for (const std::string &walletFile : gArgs.GetArgs("-wallet")) {
      if (fs::exists(walletFile)) return true;
    }
  }
  if (CheckIfWalletDatExists()) return true;
  
  SetPassphraseDialog dlg(nullptr);
  dlg.exec();
  password = dlg.getPassword();
  return false;
}

bool BitcoinApplication::createWindow(const Config *config,
                                      const NetworkStyle *networkStyle) {

  if (g_wallet_init_interface.HasWalletSupport()) {
    if (!setupPassword(pss)) {
        if (pss.empty()) return false;
    }
  }
  window = new BitcoinGUI(m_node, config, platformStyle, networkStyle, nullptr);

  pollShutdownTimer = new QTimer(window);
  connect(pollShutdownTimer, SIGNAL(timeout()), window,
          SLOT(detectShutdown()));
  return true;
}

void BitcoinApplication::createSplashScreen(const NetworkStyle *networkStyle) {
    SplashScreen *splash = new SplashScreen(m_node, nullptr, networkStyle);
    // We don't hold a direct pointer to the splash screen after creation, but
    // the splash screen will take care of deleting itself when slotFinish
    // happens.
    splash->show();
    connect(this, SIGNAL(splashFinished(QWidget *)), splash,
            SLOT(slotFinish(QWidget *)));
    connect(this, SIGNAL(requestedShutdown()), splash, SLOT(close()));
}

void BitcoinApplication::startThread() {
    if (coreThread) {
        return;
    }
    coreThread = new QThread(this);
    DeVault *executor = new DeVault(m_node, pss, wordlist);
    executor->moveToThread(coreThread);

    /*  communication to and from thread */
    connect(executor, SIGNAL(initializeResult(bool)), this,
            SLOT(initializeResult(bool)));
    connect(executor, SIGNAL(shutdownResult()), this, SLOT(shutdownResult()));
    connect(executor, SIGNAL(runawayException(QString)), this,
            SLOT(handleRunawayException(QString)));

    // Note on how Qt works: it tries to directly invoke methods if the signal
    // is emitted on the same thread that the target object 'lives' on.
    // But if the target object 'lives' on another thread (executor here does)
    // the SLOT will be invoked asynchronously at a later time in the thread
    // of the target object.  So.. we pass a pointer around.  If you pass
    // a reference around (even if it's non-const) you'll get Qt generating
    // code to copy-construct the parameter in question (Q_DECLARE_METATYPE
    // and qRegisterMetaType generate this code).  For the Config class,
    // which is noncopyable, we can't do this.  So.. we have to pass
    // pointers to Config around.  Make sure Config &/Config * isn't a
    // temporary (eg it lives somewhere aside from the stack) or this will
    // crash because initialize() gets executed in another thread at some
    // unspecified time (after) requestedInitialize() is emitted!
    connect(this,
            SIGNAL(requestedInitialize(Config *, RPCServer *, HTTPRPCRequestProcessor *)),
            executor, SLOT(initialize(Config *, RPCServer *, HTTPRPCRequestProcessor *)));

    connect(this, SIGNAL(requestedShutdown()), executor, SLOT(shutdown()));
    /*  make sure executor object is deleted in its own thread */
    connect(this, SIGNAL(stopThread()), executor, SLOT(deleteLater()));
    connect(this, SIGNAL(stopThread()), coreThread, SLOT(quit()));

    coreThread->start();
}

void BitcoinApplication::parameterSetup() {
  m_node.initLogging();
  m_node.initParameterInteraction();
}

void BitcoinApplication::requestInitialize(
                                           Config &config, RPCServer &rpcServer, HTTPRPCRequestProcessor &httpRPCRequestProcessor) {
    qDebug() << __func__ << ": Requesting initialize";
    startThread();
    // IMPORTANT: config must NOT be a reference to a temporary because below
    // signal may be connected to a slot that will be executed as a queued
    // connection in another thread!
    Q_EMIT requestedInitialize(&config, &rpcServer, &httpRPCRequestProcessor);
}

void BitcoinApplication::requestShutdown(Config &config) {
    // Show a simple window indicating shutdown status. Do this first as some of
    // the steps may take some time below, for example the RPC console may still
    // be executing a command.
    shutdownWindow.reset(ShutdownWindow::showShutdownWindow(window));

    qDebug() << __func__ << ": Requesting shutdown";
    startThread();
    window->hide();
    window->setClientModel(nullptr);
    pollShutdownTimer->stop();

#ifdef ENABLE_WALLET
    window->removeAllWallets();
    for (WalletModel *walletModel : m_wallet_models) {
        delete walletModel;
    }
    m_wallet_models.clear();
#endif
    delete clientModel;
    clientModel = nullptr;

    m_node.startShutdown();

    // Request shutdown from core thread
    Q_EMIT requestedShutdown();
}

void BitcoinApplication::initializeResult(bool success) {
    qDebug() << __func__ << ": Initialization result: " << success;
    returnValue = success ? EXIT_SUCCESS : EXIT_FAILURE;
    if (!success) {
        // Make sure splash screen doesn't stick around during shutdown.
        Q_EMIT splashFinished(window);
        // Exit first main loop invocation.
        quit();
        return;
    }
    // Log this only after AppInitMain finishes, as then logging setup is
    // guaranteed complete.
    qWarning() << "Platform customization:" << platformStyle->getName();
    clientModel = new ClientModel(m_node, optionsModel);
    window->setClientModel(clientModel);

#ifdef ENABLE_WALLET
    bool fFirstWallet = true;
    auto wallets = m_node.getWallets();
    for (auto &wallet : wallets) {
        WalletModel *const walletModel = new WalletModel(
            std::move(wallet), m_node, platformStyle, optionsModel);

        window->addWallet(walletModel);
        if (fFirstWallet) {
            window->setCurrentWallet(walletModel->getWalletName());
            fFirstWallet = false;
        }

    //    connect(walletModel,
   //             SIGNAL(coinsSent(CWallet *, SendCoinsRecipient, QByteArray)));
     //           SLOT(fetchPaymentACK(CWallet *, const SendCoinsRecipient &,
     //                                QByteArray)));

        m_wallet_models.push_back(walletModel);
    }
#endif

    // If -min option passed, start window minimized.
    if (gArgs.GetBoolArg("-min", false)) {
        window->showMinimized();
    } else {
        window->show();
    }
    Q_EMIT splashFinished(window);

#ifdef ENABLE_WALLET
    // Now that initialization/startup is done, process any command-line
    // bitcoincash: URIs or payment requests:
 //   connect(window, SIGNAL(receivedURI(QString)), 
 //           SLOT(handleURIOrFile(QString)));
#endif

    pollShutdownTimer->start(200);
}

void BitcoinApplication::shutdownResult() {
    // Exit second main loop invocation after shutdown finished.
    quit();
}

void BitcoinApplication::handleRunawayException(const QString &message) {
    QMessageBox::critical(
        nullptr, "Runaway exception",
        BitcoinGUI::tr("A fatal error occurred. Devault can no longer continue "
                       "safely and will quit.") +
            QString("\n\n") + message);
    ::exit(EXIT_FAILURE);
}

WId BitcoinApplication::getMainWinId() const {
    if (!window) {
        return 0;
    }

    return window->winId();
}

static void SetupUIArgs() {
    gArgs.AddArg(
        "-choosedatadir",
        strprintf(QObject::tr("Choose data directory on startup (default: %d)")
                      .toStdString(),
                  DEFAULT_CHOOSE_DATADIR),
        false, OptionsCategory::GUI);
    gArgs.AddArg(
        "-lang=<lang>",
        QObject::tr(
            "Set language, for example \"de_DE\" (default: system locale)")
            .toStdString(),
        false, OptionsCategory::GUI);
    gArgs.AddArg("-min", QObject::tr("Start minimized").toStdString(), false,
                 OptionsCategory::GUI);
    gArgs.AddArg(
        "-rootcertificates=<file>",
        QObject::tr(
            "Set SSL root certificates for payment request (default: -system-)")
            .toStdString(),
        false, OptionsCategory::GUI);
    gArgs.AddArg(
        "-splash",
        strprintf(QObject::tr("Show splash screen on startup (default: %d)")
                      .toStdString(),
                  DEFAULT_SPLASHSCREEN),
        false, OptionsCategory::GUI);
    gArgs.AddArg(
        "-resetguisettings",
        QObject::tr("Reset all settings changed in the GUI").toStdString(),
        false, OptionsCategory::GUI);
    gArgs.AddArg("-uiplatform",
                 strprintf("Select platform to customize UI for (one of "
                           "windows, macosx, other; default: %s)",
                           BitcoinGUI::DEFAULT_UIPLATFORM),
                 true, OptionsCategory::GUI);
}

#ifndef BITCOIN_QT_TEST

static void MigrateSettings() {
    assert(!QApplication::applicationName().isEmpty());

    static const QString legacyAppName("Devault-Core"),
#ifdef Q_OS_DARWIN
        // Macs and/or iOS et al use a domain-style name for Settings
        // files. All other platforms use a simple orgname. This
        // difference is documented in the QSettings class documentation.
        legacyOrg("devault.cc");
#else
        legacyOrg("Devault-Core");
#endif
    QSettings
        // below picks up settings file location based on orgname,appname
        legacy(legacyOrg, legacyAppName),
        // default c'tor below picks up settings file location based on
        // QApplication::applicationName(), et al -- which was already set
        // in main()
        abc;
#ifdef Q_OS_DARWIN
    // Disable bogus OSX keys from MacOS system-wide prefs that may cloud our
    // judgement ;) (this behavior is also documented in QSettings docs)
    legacy.setFallbacksEnabled(false);
    abc.setFallbacksEnabled(false);
#endif
    const QStringList legacyKeys(legacy.allKeys());

    // We only migrate settings if we have Core settings but no Bitcoin-ABC
    // settings
    if (!legacyKeys.isEmpty() && abc.allKeys().isEmpty()) {
        for (const QString &key : legacyKeys) {
            // now, copy settings over
            abc.setValue(key, legacy.value(key));
        }
    }
}

int main(int argc, char *argv[]) {
    SetupEnvironment();

    std::unique_ptr<interfaces::Node> node = interfaces::MakeNode();

    /// 1. Parse command-line options. These take precedence over anything else.
    // Command-line options take precedence:
    node->setupServerArgs();
    SetupUIArgs();
    node->parseParameters(argc, argv);

    // Do not refer to data directory yet, this can be overridden by
    // Intro::pickDataDirectory

    /// 2. Basic Qt initialization (not dependent on parameters or
    /// configuration)
    Q_INIT_RESOURCE(bitcoin);
    Q_INIT_RESOURCE(bitcoin_locale);

    BitcoinApplication app(*node, argc, argv);
    // Generate high-dpi pixmaps
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#ifdef Q_OS_MAC
    QApplication::setAttribute(Qt::AA_DontShowIconsInMenus);
#endif

    // Register meta types used for QMetaObject::invokeMethod
    qRegisterMetaType<bool *>();
    //   Need to pass name here as Amount is a typedef (see
    //   http://qt-project.org/doc/qt-5/qmetatype.html#qRegisterMetaType)
    //   IMPORTANT if it is no longer a typedef use the normal variant above
    qRegisterMetaType<Amount>("Amount");
    qRegisterMetaType<std::function<void(void)>>("std::function<void(void)>");

    // Need to register any types Qt doesn't know about if you intend
    // to use them with the signal/slot mechanism Qt provides. Even pointers.
    // Note that class Config is noncopyable and so we can't register a
    // non-pointer version of it with Qt, because Qt expects to be able to
    // copy-construct non-pointers to objects for invoking slots
    // behind-the-scenes in the 'Queued' connection case.
    qRegisterMetaType<Config *>();

    /// 3. Application identification
    // must be set before OptionsModel is initialized or translations are
    // loaded, as it is used to locate QSettings.
    // Note: If you move these calls somewhere else, be sure to bring
    // MigrateSettings() below along for the ride.
    QApplication::setOrganizationName(QAPP_ORG_NAME);
    QApplication::setOrganizationDomain(QAPP_ORG_DOMAIN);
    QApplication::setApplicationName(QAPP_APP_NAME_DEFAULT);
    // Migrate settings from core's/our old GUI settings to DeVault
    // only if core's exist but DeVault's doesn't.
    // NOTE -- this function needs to be called *after* the above 3 lines
    // that set the app orgname and app name! If you move the above 3 lines
    // to elsewhere, take this call with you!
    MigrateSettings();
    GUIUtil::SubstituteFonts(GetLangTerritory());

    /// 4. Initialization of translations, so that intro dialog is in user's
    /// language. Now that QSettings are accessible, initialize translations.
    QTranslator qtTranslatorBase, qtTranslator, translatorBase, translator;
    initTranslations(qtTranslatorBase, qtTranslator, translatorBase,
                     translator);
    translationInterface.Translate.connect(Translate);

    // Show help message immediately after parsing command-line options (for
    // "-lang") and setting locale, but before showing splash screen.
    if (HelpRequested(gArgs) || gArgs.IsArgSet("-version")) {
        HelpMessageDialog help(*node, nullptr, gArgs.IsArgSet("-version"));
        help.showOrPrint();
        return EXIT_SUCCESS;
    }

    /// 4.5 Settings available --> init Platformstyle 
    app.initPlatformStyle(); 

    /// 5. Now that settings and translations are available, ask user for data
    /// directory. User language is set up: pick a data directory.
    if (!Intro::pickDataDirectory(*node)) {
        return EXIT_SUCCESS;
    }

    /// 6. Determine availability of data and blocks directory and parse
    /// bitcoin.conf
    /// - Do not call GetDataDir(true) before this step finishes.
    if (!fs::is_directory(GetDataDir(false))) {
        QMessageBox::critical(
            0, QObject::tr(PACKAGE_NAME),
            QObject::tr(
                "Error: Specified data directory \"%1\" does not exist.")
                .arg(QString::fromStdString(gArgs.GetArg("-datadir", ""))));
        return EXIT_FAILURE;
    }
    try {
        node->readConfigFile(gArgs.GetArg("-conf", BITCOIN_CONF_FILENAME));
    } catch (const std::exception &e) {
        QMessageBox::critical(
            nullptr, QObject::tr(PACKAGE_NAME),
            QObject::tr("Error: Cannot parse configuration file: %1. Only use "
                        "key=value syntax.")
                .arg(e.what()));
        return EXIT_FAILURE;
    }

    /// 7. Determine network (and switch to network specific options)
    // - Do not call Params() before this step.
    // - Do this after parsing the configuration file, as the network can be
    // switched there.
    // - QSettings() will use the new application name after this, resulting in
    // network-specific settings.
    // - Needs to be done before createOptionsModel.

    // Check for -testnet or -regtest parameter (Params() calls are only valid
    // after this clause)
    try {
        node->selectParams(gArgs.GetChainName());
    } catch (std::exception &e) {
        QMessageBox::critical(nullptr, QObject::tr(PACKAGE_NAME),
                              QObject::tr("Error: %1").arg(e.what()));
        return EXIT_FAILURE;
    }
    QScopedPointer<const NetworkStyle> networkStyle(NetworkStyle::instantiate(
        QString::fromStdString(Params().NetworkIDString())));
    assert(!networkStyle.isNull());
    // Allow for separate UI settings for testnets
    QApplication::setApplicationName(networkStyle->getAppName());
    // Re-initialize translations after changing application name (language in
    // network-specific settings can be different)
    initTranslations(qtTranslatorBase, qtTranslator, translatorBase,
                     translator);

    /// 9. Main GUI initialization
    // Install global event filter that makes sure that long tooltips can be
    // word-wrapped.
    app.installEventFilter(
        new GUIUtil::ToolTipToRichTextFilter(TOOLTIP_WRAP_THRESHOLD, &app));
#if defined(Q_OS_WIN)
    // Install global event filter for processing Windows session related
    // Windows messages (WM_QUERYENDSESSION and WM_ENDSESSION)
    qApp->installNativeEventFilter(new WinShutdownMonitor());
#endif
    // Install qDebug() message handler to route to debug.log
    qInstallMessageHandler(DebugMessageHandler);
    // Allow parameter interaction before we create the options model
    app.parameterSetup();
    // Load GUI settings from QSettings
    app.createOptionsModel(gArgs.GetBoolArg("-resetguisettings", false));

    // Subscribe to global signals from core
    std::unique_ptr<interfaces::Handler> handler = node->handleInitMessage(InitMessage);

    // Get global config
    Config &config = const_cast<Config &>(GetConfig());

    if (gArgs.GetBoolArg("-splash", DEFAULT_SPLASHSCREEN) &&
        !gArgs.GetBoolArg("-min", false)) {
        app.createSplashScreen(networkStyle.data());
    }

    RPCServer rpcServer;
    HTTPRPCRequestProcessor httpRPCRequestProcessor(config, rpcServer);

    try {
        if (!app.createWindow(&config, networkStyle.data())) {
            return EXIT_FAILURE;
        }
        // Perform base initialization before spinning up
        // initialization/shutdown thread. This is acceptable because this
        // function only contains steps that are quick to execute, so the GUI
        // thread won't be held up.
        if (!node->baseInitialize(config)) {
            // A dialog with detailed error will have been shown by InitError()
            return EXIT_FAILURE;
        }
        app.requestInitialize(config, rpcServer, httpRPCRequestProcessor);
#if defined(Q_OS_WIN)
        WinShutdownMonitor::registerShutdownBlockReason(
            QObject::tr("%1 didn't yet exit safely...")
                .arg(QObject::tr(PACKAGE_NAME)),
            (HWND)app.getMainWinId());
#endif
        app.exec();
        app.requestShutdown(config);
        app.exec();
        return app.getReturnValue();
    } catch (const std::exception &e) {
        PrintExceptionContinue(&e, "Runaway exception");
        app.handleRunawayException(QString::fromStdString(node->getWarnings("gui")));
    } catch (...) {
        PrintExceptionContinue(nullptr, "Runaway exception");
        app.handleRunawayException(QString::fromStdString(node->getWarnings("gui")));
    }
    return EXIT_FAILURE;
}
#endif // BITCOIN_QT_TEST
