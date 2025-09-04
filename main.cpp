#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QIcon>
#include "database/DatabaseManager.h"
#include "database/DatabaseInitializer.h"
#include "interlocking/InterlockingService.h"
#include "route/RouteAssignmentService.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    // Register only essential C++ types with QML
    qmlRegisterType<DatabaseManager>("RailFlux.Database", 1, 0, "DatabaseManager");
    qmlRegisterType<DatabaseInitializer>("RailFlux.Database", 1, 0, "DatabaseInitializer");
    qmlRegisterType<InterlockingService>("RailFlux.Interlocking", 1, 0, "InterlockingService");
    qmlRegisterUncreatableType<ValidationResult>("RailFlux.Interlocking", 1, 0, "validationResult",
                                                 "ValidationResult is returned from C++ functions");

    // Register only RouteAssignmentService
    qmlRegisterType<RailFlux::Route::RouteAssignmentService>("RailFlux.Route", 1, 0, "RouteAssignmentService");

    app.setWindowIcon(QIcon(":/resources/icons/railway-icon.ico"));
    qDebug() << "Icon exists" << QFile(":/icons/railway-icon.ico").exists();

    QQmlApplicationEngine engine;

    // Create only core services
    DatabaseManager* dbManager = new DatabaseManager(&app);
    DatabaseInitializer* dbInitializer = new DatabaseInitializer(&app);
    InterlockingService* interlockingService = new InterlockingService(dbManager, &app);

    using namespace RailFlux::Route;
    RouteAssignmentService* routeAssignmentService = new RouteAssignmentService(&app);

    // Minimal service composition - only DatabaseManager needed
    qDebug() << "Setting up RouteAssignmentService with minimal dependencies...";
    routeAssignmentService->setServices(
        dbManager
        );

    // Set only essential context properties for QML access
    engine.rootContext()->setContextProperty("globalDatabaseManager", dbManager);
    engine.rootContext()->setContextProperty("globalDatabaseInitializer", dbInitializer);
    engine.rootContext()->setContextProperty("globalInterlockingService", interlockingService);
    engine.rootContext()->setContextProperty("globalRouteAssignmentService", routeAssignmentService);

    dbManager->setInterlockingService(interlockingService);

    // Minimal database connection callback
    QObject::connect(dbManager, &DatabaseManager::connectionStateChanged,
                     [dbManager, interlockingService, routeAssignmentService](bool connected) {
                         if (connected) {
                             qDebug() << "Database connected, initializing services...";

                             // Initialize core services only
                             interlockingService->initialize();
                             routeAssignmentService->initialize();

                             // Basic health check
                             if (!routeAssignmentService->isOperational()) {
                                 qCritical() << "CRITICAL: RouteAssignmentService failed to initialize!";
                                 qDebug() << "Service Health Check:";
                                 qDebug() << "   DatabaseManager connected:" << (dbManager && dbManager->isConnected());
                                 qCritical() << "System will continue but route assignment will not be available";
                             } else {
                                 qDebug() << "RouteAssignmentService initialized successfully";
                             }
                         } else {
                             qWarning() << "Database disconnected, services may become non-operational";
                         }
                     });

    // Essential freeze signal monitoring
    QObject::connect(interlockingService, &InterlockingService::systemFreezeRequired,
                     [](const QString& trackSegmentId, const QString& reason, const QString& details) {
                         qCritical() << "FREEZE SIGNAL DETECTED";
                         qCritical() << "SYSTEM FREEZE ACTIVATED";
                         qCritical() << "Track Segment ID:" << trackSegmentId;
                         qCritical() << "Reason:" << reason;
                         qCritical() << "Details:" << details;
                         qCritical() << "Timestamp:" << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
                         qCritical() << "MANUAL INTERVENTION REQUIRED";
                     });

    // Basic route event logging (console only)
    QObject::connect(routeAssignmentService, &RouteAssignmentService::routeRequested,
                     [](const QString& requestId, const QString& sourceSignal, const QString& destSignal) {
                         qDebug() << "Route requested:" << requestId << "from" << sourceSignal << "to" << destSignal;
                     });

    QObject::connect(routeAssignmentService, &RouteAssignmentService::routeAssigned,
                     [](const QString& routeId, const QString& sourceSignal, const QString& destSignal, const QStringList& path) {
                         qDebug() << "Route assigned:" << routeId << "from" << sourceSignal << "to" << destSignal;
                         qDebug() << "   Path:" << path.join(" -> ");
                     });

    QObject::connect(routeAssignmentService, &RouteAssignmentService::routeFailed,
                     [](const QString& routeId, const QString& reason) {
                         qWarning() << "Route failed:" << routeId << "Reason:" << reason;
                     });

    // Basic performance monitoring
    QObject::connect(routeAssignmentService, &RouteAssignmentService::performanceWarning,
                     [](const QString& metric, double value, double threshold) {
                         qWarning() << "Route Performance Warning:" << metric << "=" << value << "(threshold:" << threshold << ")";
                     });

    // Cleanup on application exit
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [dbManager]() {
        qDebug() << "Application shutting down, cleaning up database...";
        dbManager->cleanup();
        dbManager->stopPolling();
    });

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.loadFromModule("RailFlux", "Main");

    // Start database connection
    qDebug() << "Connecting to database...";
    if (dbManager->connectToDatabase()) {
        qDebug() << "Database connection established";
        dbManager->startPolling();
        dbManager->enableRealTimeUpdates();
    } else {
        qWarning() << "Failed to connect to database - some features may not be available";
    }

    return app.exec();
}
