#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QIcon>
#include "database/DatabaseManager.h"
#include "database/DatabaseInitializer.h"
#include "interlocking/InterlockingService.h"
#include "route/RouteAssignmentService.h"
#include "route/GraphService.h"
#include "route/ResourceLockService.h"
#include "route/OverlapService.h"
#include "route/TelemetryService.h"
#include "route/VitalRouteController.h"
#include "route/SafetyMonitorService.h"
#include "interlocking/AspectPropagationService.h"
#include "interlocking/InterlockingRuleEngine.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    // Register C++ types with QML
    qmlRegisterType<DatabaseManager>("RailFlux.Database", 1, 0, "DatabaseManager");
    qmlRegisterType<DatabaseInitializer>("RailFlux.Database", 1, 0, "DatabaseInitializer");
    qmlRegisterType<InterlockingService>("RailFlux.Interlocking", 1, 0, "InterlockingService");
    //   CORRECT for Qt 6 Q_GADGET types
    qmlRegisterUncreatableType<ValidationResult>("RailFlux.Interlocking", 1, 0, "validationResult",
                                                 "ValidationResult is returned from C++ functions");

    // Register Route Assignment services with QML
    qmlRegisterType<RailFlux::Route::RouteAssignmentService>("RailFlux.Route", 1, 0, "RouteAssignmentService");
    qmlRegisterType<RailFlux::Route::GraphService>("RailFlux.Route", 1, 0, "GraphService");
    qmlRegisterType<RailFlux::Route::ResourceLockService>("RailFlux.Route", 1, 0, "ResourceLockService");
    qmlRegisterType<RailFlux::Route::OverlapService>("RailFlux.Route", 1, 0, "OverlapService");
    qmlRegisterType<RailFlux::Route::TelemetryService>("RailFlux.Route", 1, 0, "TelemetryService");
    qmlRegisterType<RailFlux::Route::VitalRouteController>("RailFlux.Route", 1, 0, "VitalRouteController");
    qmlRegisterType<RailFlux::Route::SafetyMonitorService>("RailFlux.Route", 1, 0, "SafetyMonitorService");
    qmlRegisterType<RailFlux::Interlocking::AspectPropagationService>("RailFlux.Interlocking", 1, 0, "AspectPropagationService");

    app.setWindowIcon(QIcon(":/resources/icons/railway-icon.ico"));
    qDebug() << "Icon exists" << QFile(":/icons/railway-icon.ico").exists();

    QQmlApplicationEngine engine;

    // Create global instances
    DatabaseManager* dbManager = new DatabaseManager(&app);
    DatabaseInitializer* dbInitializer = new DatabaseInitializer(&app);
    InterlockingService* interlockingService = new InterlockingService(dbManager, &app);

    //   FIX: Get the rule engine from InterlockingService (which already has rules loaded)
    // Instead of creating a new one
    RailFlux::Interlocking::AspectPropagationService* aspectPropagationService =
        new RailFlux::Interlocking::AspectPropagationService(dbManager, interlockingService->getRuleEngine(), &app);

    // Create Route Assignment service hierarchy
    using namespace RailFlux::Route;

    // Layer 2: Domain Services
    GraphService* graphService = new GraphService(dbManager, &app);
    ResourceLockService* resourceLockService = new ResourceLockService(dbManager, &app);
    OverlapService* overlapService = new OverlapService(dbManager, &app);
    TelemetryService* telemetryService = new TelemetryService(dbManager, &app);

    // Layer 3: Route Management Services
    VitalRouteController* vitalRouteController = new VitalRouteController(dbManager, interlockingService, resourceLockService, telemetryService, &app);
    RouteAssignmentService* routeAssignmentService = new RouteAssignmentService(&app);
    SafetyMonitorService* safetyMonitorService = new SafetyMonitorService(dbManager, telemetryService, &app);

    //   CRITICAL FIX: Compose services using the service composition pattern
    qDebug() << " Composing RouteAssignmentService dependencies...";
    routeAssignmentService->setServices(
        dbManager,
        graphService,
        resourceLockService,
        overlapService,
        telemetryService,
        vitalRouteController
        );

    //   NEW: Connect AspectPropagationService to VitalRouteController
    qDebug() << " Connecting AspectPropagationService to VitalRouteController...";
    vitalRouteController->setAspectPropagationService(aspectPropagationService);

    // Set context properties for QML access
    engine.rootContext()->setContextProperty("globalDatabaseManager", dbManager);
    engine.rootContext()->setContextProperty("globalDatabaseInitializer", dbInitializer);
    engine.rootContext()->setContextProperty("globalInterlockingService", interlockingService);
    engine.rootContext()->setContextProperty("globalRouteAssignmentService", routeAssignmentService);
    engine.rootContext()->setContextProperty("globalGraphService", graphService);
    engine.rootContext()->setContextProperty("globalResourceLockService", resourceLockService);
    engine.rootContext()->setContextProperty("globalOverlapService", overlapService);
    engine.rootContext()->setContextProperty("globalTelemetryService", telemetryService);
    engine.rootContext()->setContextProperty("globalVitalRouteController", vitalRouteController);
    engine.rootContext()->setContextProperty("globalSafetyMonitorService", safetyMonitorService);
    engine.rootContext()->setContextProperty("globalAspectPropagationService", aspectPropagationService);

    dbManager->setInterlockingService(interlockingService);

    //    Database connection callback with proper service initialization order
    QObject::connect(dbManager, &DatabaseManager::connectionStateChanged,
                     [dbManager, interlockingService, routeAssignmentService, telemetryService, safetyMonitorService, graphService, resourceLockService, overlapService, vitalRouteController, aspectPropagationService](bool connected) {
                         if (connected) {
                             qDebug() << " Database connected, initializing services...";

                             // Initialize services in proper dependency order
                             interlockingService->initialize();
                             telemetryService->initialize();
                             safetyMonitorService->initialize();

                             //   NEW: Initialize AspectPropagationService
                             qDebug() << " Initializing AspectPropagationService...";
                             aspectPropagationService->initialize();

                             //   MOVED: RouteAssignmentService initialization AFTER database connection
                             qDebug() << " Initializing RouteAssignmentService...";
                             routeAssignmentService->initialize();

                             // Verify operational state
                             if (!routeAssignmentService->isOperational()) {
                                 qCritical() << " CRITICAL: RouteAssignmentService failed to initialize!";

                                 // Debugging: Check individual service health
                                 qDebug() << " Service Health Check:";
                                 qDebug() << "   DatabaseManager connected:" << (dbManager && dbManager->isConnected());
                                 qDebug() << "   GraphService loaded:" << (graphService && graphService->isLoaded());
                                 qDebug() << "   ResourceLockService operational:" << (resourceLockService && resourceLockService->isOperational());
                                 qDebug() << "   OverlapService operational:" << (overlapService && overlapService->isOperational());
                                 qDebug() << "   TelemetryService operational:" << (telemetryService && telemetryService->isOperational());
                                 qDebug() << "   VitalController operational:" << (vitalRouteController && vitalRouteController->isOperational());

                                 qCritical() << " System will continue but route assignment will not be available";
                             } else {
                                 qDebug() << "  RouteAssignmentService initialized successfully";
                             }
                         } else {
                             qWarning() << " Database disconnected, services may become non-operational";
                         }
                     });

    //   NEW: Connect freeze signal for safety system monitoring
    QObject::connect(interlockingService, &InterlockingService::systemFreezeRequired,
                     [](const QString& trackSegmentId, const QString& reason, const QString& details) {
                         qCritical() << " FREEZE SIGNAL DETECTED IN MAIN.CPP ";
                         qCritical() << " SYSTEM FREEZE ACTIVATED ";
                         qCritical() << "Track Segment SEGMENT ID:" << trackSegmentId;
                         qCritical() << "Reason:" << reason;
                         qCritical() << "Details:" << details;
                         qCritical() << "Timestamp:" << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
                         qCritical() << " MANUAL INTERVENTION REQUIRED ";
                         qCritical() << " END FREEZE SIGNAL ";
                     });

    //
    // ROUTE ASSIGNMENT SERVICE SIGNAL/SLOT CONNECTIONS
    //

    // A. Route Service to Telemetry connections
    QObject::connect(routeAssignmentService, &RouteAssignmentService::routeRequested,
                     telemetryService, [telemetryService](const QString& requestId, const QString& sourceSignal, const QString& destSignal) {
                         telemetryService->recordRouteEvent(requestId, "ROUTE_REQUESTED", QVariantMap{
                                                                                              {"sourceSignal", sourceSignal},
                                                                                              {"destSignal", destSignal}
                                                                                          });
                     });

    QObject::connect(routeAssignmentService, &RouteAssignmentService::routeAssigned,
                     telemetryService, [telemetryService](const QString& routeId, const QString& sourceSignal, const QString& destSignal, const QStringList& path) {
                         telemetryService->recordRouteEvent(routeId, "ROUTE_RESERVED", QVariantMap{
                                                                                           {"sourceSignal", sourceSignal},
                                                                                           {"destSignal", destSignal},
                                                                                           {"pathLength", path.size()},
                                                                                           {"path", path}
                                                                                       });
                     });

    QObject::connect(routeAssignmentService, &RouteAssignmentService::routeActivated,
                     telemetryService, [telemetryService](const QString& routeId) {
                         telemetryService->recordRouteEvent(routeId, "ROUTE_ACTIVATED", QVariantMap{});
                     });

    QObject::connect(routeAssignmentService, &RouteAssignmentService::routeReleased,
                     telemetryService, [telemetryService](const QString& routeId, const QString& reason) {
                         telemetryService->recordRouteEvent(routeId, "ROUTE_RELEASED", QVariantMap{
                                                                                           {"reason", reason}
                                                                                       });
                     });

    // B. Route Service to Safety Monitor connections
    QObject::connect(routeAssignmentService, &RouteAssignmentService::routeFailed,
                     safetyMonitorService, [safetyMonitorService](const QString& routeId, const QString& reason) {
                         safetyMonitorService->recordSafetyViolation(routeId, reason, "WARNING");
                     });

    QObject::connect(routeAssignmentService, &RouteAssignmentService::emergencyActivated,
                     safetyMonitorService, [safetyMonitorService](const QString& reason) {
                         safetyMonitorService->recordEmergencyEvent("EMERGENCY_MODE_ACTIVATED", reason);
                     });

    QObject::connect(routeAssignmentService, &RouteAssignmentService::systemOverloaded,
                     safetyMonitorService, [safetyMonitorService](int pendingRequests, int maxConcurrent) {
                         safetyMonitorService->recordPerformanceWarning("SYSTEM_OVERLOAD", QVariantMap{
                                                                                               {"pendingRequests", pendingRequests},
                                                                                               {"maxConcurrent", maxConcurrent}
                                                                                           });
                     });

    // C. Database to Route Service (Track Circuit Changes) - reactive updates
    QObject::connect(dbManager, &DatabaseManager::trackCircuitUpdated,
                     routeAssignmentService, [routeAssignmentService, dbManager](const QString& circuitId) {
                         // Get occupancy state from database and call the slot
                         auto circuit = dbManager->getTrackCircuitById(circuitId);
                         bool isOccupied = circuit.value("is_occupied", false).toBool();
                         routeAssignmentService->onTrackCircuitOccupancyChanged(circuitId, isOccupied);
                     });

    QObject::connect(dbManager, &DatabaseManager::pointMachineUpdated,
                     routeAssignmentService, [routeAssignmentService, dbManager](const QString& machineId) {
                         QString position = dbManager->getCurrentPointPosition(machineId);
                         // Route service would handle point machine position changes if needed
                     });

    // D. Emergency Shutdown Connection - safety critical
    QObject::connect(safetyMonitorService, &SafetyMonitorService::emergencyShutdownRequired,
                     [routeAssignmentService](const QString& reason) {
                         qCritical() << " EMERGENCY SHUTDOWN TRIGGERED:" << reason;
                         routeAssignmentService->emergencyReleaseAllRoutes("EMERGENCY_SHUTDOWN: " + reason);
                     });

    QObject::connect(routeAssignmentService, &RouteAssignmentService::routeActivated,
                     dbManager, [dbManager](const QString& routeId) {
                         dbManager->updateRouteActivation(routeId);
                         dbManager->insertRouteEvent(routeId, "ROUTE_ACTIVATED", QVariantMap{}, "ROUTE_SYSTEM");
                     });

    QObject::connect(routeAssignmentService, &RouteAssignmentService::routeReleased,
                     dbManager, [dbManager](const QString& routeId, const QString& reason) {
                         dbManager->updateRouteRelease(routeId);
                         dbManager->insertRouteEvent(routeId, "ROUTE_RELEASED", QVariantMap{{"reason", reason}}, "ROUTE_SYSTEM");
                     });

    QObject::connect(routeAssignmentService, &RouteAssignmentService::routeFailed,
                     dbManager, [dbManager](const QString& routeId, const QString& reason) {
                         dbManager->updateRouteFailure(routeId, reason);
                         dbManager->insertRouteEvent(routeId, "ROUTE_FAILED", QVariantMap{{"reason", reason}}, "ROUTE_SYSTEM");
                     });

    // F. Performance monitoring connections
    QObject::connect(routeAssignmentService, &RouteAssignmentService::performanceWarning,
                     [](const QString& metric, double value, double threshold) {
                         qWarning() << " Route Performance Warning:" << metric << "=" << value << "(threshold:" << threshold << ")";
                     });

    //   ADD: Cleanup on application exit
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [dbManager]() {
        qDebug() << " Application shutting down, cleaning up database...";
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

    //   Start database connection and polling - services will initialize via callback
    qDebug() << " Connecting to database...";
    if (dbManager->connectToDatabase()) {
        qDebug() << "  Database connection established";
        dbManager->startPolling();
        dbManager->enableRealTimeUpdates();  //   Enable LISTEN/NOTIFY
    } else {
        qWarning() << " Failed to connect to database - some features may not be available";
    }

    return app.exec();
}
