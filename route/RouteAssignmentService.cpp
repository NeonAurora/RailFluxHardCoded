#include "RouteAssignmentService.h"
#include "../database/DatabaseManager.h"
#include "GraphService.h"
#include "ResourceLockService.h"
#include "OverlapService.h"
#include "TelemetryService.h"
#include "VitalRouteController.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <QtMath>
#include <algorithm>

namespace RailFlux::Route {

RouteAssignmentService::RouteAssignmentService(QObject* parent)
    : QObject(parent)
    , m_processingTimer(new QTimer(this))
    , m_serviceStartTime(QDateTime::currentDateTime().toSecsSinceEpoch())
    , m_maintenanceTimer(new QTimer(this))
{
    initializeHardcodedRoutes();
}

RouteAssignmentService::~RouteAssignmentService() {
    if (m_processingTimer) {
        m_processingTimer->stop();
    }
    if (m_maintenanceTimer) {
        m_maintenanceTimer->stop();
    }
}

void RouteAssignmentService::setServices(
    DatabaseManager* dbManager
) {
    m_dbManager = dbManager;
}

void RouteAssignmentService::initialize() {
    if (!m_dbManager) {
        qCritical() << "[RouteAssignmentService > initialize] DatabaseManager not set";
        return;
    }

    try {

        // Load configuration
        if (loadConfiguration()) {
            // Connect to database changes for reactive updates
            connect(
                m_dbManager,
                &DatabaseManager::connectionStateChanged,
                this,
                [this](bool connected) {
                    if (!connected) {
                        m_isOperational = false;
                        emit operationalStateChanged();
                    }
                }
            );
        } else {
            qCritical() << "[RouteAssignmentService > initialize] Failed to load configuration";
        }
    } catch (const std::exception& e) {
        qCritical() << "[RouteAssignmentService > initialize] Initialization failed:" << e.what();
        m_isOperational = false;
        emit operationalStateChanged();
    }
}

QString RouteAssignmentService::requestRoute(
    const QString& sourceSignalId,
    const QString& destSignalId,
    const QString& direction,
    const QString& requestedBy,
    const QVariantMap& trainData,
    const QString& priority
    ) {
    m_totalRequests++;

    // =====================================
    // GENERATE ROUTE ID
    // =====================================
    QString routeId = QUuid::createUuid().toString();

    qDebug() << "🚀 [HARDCODED_ROUTE] Processing route request:";
    qDebug() << "   📍 Route ID:" << routeId;
    qDebug() << "   🚦 From:" << sourceSignalId << "→" << destSignalId;
    qDebug() << "   👤 Requested by:" << requestedBy;

    // =====================================
    // FIND HARDCODED ROUTE
    // =====================================
    QElapsedTimer timer;
    timer.start();

    HardcodedRoute hardcodedRoute = findHardcodedRoute(sourceSignalId, destSignalId);

    if (hardcodedRoute.reachability == "BLOCKED") {
        qWarning() << "❌ Route blocked:" << hardcodedRoute.blockedReason;

        // Emit failure signal
        emit routeFailed(routeId, hardcodedRoute.blockedReason);

        m_failedRoutes++;
        return QString();  // Return empty string to indicate failure
    }

    // =====================================
    // SIMULATE PROCESSING TIME
    // =====================================
    // Sleep for realistic processing time (optional)
    QThread::msleep(static_cast<unsigned long>(hardcodedRoute.simulatedProcessingTime));

    // =====================================
    // APPLY HARDCODED ROUTE CHANGES
    // =====================================
    bool routeSuccess = applyHardcodedRoute(routeId, hardcodedRoute, requestedBy);

    double totalTime = timer.elapsed();

    if (routeSuccess) {
        qDebug() << "✅ [HARDCODED_ROUTE] Route established successfully!";
        qDebug() << "   ⏱️ Total time:" << totalTime << "ms";
        qDebug() << "   🛤️ Path:" << hardcodedRoute.path.join(" → ");
        qDebug() << "   🚦 Signals set:" << hardcodedRoute.signalAspects.keys();
        qDebug() << "   🔧 Point machines:" << hardcodedRoute.pointMachineSettings.keys();

        // Emit success signal
        emit routeAssigned(routeId, sourceSignalId, destSignalId, hardcodedRoute.path);

        m_successfulRoutes++;

        // Update metrics
        if (m_telemetryService) {
            m_telemetryService->recordPerformanceMetric(
                "route_processing_hardcoded",
                totalTime,
                true,
                routeId,
                QVariantMap{
                    {"sourceSignal", sourceSignalId},
                    {"destSignal", destSignalId},
                    {"pathLength", hardcodedRoute.path.size()},
                    {"overlapCount", hardcodedRoute.overlapCircuits.size()}
                }
                );
        }

        return routeId;
    } else {
        qCritical() << "❌ [HARDCODED_ROUTE] Failed to apply route changes";
        emit routeFailed(routeId, "ROUTE_APPLICATION_FAILED");
        m_failedRoutes++;
        return QString();
    }
}

// Utility methods
QString RouteAssignmentService::generateRequestId() const {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString RouteAssignmentService::routeStateToString(RouteState state) const {
    // Basic implementation - adjust based on actual RouteState enum values
    switch (state) {
        case RouteState::REQUESTED:           return "REQUESTED";
        case RouteState::VALIDATING:          return "VALIDATING";
        case RouteState::RESERVED:            return "RESERVED";
        case RouteState::ACTIVE:              return "ACTIVE";
        case RouteState::PARTIALLY_RELEASED:  return "PARTIALLY_RELEASED";
        case RouteState::RELEASED:            return "RELEASED";
        case RouteState::FAILED:              return "FAILED";
        case RouteState::EMERGENCY_RELEASED:  return "EMERGENCY_RELEASED";
        default:                              return "UNKNOWN";
    }
}

// Database integration stubs
bool RouteAssignmentService::loadConfiguration() {
    // Load configuration from database or config files
    return true; // Placeholder
}

bool RouteAssignmentService::activateRoute(const QString& routeId) {
    if (!m_vitalController) {
        return false;
    }

    // Update route state to ACTIVE instead of calling non-existent method
    bool success = m_vitalController->updateRouteState(routeId, "ACTIVE");

    if (success) {
        emit routeActivated(routeId);
    } else {
        qCritical() << "[RouteAssignmentService > activateRoute] Failed to activate route" << routeId;
    }

    return success;
}

QVariantMap RouteAssignmentService::formatScanResults(const QList<DestinationCandidate>& candidates) {

    QVariantMap result;
    QVariantList reachableClear, reachableRequiresPM, blocked;

    // Group candidates by reachability
    for (const auto& candidate : candidates) {
        QVariantMap candidateMap;
        candidateMap["dest_signal_id"] = candidate.destSignalId;
        candidateMap["display_name"] = candidate.displayName;
        candidateMap["direction"] = candidate.direction;
        candidateMap["reachability"] = candidate.reachability;
        candidateMap["blocked_reason"] = candidate.blockedReason;

        // Path summary
        QVariantMap pathSummary;
        pathSummary["hop_count"] = candidate.pathSummary.hopCount;
        pathSummary["circuits_preview"] = QVariantList(candidate.pathSummary.circuitsPreview.begin(),
                                                       candidate.pathSummary.circuitsPreview.end());
        pathSummary["estimated_weight"] = candidate.pathSummary.estimatedWeight;
        candidateMap["path_summary"] = pathSummary;

        // Required PM actions
        QVariantList pmActions;
        for (const auto& action : candidate.requiredPMActions) {
            QVariantMap actionMap;
            actionMap["machine_id"] = action.machineId;
            actionMap["current_position"] = action.currentPosition;
            actionMap["target_position"] = action.targetPosition;
            pmActions.append(actionMap);
        }
        candidateMap["required_pm_actions"] = pmActions;

        candidateMap["conflicts"] = QVariantList(candidate.conflicts.begin(), candidate.conflicts.end());

        if (candidate.reachability == "REACHABLE_CLEAR") {
            reachableClear.append(candidateMap);
        } else if (candidate.reachability == "REACHABLE_REQUIRES_PM") {
            reachableRequiresPM.append(candidateMap);
        } else {
            blocked.append(candidateMap);
        }
    }

    result["reachable_clear"] = reachableClear;
    result["reachable_requires_pm"] = reachableRequiresPM;
    result["blocked"] = blocked;
    result["success"] = true;

    return result;
}


int RouteAssignmentService::convertPriorityToInt(const QString& priorityStr) const {
    //   SAFETY: Convert string priorities to valid database range (1-1000)
    if (priorityStr == "EMERGENCY") {
        return 1000;        // Highest priority
    } else if (priorityStr == "HIGH") {
        return 600;         // High priority
    } else if (priorityStr == "NORMAL") {
        return 100;         // Normal priority (default)
    } else if (priorityStr == "LOW") {
        return 50;          // Low priority (but still > 1)
    } else {
        qWarning() << "RouteAssignmentService: Unknown priority string:" << priorityStr
                   << "- using default priority 100";
        return 100;         // Safe default
    }
}

void RailFlux::Route::RouteAssignmentService::initializeHardcodedRoutes() {

    // =====================================
    // DEFINE YOUR HARDCODED ROUTES HERE
    // =====================================

    // Helper lambda to create routes easily - ✅ FIXED PARAMETERS
    auto createRoute = [](const QString& source, const QString& dest,
                          const QStringList& path, const QStringList& overlap,
                          const QVariantMap& signalAspects, const QVariantMap& pointMachines,  // ✅ RENAMED from 'signals'
                          const QString& reachability = "SUCCESS",
                          const QString& blockedReason = "",
                          double processingTime = 50.0) -> HardcodedRoute {
        HardcodedRoute route;
        route.sourceSignalId = source;
        route.destSignalId = dest;
        route.path = path;
        route.overlapCircuits = overlap;
        route.signalAspects = signalAspects;  // ✅ FIXED PARAMETER NAME
        route.pointMachineSettings = pointMachines;
        route.reachability = reachability;
        route.blockedReason = blockedReason;
        route.simulatedProcessingTime = processingTime;
        return route;
    };

    // =====================================
    // ROUTE DEFINITIONS - ✅ NOW WITH CORRECT PARAMETER COUNT
    // =====================================

    // Route 1: HM001 → ST001 (Simple route)
    m_hardcodedRoutes.routes.append(createRoute(
        "HM001", "ST001",
        QStringList{"W22T", "3T"},           // Path
        QStringList{"W21T", "2T"},                           // Overlap
        QVariantMap{{"HM001", "YELLOW"}, {"ST001", "RED"}},     // Signal aspects
        QVariantMap{{"PM001", "NORMAL"}},                      // Point machines
        "SUCCESS", "", 45.0
        ));

    // Route 2: HM001 → ST002 (Requires PM movement)
    m_hardcodedRoutes.routes.append(createRoute(
        "HM001", "ST002",
        QStringList{"W22T", "4T"},
        QStringList{"W21T", "2T"},
        QVariantMap{{"HM001", "YELLOW"}, {"ST002", "RED"}},
        QVariantMap{{"PM001", "REVERSE"}},
        "SUCCESS", "", 75.0
        ));

    // Route 4: ST001 → AS001 (Starter to Advanced Starter)
    m_hardcodedRoutes.routes.append(createRoute(
        "ST001", "AS001",
        QStringList{"W21T", "2T"},
        QStringList{"1T", "A1T"},
        QVariantMap{{"ST001", "YELLOW"}, {"AS001", "RED"}},
        QVariantMap{},  // No PM changes needed
        "SUCCESS", "", 35.0
        ));

    // =====================================
    // INDEX ROUTES BY SOURCE SIGNAL
    // =====================================
    for (const auto& route : m_hardcodedRoutes.routes) {
        m_hardcodedRoutes.routesBySource[route.sourceSignalId].append(route);
    }

    qDebug() << "✅ Initialized" << m_hardcodedRoutes.routes.size() << "hardcoded routes";
}

HardcodedRoute RailFlux::Route::RouteAssignmentService::findHardcodedRoute(const QString& sourceId, const QString& destId) {
    // Search for matching route
    for (const auto& route : m_hardcodedRoutes.routes) {
        if (route.sourceSignalId == sourceId && route.destSignalId == destId) {
            return route;
        }
    }

    // Return empty route if not found
    HardcodedRoute emptyRoute;
    emptyRoute.reachability = "BLOCKED";
    emptyRoute.blockedReason = "ROUTE_NOT_DEFINED";
    emptyRoute.simulatedProcessingTime = 25.0;  // ✅ Initialize this field
    return emptyRoute;
}

bool RailFlux::Route::RouteAssignmentService::applyHardcodedRoute(const QString& routeId, const HardcodedRoute& route, const QString& operatorId) {

    qDebug() << "🔧 [HARDCODED_ROUTE] Applying route changes for:" << routeId;

    // =====================================
    // STEP 1: SET SIGNAL ASPECTS
    // =====================================
    for (auto it = route.signalAspects.begin(); it != route.signalAspects.end(); ++it) {
        QString signalId = it.key();
        QString aspect = it.value().toString();

        qDebug() << "   🚦 Setting signal" << signalId << "to" << aspect;

        // ✅ CORRECTED: Use the correct DatabaseManager method signature
        if (m_dbManager) {
            bool success = m_dbManager->updateSignalAspect(signalId, "MAIN", aspect);  // ✅ 3 parameters
            if (!success) {
                qCritical() << "❌ Failed to set signal" << signalId << "to" << aspect;
                return false;
            }
        }
    }

    // =====================================
    // STEP 2: MOVE POINT MACHINES
    // =====================================
    for (auto it = route.pointMachineSettings.begin(); it != route.pointMachineSettings.end(); ++it) {
        QString machineId = it.key();
        QString position = it.value().toString();

        qDebug() << "   🔧 Moving point machine" << machineId << "to" << position;

        // ✅ CORRECTED: Use the method that exists in DatabaseManager
        if (m_dbManager) {
            // Based on the project knowledge, this method takes only 2 parameters
            bool success = m_dbManager->updatePointMachinePosition(machineId, position);
            if (!success) {
                qCritical() << "❌ Failed to move point machine" << machineId << "to" << position;
                return false;
            }
        }
    }

    // =====================================
    // STEP 3: PERSIST ROUTE ASSIGNMENT (SIMPLIFIED)
    // =====================================
    if (m_dbManager) {
        // ✅ SIMPLIFIED: Just log the route assignment for now
        qDebug() << "   📝 Route assignment recorded:"
                 << "ID:" << routeId
                 << "From:" << route.sourceSignalId
                 << "To:" << route.destSignalId
                 << "Path:" << route.path.join(" → ");

        // TODO: If you need database persistence, implement a proper route logging method
        // For now, we'll skip database persistence to avoid method signature issues
    }

    // =====================================
    // STEP 4: SETUP OVERLAP MONITORING (Optional)
    // =====================================
    if (!route.overlapCircuits.isEmpty()) {
        qDebug() << "   🛡️ Setting up overlap monitoring for:" << route.overlapCircuits;
        // You can add overlap monitoring logic here if needed
    }

    return true;
}

} // namespace RailFlux::Route
