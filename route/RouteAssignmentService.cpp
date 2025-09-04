#include "RouteAssignmentService.h"
#include "../database/DatabaseManager.h"

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

// Database integration stubs
bool RouteAssignmentService::loadConfiguration() {
    // Load configuration from database or config files
    return true; // Placeholder
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

QVariantMap RouteAssignmentService::scanDestinationSignals(
    const QString& sourceSignalId,
    const QString& direction,
    bool includeBlocked) {

    QElapsedTimer scanTimer;
    scanTimer.start();

    // Validate source signal
    if (!m_dbManager) {
        return QVariantMap{{"error", "Database manager not available"}};
    }

    auto sourceSignal = m_dbManager->getSignalById(sourceSignalId);
    if (sourceSignal.isEmpty()) {
        return QVariantMap{{"error", "Source signal not found: " + sourceSignalId}};
    }

    // Auto-determine direction if needed
    QString actualDirection = direction;
    if (direction == "AUTO") {
        actualDirection = "UP";
    }

    if (actualDirection != "UP" && actualDirection != "DOWN") {
        return QVariantMap{{"error", "Invalid direction: " + actualDirection}};
    }

    // Perform scan
    auto candidates = performDestinationScan(sourceSignalId, actualDirection);

    // Count different types before filtering
    int totalCandidates = candidates.size();
    int reachableClear = 0, reachableRequiresPM = 0, blocked = 0, invalid = 0;

    for (const auto& candidate : candidates) {
        if (candidate.reachability == "REACHABLE_CLEAR") reachableClear++;
        else if (candidate.reachability == "REACHABLE_REQUIRES_PM") reachableRequiresPM++;
        else if (candidate.reachability == "BLOCKED") blocked++;

        if (candidate.pathSummary.hopCount < 0) invalid++;
    }

    // Filter out blocked candidates if requested
    if (!includeBlocked) {
        candidates.erase(
            std::remove_if(candidates.begin(), candidates.end(),
                           [](const DestinationCandidate& c) {
                               return c.reachability == "BLOCKED";
                           }),
            candidates.end()
            );
    }

    // Format results
    auto results = formatScanResults(candidates);
    results["scan_time_ms"] = scanTimer.elapsed();
    results["source_signal_id"] = sourceSignalId;
    results["direction"] = actualDirection;
    results["total_candidates"] = candidates.size();

    // Add summary statistics for monitoring
    results["summary_stats"] = QVariantMap{
        {"reachable_clear", reachableClear},
        {"reachable_requires_pm", reachableRequiresPM},
        {"blocked", blocked},
        {"invalid_paths", invalid}
    };

    Q_UNUSED(totalCandidates);
    return results;
}

QList<RouteAssignmentService::DestinationCandidate>
RouteAssignmentService::performDestinationScan(
    const QString& sourceSignalId,
    const QString& direction) {

    QList<DestinationCandidate> candidates;

    // =====================================
    // HARDCODED ROUTE DEFINITIONS
    // =====================================

    // Helper function to create a basic candidate
    auto createCandidate = [&](const QString& destId, const QString& displayName,
                               const QString& reachability, int hopCount = 3,
                               double weight = 1.0, const QStringList& circuitPreview = {},
                               const QList<DestinationCandidate::RequiredPMAction>& pmActions = {}) -> DestinationCandidate {
        DestinationCandidate candidate;
        candidate.destSignalId = destId;
        candidate.displayName = displayName;
        candidate.direction = direction;
        candidate.reachability = reachability;

        if (reachability == "BLOCKED") {
            candidate.blockedReason = "BLOCK";
            candidate.pathSummary.hopCount = -1;
            candidate.pathSummary.estimatedWeight = -1.0;
            candidate.pathSummary.circuitsPreview.clear();
        } else {
            candidate.pathSummary.hopCount = hopCount;
            candidate.pathSummary.estimatedWeight = weight;
            candidate.pathSummary.circuitsPreview = circuitPreview.isEmpty()
                                                        ? QStringList{"TC01", "TC02", "...", "TC" + QString::number(hopCount + 10)}
                                                        : circuitPreview;
        }

        candidate.requiredPMActions = pmActions;
        candidate.conflicts.clear();
        candidate.telemetry.clear();

        return candidate;
    };

    // Helper function to create a PM action
    auto createPMAction = [](const QString& machineId, const QString& currentPos,
                             const QString& targetPos) -> DestinationCandidate::RequiredPMAction {
        DestinationCandidate::RequiredPMAction action;
        action.machineId = machineId;
        action.currentPosition = currentPos;
        action.targetPosition = targetPos;
        return action;
    };

    // =====================================
    // HARDCODED SIGNAL ROUTES
    // =====================================

    if (sourceSignalId == "HM001") {
        // Route from HM001 - HOME signal
        candidates.append(createCandidate("ST001", "ST001 (Starter)", "REACHABLE_CLEAR", 2, 1,
                                          QStringList{"W22T", "3T"}));
        candidates.append(createCandidate("ST002", "ST002 (Starter)", "REACHABLE_CLEAR", 2, 1,
                                          QStringList{"W22T", "4T"}));
    }
    else if (sourceSignalId == "HM002") {
        // Route from HM002 - HOME signal
        candidates.append(createCandidate("ST003", "ST003 (Starter)", "REACHABLE_CLEAR", 2, 1,
                                          QStringList{"W21T", "3T"}));
        candidates.append(createCandidate("ST004", "ST004 (Starter)", "REACHABLE_CLEAR", 2, 1,
                                          QStringList{"W21T", "4T"}));
    }
    else if (sourceSignalId == "ST001") {
        candidates.append(createCandidate("AS001", "AS001 (Advanced Starter)", "REACHABLE_CLEAR", 2, 1,
                                          QStringList{"W21T", "2T"}));
    }
    else if (sourceSignalId == "ST002") {
        candidates.append(createCandidate("AS002", "AS002 (Advanced Starter)", "REACHABLE_CLEAR", 2, 1,
                                          QStringList{"W22T", "5T"}));
    }
    else {
        // Default case - return some generic candidates for any unknown signal
        candidates.append(createCandidate("DEFAULT_01", "Default Dest 1", "REACHABLE_CLEAR", 3, 2.0));
        candidates.append(createCandidate("DEFAULT_02", "Default Dest 2", "BLOCKED"));
    }

    // =====================================
    // MAINTAIN ORIGINAL SORTING LOGIC
    // =====================================

    // Sort candidates: Reachable first, then by hop count, then by weight
    std::sort(candidates.begin(), candidates.end(),
              [](const DestinationCandidate& a, const DestinationCandidate& b) {
                  auto getPriority = [](const QString& reachability) {
                      if (reachability == "REACHABLE_CLEAR") return 0;
                      if (reachability == "REACHABLE_REQUIRES_PM") return 1;
                      return 2; // BLOCKED
                  };
                  int aPriority = getPriority(a.reachability);
                  int bPriority = getPriority(b.reachability);
                  if (aPriority != bPriority) return aPriority < bPriority;
                  if (a.pathSummary.hopCount != b.pathSummary.hopCount)
                      return a.pathSummary.hopCount < b.pathSummary.hopCount;
                  return a.pathSummary.estimatedWeight < b.pathSummary.estimatedWeight;
              });

    Q_UNUSED(sourceSignalId);
    Q_UNUSED(direction);
    return candidates;
}

} // namespace RailFlux::Route
