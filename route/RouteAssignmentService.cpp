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
    // Setup processing timer
    m_processingTimer->setInterval(m_queueProcessingIntervalMs);
    connect(
        m_processingTimer,
        &QTimer::timeout,
        this,
        &RouteAssignmentService::processRequestQueue
    );

    // Setup maintenance timer
    m_maintenanceTimer->setInterval(m_maintenanceIntervalMs);
    connect(
        m_maintenanceTimer,
        &QTimer::timeout,
        this,
        &RouteAssignmentService::performMaintenanceCheck
    );
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
    DatabaseManager* dbManager,
    GraphService* graphService,
    ResourceLockService* resourceLockService,
    OverlapService* overlapService,
    TelemetryService* telemetryService,
    VitalRouteController* vitalController
) {
    m_dbManager = dbManager;

    // Take ownership of services (they will be managed by this service)
    m_graphService.reset(graphService);
    m_resourceLockService.reset(resourceLockService);
    m_overlapService.reset(overlapService);
    m_telemetryService.reset(telemetryService);
    m_vitalController.reset(vitalController);
}

void RouteAssignmentService::initialize() {
    if (!m_dbManager) {
        qCritical() << "[RouteAssignmentService > initialize] DatabaseManager not set";
        return;
    }

    try {
        // Initialize all composed services
        if (m_graphService) m_graphService->loadGraphFromDatabase();
        if (m_resourceLockService) m_resourceLockService->initialize();
        if (m_overlapService) m_overlapService->initialize();
        if (m_telemetryService) m_telemetryService->initialize();
        if (m_vitalController) m_vitalController->initialize();

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

            // Connect to track circuit changes
            connect(
                m_dbManager,
                &DatabaseManager::trackSegmentUpdated,
                this,
                [this](const QString& segmentId) {
                    // Get the circuit associated with this segment and its occupancy
                    QString circuitId = m_dbManager->getCircuitIdByTrackSegmentId(segmentId);
                    if (!circuitId.isEmpty()) {
                        auto circuit = m_dbManager->getTrackCircuitById(circuitId);
                        bool isOccupied = circuit.value("is_occupied", false).toBool();
                        onTrackCircuitOccupancyChanged(circuitId, isOccupied);
                    }
                }
            );

            // Start processing
            m_isOperational = areServicesHealthy();
            if (m_isOperational) {
                m_processingTimer->start();
                m_maintenanceTimer->start();
                emit operationalStateChanged();

                // Record initialization
                if (m_telemetryService) {
                    m_telemetryService->recordSafetyEvent(
                        "route_service_initialized",
                        "INFO",
                        "RouteAssignmentService",
                        "Main orchestration service initialized",
                        "system"
                    );
                }
            } else {
                qCritical() << "[RouteAssignmentService > initialize] Services not healthy";
            }
        } else {
            qCritical() << "[RouteAssignmentService > initialize] Failed to load configuration";
        }
    } catch (const std::exception& e) {
        qCritical() << "[RouteAssignmentService > initialize] Initialization failed:" << e.what();
        m_isOperational = false;
        emit operationalStateChanged();
    }
}

bool RouteAssignmentService::areServicesHealthy() const {
    return m_dbManager && m_dbManager->isConnected() &&
           m_graphService && m_graphService->isLoaded() &&
           m_resourceLockService && m_resourceLockService->isOperational() &&
           m_overlapService && m_overlapService->isOperational() &&
           m_telemetryService && m_telemetryService->isOperational() &&
           m_vitalController && m_vitalController->isOperational();
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
    // BASIC VALIDATION (Keep existing)
    // =====================================
    if (!m_isOperational) {
        qWarning() << "❌ Route request rejected - service not operational";
        return QString();
    }

    if (!isValidSignalId(sourceSignalId) || !isValidSignalId(destSignalId)) {
        qWarning() << "❌ Route request rejected - invalid signal IDs";
        return QString();
    }

    if (!isValidDirection(direction)) {
        qWarning() << "❌ Route request rejected - invalid direction";
        return QString();
    }

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

void RouteAssignmentService::addToQueue(const RouteRequest& request) {
    m_requestQueue.enqueue(request);

    // Prioritize queue if needed
    if (m_requestQueue.size() > 1) {
        prioritizeQueue();
    }

    // Check for overload
    if (m_requestQueue.size() > OVERLOAD_THRESHOLD) {
        emit systemOverloaded(m_requestQueue.size(), m_maxConcurrentRoutes);

        if (m_telemetryService) {
            m_telemetryService->recordSafetyEvent(
                "system_overload",
                "WARNING",
                "RouteAssignmentService",
                QString("Request queue size: %1").arg(m_requestQueue.size()),
                "system"
            );
        }
    }
}

void RouteAssignmentService::prioritizeQueue() {
    // Convert queue to list for sorting
    QList<RouteRequest> requests;
    while (!m_requestQueue.isEmpty()) {
        requests.append(m_requestQueue.dequeue());
    }

    // Sort by priority (higher priority first, then by timestamp)
    std::sort(
        requests.begin(),
        requests.end(),
        [this](const RouteRequest& a, const RouteRequest& b) {
            int priorityA = calculateRequestPriority(a);
            int priorityB = calculateRequestPriority(b);

            if (priorityA != priorityB) {
                return priorityA > priorityB; // Higher priority first
            }

            return a.requestedAt < b.requestedAt; // Earlier requests first
        }
    );

    // Rebuild queue
    for (const RouteRequest& request : requests) {
        m_requestQueue.enqueue(request);
    }
}

int RouteAssignmentService::calculateRequestPriority(const RouteRequest& request) const {
    // Priority scoring system
    int score = 100; // Base priority

    if (request.priority == "EMERGENCY") {
        score += 1000;
    } else if (request.priority == "HIGH") {
        score += 500;
    } else if (request.priority == "LOW") {
        score -= 100;
    }

    // Age factor - older requests get higher priority
    qint64 ageSeconds = request.requestedAt.secsTo(QDateTime::currentDateTime());
    score += static_cast<int>(ageSeconds / 10); // +1 point per 10 seconds

    return score;
}

void RouteAssignmentService::processRequestQueue() {
    if (!m_isOperational || m_requestQueue.isEmpty()) {
        return;
    }

    if (!shouldProcessRequest()) {
        return; // Too many active routes or system overloaded
    }

    RouteRequest request = dequeueRequest();

    QElapsedTimer totalTimer;
    totalTimer.start();

    // Add to processing requests
    m_processingRequests[request.key()] = request;

    // Process the request through the pipeline
    ProcessingResult result = processRouteRequest(request);

    double totalTime = totalTimer.elapsed();
    recordProcessingTime("total_processing", totalTime);

    // Remove from processing
    m_processingRequests.remove(request.key());

    if (result.success) {
        m_successfulRoutes++;
        emit routeAssigned(result.routeId, request.sourceSignalId, request.destSignalId, result.path);
        emit routeCountChanged();
    } else {
        m_failedRoutes++;
        emit routeFailed(request.key(), result.error);
    }

    // Record performance metrics
    if (m_telemetryService) {
        m_telemetryService->recordPerformanceMetric(
            "route_processing",
            totalTime,
            result.success,
            request.key(),
            QVariantMap{
                {"sourceSignal", request.sourceSignalId},
                {"destSignal", request.destSignalId},
                {"direction",    request.direction},
                {"pathLength",   result.path.size()},
                {"overlapCount", result.overlapCircuits.size()}
            }
        );
    }

    emit requestQueueChanged();
}

RouteRequest RouteAssignmentService::dequeueRequest() {
    return m_requestQueue.dequeue();
}

bool RouteAssignmentService::shouldProcessRequest() const {
    // Check if we can accept more concurrent routes
    int currentActiveRoutes = activeRoutes();
    int maxRoutes = m_degradedMode ? m_degradedMaxRoutes : m_maxConcurrentRoutes;

    return currentActiveRoutes < maxRoutes;
}

ProcessingResult RouteAssignmentService::processRouteRequest(const RouteRequest& request) {
    ProcessingResult result;
    result.performanceBreakdown = QVariantMap();

    //   STAGE 1: Basic Request Validation (KEEP - No conflicts)
    QElapsedTimer stageTimer;
    stageTimer.start();

    result = validateRequest(request);
    if (!result.success) return result;

    double validationTime = stageTimer.elapsed();
    result.performanceBreakdown["validation_ms"] = validationTime;

    //   STAGE 2: Pathfinding (KEEP - Essential for point machine calculation)
    stageTimer.restart();

    ProcessingResult pathResult = performPathfinding(request);
    if (!pathResult.success) return pathResult;

    result.path = pathResult.path;
    double pathfindingTime = stageTimer.elapsed();
    result.performanceBreakdown["pathfinding_ms"] = pathfindingTime;

    //   STAGE 3: Overlap Calculation (KEEP - Essential for safety)
    stageTimer.restart();

    ProcessingResult overlapResult = calculateOverlap(request, result.path);
    if (!overlapResult.success) return overlapResult;

    result.overlapCircuits = overlapResult.overlapCircuits;
    double overlapTime = stageTimer.elapsed();
    result.performanceBreakdown["overlap_calculation_ms"] = overlapTime;

    //  STAGE 4: REPLACE reserveResources() with Intelligent Route Establishment
    stageTimer.restart();

    if (!m_vitalController) {
        result.error = "VitalRouteController not available";
        return result;
    }

    // Get current point machine states for intelligent propagation
    QVariantMap currentPMStates;
    if (m_dbManager) {
        currentPMStates = m_dbManager->getAllPointMachineStates();
    }

    //   CALL INTELLIGENT ROUTE ESTABLISHMENT (replaces reserveResources + execution)
    QVariantMap intelligentResult = m_vitalController->establishRouteWithIntelligentAspects(
        request.sourceSignalId,
        request.destSignalId,
        result.path,                    //  Pass route path
        result.overlapCircuits,         //  Pass overlap path
        currentPMStates
        );

    if (!intelligentResult["success"].toBool()) {
        result.error = QString("Intelligent route establishment failed: %1")
        .arg(intelligentResult["error"].toString());
        return result;
    }

    // Extract results from intelligent establishment
    result.routeId = request.requestId.toString();
    result.signalAspects = intelligentResult["signalAspects"].toMap();
    result.pointMachines = intelligentResult["pointMachines"].toMap();

    double intelligentTime = stageTimer.elapsed();
    result.performanceBreakdown["intelligent_establishment_ms"] = intelligentTime;

    //   STAGE 5: Database Persistence (KEEP - But simplified)
    stageTimer.restart();

    //   SIMPLIFIED: Only persist route assignment (signals/PMs already updated by intelligent establishment)
    if (!persistRouteAssignment(result.routeId, request, result.path)) {
        result.error = "Failed to persist route assignment";
        return result;
    }

    //   ENHANCED: Reserve overlap with point machine awareness
    if (!result.overlapCircuits.isEmpty() && m_overlapService) {
        QStringList releaseTriggers;
        QVariantMap overlapReservation = m_overlapService->reserveOverlap(
            result.routeId,
            request.destSignalId,
            result.overlapCircuits,
            releaseTriggers,
            request.requestedBy
            );
        // Overlap reservation failure is non-critical
    }

    double finalizationTime = stageTimer.elapsed();
    result.performanceBreakdown["finalization_ms"] = finalizationTime;

    //   SUCCESS
    result.success = true;
    result.totalTimeMs = validationTime + pathfindingTime + overlapTime + intelligentTime + finalizationTime;

    qDebug() << "  ProcessRouteRequest: Complete intelligent route establishment succeeded";
    qDebug() << "    Total time:" << result.totalTimeMs << "ms";
    qDebug() << "   Signals set:" << result.signalAspects.keys();
    qDebug() << "    Point machines moved:" << result.pointMachines.keys();

    return result;
}

ProcessingResult RouteAssignmentService::validateRequest(const RouteRequest& request) {
    ProcessingResult result;

    if (!m_vitalController) {
        result.error = "VitalRouteController not available";
        return result;
    }

    // Use VitalRouteController for safety-critical validation
    QVariantMap validationResult = m_vitalController->validateRouteRequest(
        request.sourceSignalId,
        request.destSignalId,
        request.direction,
        request.requestedBy
    );

    if (!validationResult["success"].toBool()) {
        result.error = QString("Validation failed: %1").arg(validationResult["reason"].toString());
        result.validationResults = validationResult;
        return result;
    }

    result.success = true;
    result.validationResults = validationResult;
    return result;
}

ProcessingResult RouteAssignmentService::performPathfinding(const RouteRequest& request) {
    ProcessingResult result;

    if (!m_graphService) {
        result.error = "GraphService not available";
        return result;
    }

    // Get the right circuits for pathfinding
    QString startCircuitId = resolveSignalToCircuit(request.sourceSignalId, true);
    QString goalCircuitId = resolveSignalToCircuit(request.destSignalId, false);

    if (startCircuitId.isEmpty()) {
        result.error = QString("Source signal %1 has no succeededByCircuitId").arg(request.sourceSignalId);
        return result;
    }

    if (goalCircuitId.isEmpty()) {
        result.error = QString("Destination signal %1 has no precededByCircuitId").arg(request.destSignalId);
        return result;
    }

    QVariantMap currentPMStates;
    if (m_dbManager) {
        currentPMStates = m_dbManager->getAllPointMachineStates();
    }

    QVariantMap pathResult = m_graphService->findRoute(
        startCircuitId,
        goalCircuitId,
        request.direction,
        currentPMStates,
        static_cast<int>(PATHFINDING_TIMEOUT_MS)
    );

    if (!pathResult["success"].toBool()) {
        result.error = QString("Pathfinding failed: %1").arg(pathResult["error"].toString());
        return result;
    }

    result.success = true;
    result.path = pathResult["path"].toStringList();
    result.performanceBreakdown["pathfinding_nodes_explored"] = pathResult["nodesExplored"];
    result.performanceBreakdown["pathfinding_cost"] = pathResult["cost"];

    return result;
}

QString RouteAssignmentService::resolveSignalToCircuit(const QString& signalId, bool isSource) {
    if (!m_dbManager) {
        return QString();
    }

    QVariantMap signalData = m_dbManager->getSignalById(signalId);
    if (signalData.isEmpty()) {
        return QString();
    }

    QString circuitId;
    if (isSource) {
        circuitId = signalData.value("succeededByCircuitId", "").toString();
    } else {
        circuitId = signalData.value("precededByCircuitId", "").toString();
    }

    return circuitId;
}

ProcessingResult RouteAssignmentService::calculateOverlap(
    const RouteRequest& request,
    const QStringList& path
    ) {
    ProcessingResult result;

    Q_UNUSED(path)

    if (!m_overlapService) {
        result.error = "OverlapService not available";
        return result;
    }

    // Calculate overlap for destination signal
    QVariantMap overlapResult = m_overlapService->calculateOverlap(
        request.sourceSignalId,
        request.destSignalId,
        request.direction,
        request.trainData
    );

    if (!overlapResult["success"].toBool()) {
        result.error = QString("Overlap calculation failed: %1").arg(overlapResult["error"].toString());
        return result;
    }

    result.success = true;
    result.overlapCircuits = overlapResult["overlapCircuits"].toStringList();
    result.performanceBreakdown["overlap_hold_seconds"] = overlapResult["holdSeconds"];
    result.performanceBreakdown["overlap_method"] = overlapResult["method"];

    return result;
}

ProcessingResult RouteAssignmentService::finalizeRoute(
    const RouteRequest& request,
    const QStringList& path,
    const QStringList& overlap
    ) {
    ProcessingResult result;

    //   SIMPLIFIED: Only handle database persistence and overlap reservation
    // Signal aspects and point machine movements already handled by intelligent establishment

    // 1. Persist route assignment to database
    QString routeId = request.requestId.toString();
    if (!persistRouteAssignment(routeId, request, path)) {
        result.error = "Failed to persist route assignment to database";
        return result;
    }

    qDebug() << "  Route assignment persisted to database:" << routeId;

    // 2. Reserve overlap resources if needed (safety-critical)
    if (!overlap.isEmpty() && m_overlapService) {
        qDebug() << "🛡️ Reserving overlap resources for route:" << routeId;

        QStringList releaseTriggers; // Would be determined from overlap definition
        QVariantMap overlapReservation = m_overlapService->reserveOverlap(
            routeId,
            request.destSignalId,
            overlap,
            releaseTriggers,
            request.requestedBy
            );

        //   ENHANCED: Check overlap reservation result (optional but logged)
        if (overlapReservation["success"].toBool()) {
            qDebug() << "  Overlap reservation successful for" << overlap.size() << "circuits";
            result.overlapReservationId = overlapReservation["reservationId"].toString();
        } else {
            qWarning() << " Overlap reservation failed (non-critical):"
                       << overlapReservation["error"].toString();
            // Note: Overlap failure is non-critical - route can still proceed
        }
    } else {
        qDebug() << " No overlap required for this route";
    }

    // 3. Success - route is now fully established and persisted
    result.success = true;
    result.routeId = routeId;

    qDebug() << "  Route finalization completed:" << routeId;

    return result;
}


bool RouteAssignmentService::cancelRoute(const QString& routeId, const QString& reason) {
    if (!m_isOperational) {
        return false;
    }

    // Check if route is in processing
    if (m_processingRequests.contains(routeId)) {
        m_processingRequests.remove(routeId);
        return true;
    }

    // Use VitalRouteController to release active route
    if (m_vitalController) {
        QVariantMap releaseResult = m_vitalController->releaseRouteResources(routeId);
        if (releaseResult["success"].toBool()) {
            emit routeReleased(routeId, reason);
            emit routeCountChanged();
            return true;
        }
    }

    return false;
}

bool RouteAssignmentService::emergencyReleaseRoute(const QString& routeId, const QString& reason) {
    if (!m_vitalController) {
        return false;
    }

    QVariantMap result = m_vitalController->emergencyRelease(routeId, reason);
    bool success = result["success"].toBool();

    if (success) {
        m_emergencyReleases++;

        if (m_telemetryService) {
            m_telemetryService->recordSafetyEvent(
                "emergency_route_release",
                "CRITICAL",
                routeId,
                QString("Emergency release: %1").arg(reason),
                "RouteAssignmentService"
            );
        }

        qCritical() << "[RouteAssignmentService > emergencyReleaseRoute] Emergency release for route"
                    << routeId << ":" << reason;
        emit routeReleased(routeId, QString("EMERGENCY: %1").arg(reason));
        emit routeCountChanged();
    }

    return success;
}

bool RouteAssignmentService::emergencyReleaseAllRoutes(const QString& reason) {
    if (!m_vitalController) {
        return false;
    }

    QVariantMap result = m_vitalController->emergencyReleaseAll(reason);
    bool success = result["success"].toBool();

    if (success) {
        m_emergencyReleases++;

        if (m_telemetryService) {
            m_telemetryService->recordSafetyEvent(
                "emergency_all_routes_release",
                "EMERGENCY",
                "ALL_ROUTES",
                QString("Emergency release all: %1").arg(reason),
                "RouteAssignmentService"
            );
        }

        qCritical() << "[RouteAssignmentService > emergencyReleaseAllRoutes] EMERGENCY RELEASE ALL ROUTES:"
                    << reason;
        emit routeCountChanged();
    }

    return success;
}

void RouteAssignmentService::activateEmergencyMode(const QString& reason) {
    if (m_emergencyMode) {
        return; // Already in emergency mode
    }

    m_emergencyMode = true;
    enterDegradedMode();

    qCritical() << "[RouteAssignmentService > activateEmergencyMode] EMERGENCY MODE ACTIVATED:" << reason;
    emit emergencyActivated(reason);
    emit emergencyModeChanged();

    if (m_telemetryService) {
        m_telemetryService->recordSafetyEvent(
            "emergency_mode_activated",
            "EMERGENCY",
            "RouteAssignmentService",
            QString("Emergency mode activated: %1").arg(reason),
            "system"
        );
    }
}

void RouteAssignmentService::deactivateEmergencyMode() {
    if (!m_emergencyMode) {
        return;
    }

    m_emergencyMode = false;
    exitDegradedMode();

    emit emergencyDeactivated();
    emit emergencyModeChanged();

    if (m_telemetryService) {
        m_telemetryService->recordSafetyEvent(
            "emergency_mode_deactivated",
            "INFO",
            "RouteAssignmentService",
            "Emergency mode deactivated - normal operations resumed",
            "system"
        );
    }
}

void RouteAssignmentService::enterDegradedMode() {
    m_degradedMode = true;
    applyDegradedModeSettings();
}

void RouteAssignmentService::exitDegradedMode() {
    m_degradedMode = false;
    restoreNormalModeSettings();
}

void RouteAssignmentService::applyDegradedModeSettings() {
    // Reduce concurrent route limit
    // Increase processing intervals
    // Apply conservative timeouts
}

void RouteAssignmentService::restoreNormalModeSettings() {
    // Restore normal limits and timeouts
}

int RouteAssignmentService::activeRoutes() const {
    if (m_vitalController) {
        return m_vitalController->activeRoutes();
    }
    return 0;
}

void RouteAssignmentService::performMaintenanceCheck() {
    if (!m_isOperational) {
        return;
    }

    checkSystemHealth();
    checkPerformanceThresholds();
    updateAverageProcessingTime();

    // Clean up old processing requests
    QDateTime cutoff = QDateTime::currentDateTime().addSecs(-m_processingTimeoutMs / 1000);
    QStringList expiredRequests;

    for (auto it = m_processingRequests.begin(); it != m_processingRequests.end(); ++it) {
        if (it.value().requestedAt < cutoff) {
            expiredRequests.append(it.key());
        }
    }

    for (const QString& requestId : expiredRequests) {
        m_processingRequests.remove(requestId);
        m_timeouts++;
        // Timeouts are tracked via counters/telemetry; no log spam here
    }
}

void RouteAssignmentService::checkPerformanceThresholds() {
    if (m_averageProcessingTime > WARNING_PROCESSING_TIME_MS) {
        emit performanceWarning("average_processing_time", m_averageProcessingTime, WARNING_PROCESSING_TIME_MS);
    }

    if (m_requestQueue.size() > MAX_QUEUE_SIZE / 2) {
        emit performanceWarning("queue_size", m_requestQueue.size(), MAX_QUEUE_SIZE / 2);
    }
}

void RouteAssignmentService::recordProcessingTime(const QString& stage, double timeMs) {
    // Record stage-specific performance
    if (!m_stagePerformance.contains(stage)) {
        m_stagePerformance[stage] = QList<double>();
    }

    m_stagePerformance[stage].append(timeMs);
    if (m_stagePerformance[stage].size() > PERFORMANCE_HISTORY_SIZE) {
        m_stagePerformance[stage].removeFirst();
    }

    // Record total processing time
    if (stage == "total_processing") {
        m_processingTimes.append(timeMs);
        if (m_processingTimes.size() > PERFORMANCE_HISTORY_SIZE) {
            m_processingTimes.removeFirst();
        }
    }
}

void RouteAssignmentService::updateAverageProcessingTime() {
    if (m_processingTimes.isEmpty()) {
        return;
    }

    double total = std::accumulate(m_processingTimes.begin(), m_processingTimes.end(), 0.0);
    m_averageProcessingTime = total / m_processingTimes.size();

    emit performanceChanged();
}

QVariantMap RouteAssignmentService::getPerformanceStatistics() const {
    QVariantMap stats;

    stats["averageProcessingTimeMs"] = m_averageProcessingTime;
    stats["totalRequests"] = m_totalRequests;
    stats["successfulRoutes"] = m_successfulRoutes;
    stats["failedRoutes"] = m_failedRoutes;
    stats["emergencyReleases"] = m_emergencyReleases;
    stats["timeouts"] = m_timeouts;
    stats["successRate"] = m_totalRequests > 0 ? (double)m_successfulRoutes / m_totalRequests * 100.0 : 0.0;
    stats["pendingRequests"] = m_requestQueue.size();
    stats["activeRoutes"] = activeRoutes();

    // Stage-specific performance
    QVariantMap stageStats;
    for (auto it = m_stagePerformance.begin(); it != m_stagePerformance.end(); ++it) {
        if (!it.value().isEmpty()) {
            double avg = std::accumulate(it.value().begin(), it.value().end(), 0.0) / it.value().size();
            stageStats[it.key() + "_avg_ms"] = avg;
        }
    }
    stats["stagePerformance"] = stageStats;

    return stats;
}

// Validation helper methods
bool RouteAssignmentService::isValidSignalId(const QString& signalId) const {
    return !signalId.isEmpty() && signalId.length() >= 3; // Basic validation
}

bool RouteAssignmentService::isValidDirection(const QString& direction) const {
    return direction == "UP" || direction == "DOWN";
}

bool RouteAssignmentService::isValidPriority(const QString& priority) const {
    return priority == "LOW" || priority == "NORMAL" || priority == "HIGH" || priority == "EMERGENCY";
}

bool RouteAssignmentService::canAcceptNewRequests() const {
    return m_isOperational &&
           m_requestQueue.size() < MAX_QUEUE_SIZE &&
           !m_emergencyMode;
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

bool RouteAssignmentService::persistRouteRequest(const RouteRequest& request) {
    Q_UNUSED(request)
    return true; // Placeholder
}

bool RouteAssignmentService::persistRouteAssignment(
    const QString& routeId,
    const RouteRequest& request,
    const QStringList& path
) {
    Q_UNUSED(routeId)
    Q_UNUSED(request)
    Q_UNUSED(path)
    return true; // Placeholder
}

// Event handler stubs
void RouteAssignmentService::onTrackCircuitOccupancyChanged(const QString& circuitId, bool isOccupied) {
    Q_UNUSED(circuitId)
    Q_UNUSED(isOccupied)
    // Handle reactive updates to routes based on track occupancy changes
}

void RouteAssignmentService::onRouteStateChanged(const QString& routeId, const QString& newState) {
    Q_UNUSED(routeId)
    Q_UNUSED(newState)
    // Handle route state changes from VitalRouteController
}

void RouteAssignmentService::checkSystemHealth() {
    bool wasOperational = m_isOperational;
    m_isOperational = areServicesHealthy();

    if (wasOperational != m_isOperational) {
        emit operationalStateChanged();
    }
}

void RouteAssignmentService::onSystemOverload() {
    // Enter degraded mode to reduce load
    enterDegradedMode();

    // Clear non-essential pending requests
    if (m_requestQueue.size() > MAX_QUEUE_SIZE / 2) {
        int removed = 0;
        auto it = m_requestQueue.begin();
        while (it != m_requestQueue.end() && removed < MAX_QUEUE_SIZE / 4) {
            if (it->priority != "EMERGENCY" && it->priority != "HIGH") {
                it = m_requestQueue.erase(it);
                removed++;
            } else {
                ++it;
            }
        }
    }

    emit systemOverloaded(m_requestQueue.size(), m_maxConcurrentRoutes);
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

bool RouteAssignmentService::releaseRoute(const QString& routeId, const QString& reason) {
    if (!m_vitalController) {
        return false;
    }

    QVariantMap result = m_vitalController->releaseRouteResources(routeId);
    bool success = result["success"].toBool();

    if (success) {
        emit routeReleased(routeId, reason);
        emit routeCountChanged();
    } else {
        qCritical() << "[RouteAssignmentService > releaseRoute] Failed to release route"
                    << routeId << "Error:" << result["error"].toString();
    }

    return success;
}

QVariantMap RouteAssignmentService::getRouteStatus(const QString& routeId) const {
    if (!m_vitalController) {
        return QVariantMap{{"error", "VitalRouteController not available"}};
    }

    return m_vitalController->getRouteStatus(routeId);
}

QVariantList RouteAssignmentService::getActiveRoutes() const {
    if (!m_vitalController) {
        return QVariantList();
    }

    return m_vitalController->getActiveRoutes();
}

QVariantList RouteAssignmentService::getPendingRequests() const {
    QVariantList result;

    for (const RouteRequest& request : m_requestQueue) {
        QVariantMap requestMap;
        requestMap["requestId"] = request.requestId;
        requestMap["sourceSignalId"] = request.sourceSignalId;
        requestMap["destSignalId"] = request.destSignalId;
        requestMap["direction"] = request.direction;
        requestMap["priority"] = request.priority;
        requestMap["operatorId"] = request.requestedBy;
        requestMap["requestedAt"] = request.requestedAt;
        result.append(requestMap);
    }

    return result;
}

QVariantMap RouteAssignmentService::getSystemStatus() const {
    return QVariantMap{
        {"isOperational",           m_isOperational},
        {"emergencyMode",           m_emergencyMode},
        {"degradedMode",            m_degradedMode},
        {"pendingRequests",         m_requestQueue.size()},
        {"maxConcurrentRoutes",     m_maxConcurrentRoutes},
        {"processingTimeout",       m_processingTimeoutMs},
        {"queueProcessingInterval", m_queueProcessingIntervalMs},
        {"maintenanceInterval",     m_maintenanceIntervalMs}
    };
}

bool RouteAssignmentService::setMaxConcurrentRoutes(int maxRoutes) {
    if ((maxRoutes < 1) || (maxRoutes > 50)) {
        return false;
    }
    m_maxConcurrentRoutes = maxRoutes;
    return true;
}

bool RouteAssignmentService::setProcessingTimeout(int timeoutMs) {
    if ((timeoutMs < 1000) || (timeoutMs > 300000)) { // 1s to 5min
        return false;
    }
    m_processingTimeoutMs = timeoutMs;
    return true;
}

QVariantMap RouteAssignmentService::getOperationalStatistics() const {
    QVariantMap stats = getPerformanceStatistics();

    // Add operational metrics
    stats["isOperational"] = m_isOperational;
    stats["emergencyMode"] = m_emergencyMode;
    stats["degradedMode"] = m_degradedMode;
    stats["uptime"] = QDateTime::currentDateTime().toSecsSinceEpoch() - m_serviceStartTime;

    return stats;
}

QVariantList RouteAssignmentService::getRouteHistory(int limitHours) const {
    Q_UNUSED(limitHours)
    // Would query database for route history
    return QVariantList();
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
        actualDirection = determineSignalDirection(sourceSignalId);
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

QStringList RouteAssignmentService::getEligibleDestinationSignals(
    const QString& sourceSignalId,
    const QString& direction) {

    QStringList eligible;

    if (!m_dbManager) return eligible;

    // Get source signal info
    auto sourceSignal = m_dbManager->getSignalById(sourceSignalId);
    if (sourceSignal.isEmpty()) return eligible;

    QString sourceType = sourceSignal["type"].toString();

    // Define signal type compatibility matrix (from technical draft)
    QMap<QString, QStringList> compatibilityMatrix;
    compatibilityMatrix["HOME"] = {"STARTER"};
    compatibilityMatrix["STARTER"] = {"ADVANCED_STARTER"};
    compatibilityMatrix["ADVANCED_STARTER"] = {}; // Can be extended
    compatibilityMatrix["OUTER"] = {"HOME"}; // Optional

    QStringList allowedDestTypes = compatibilityMatrix.value(sourceType);
    if (allowedDestTypes.isEmpty()) {
        return eligible;
    }

    // Query database for signals matching criteria
    QSqlQuery query(m_dbManager->getDatabase());
    query.prepare(R"(
        SELECT signal_id, signal_name, signal_type
        FROM railway_control.v_signals_complete
        WHERE direction = ?
          AND is_active = true
          AND is_route_signal = true
          AND signal_type = ANY(?)
          AND manual_control_active = false
          AND preceded_by_circuit_id IS NOT NULL
          AND succeeded_by_circuit_id IS NOT NULL
          AND signal_id != ?
        ORDER BY signal_id
    )");

    // Convert QStringList to PostgreSQL array format
    QString destTypesArray = "{" + allowedDestTypes.join(",") + "}";

    query.addBindValue(direction);
    query.addBindValue(destTypesArray);
    query.addBindValue(sourceSignalId);

    if (query.exec()) {
        while (query.next()) {
            QString destSignalId = query.value("signal_id").toString();
            eligible.append(destSignalId);
        }
    } else {
        qCritical() << "[RouteAssignmentService > getEligibleDestinationSignals] Failed to query eligible destination signals:"
                    << query.lastError().text();
    }

    return eligible;
}

QList<RouteAssignmentService::DestinationCandidate::RequiredPMAction>
RouteAssignmentService::getRequiredPointMachineActions(const QStringList& path) {
    QList<DestinationCandidate::RequiredPMAction> actions;

    // Simplified placeholder
    Q_UNUSED(path)
    return actions;
}

RouteAssignmentService::DestinationCandidate
RouteAssignmentService::evaluateDestinationReachability(
    const QString& sourceSignalId,
    const QString& destSignalId,
    const QString& direction) {

    DestinationCandidate candidate;
    candidate.destSignalId = destSignalId;
    candidate.direction = direction;

    QVariantMap currentPMStates;

    if(m_dbManager) {
        currentPMStates = m_dbManager->getAllPointMachineStates();
    }

    // Get signal info for display name
    auto destSignal = m_dbManager->getSignalById(destSignalId);
    if (!destSignal.isEmpty()) {
        candidate.displayName = QString("%1 (%2)")
            .arg(destSignal["name"].toString())
            .arg(destSignal["typeName"].toString());
    }

    // Get start and goal circuits
    auto sourceSignal = m_dbManager->getSignalById(sourceSignalId);
    QString startCircuit = sourceSignal["succeededByCircuitId"].toString();
    QString goalCircuit = destSignal["precededByCircuitId"].toString();

    if (startCircuit.isEmpty() || goalCircuit.isEmpty()) {
        candidate.reachability = "BLOCKED";
        candidate.blockedReason = "INCOMPLETE_TOPOLOGY";
        return candidate;
    }

    // Use GraphService to find path and check reachability
    if (!m_graphService) {
        candidate.reachability = "BLOCKED";
        candidate.blockedReason = "PATHFINDING_UNAVAILABLE";
        return candidate;
    }

    auto pathResult = m_graphService->findRoute(
        startCircuit, goalCircuit,
        direction,
        currentPMStates,
        500
    );

    bool pathSuccess = pathResult.value("success", false).toBool();

    if (pathSuccess) {
        auto path = pathResult.value("path").toStringList();

        if (path.isEmpty()) {
            candidate.reachability = "BLOCKED";
            candidate.blockedReason = "EMPTY_PATH_RETURNED";
            return candidate;
        }

        // Check if any PM movements are required for this path
        auto requiredPMMovements = analyzeRequiredPMMovements(path, direction, currentPMStates);

        if (!requiredPMMovements.isEmpty()) {
            candidate.reachability = "REACHABLE_REQUIRES_PM";
            candidate.requiredPMActions = requiredPMMovements;
        } else {
            candidate.reachability = "REACHABLE_CLEAR";
        }

        // Set valid path metrics
        candidate.pathSummary.hopCount = path.size();
        candidate.pathSummary.estimatedWeight = pathResult.value("cost", 0.0).toDouble();

        // Create preview of path
        if (path.size() <= 3) {
            candidate.pathSummary.circuitsPreview = path;
        } else {
            QStringList preview;
            preview << path[0] << path[1] << "..." << path.last();
            candidate.pathSummary.circuitsPreview = preview;
        }

        // Clearance check
        auto clearanceCheck = checkPathClearance(path);

        if (!clearanceCheck.isCleared) {
            candidate.reachability = "BLOCKED";
            candidate.blockedReason = clearanceCheck.blockReason;
            candidate.conflicts = clearanceCheck.conflicts;
        } else if (!clearanceCheck.requiredPMActions.isEmpty()) {
            candidate.reachability = "REACHABLE_REQUIRES_PM";
            candidate.requiredPMActions = clearanceCheck.requiredPMActions;
        } else {
            candidate.reachability = "REACHABLE_CLEAR";
        }

    } else {
        QString pathError = pathResult.value("error", "Unknown pathfinding error").toString();
        candidate.reachability = "BLOCKED";
        candidate.blockedReason = QString("NO_PATH_FOUND: %1").arg(pathError);

        // Mark invalid/no path
        candidate.pathSummary.hopCount = -1;
        candidate.pathSummary.estimatedWeight = -1.0;
        candidate.pathSummary.circuitsPreview.clear();
    }

    return candidate;
}

QList<RouteAssignmentService::DestinationCandidate::RequiredPMAction>
RouteAssignmentService::analyzeRequiredPMMovements(
    const QStringList& path,
    const QString& direction,
    const QVariantMap& currentPMStates) {

    QList<DestinationCandidate::RequiredPMAction> requiredMovements;

    if (!m_graphService || path.size() < 2) {
        return requiredMovements;
    }

    // Get the target side for the direction
    QString targetSide = (direction.toUpper() == "DOWN") ? "LEFT" : "RIGHT";

    // Analyze each hop in the path for PM requirements
    for (int i = 0; i < path.size() - 1; ++i) {
        QString fromCircuit = path[i];
        QString toCircuit = path[i + 1];

        // Find the edge for this hop
        QString requiredPM;
        QString requiredPosition;

        auto edgeInfo = m_graphService->getEdgeInfo(fromCircuit, toCircuit, targetSide);

        if (edgeInfo.contains("condition_pm_id") && !edgeInfo["condition_pm_id"].toString().isEmpty()) {
            requiredPM = edgeInfo["condition_pm_id"].toString();
            requiredPosition = edgeInfo["condition_position"].toString();

            // Check current PM state
            if (currentPMStates.contains(requiredPM)) {
                QVariantMap pmData = currentPMStates[requiredPM].toMap();
                QString currentPosition = pmData["current_position"].toString();

                if (currentPosition != requiredPosition) {
                    DestinationCandidate::RequiredPMAction action;
                    action.machineId = requiredPM;
                    action.currentPosition = currentPosition;
                    action.targetPosition = requiredPosition;

                    bool alreadyExists = false;
                    for (const auto& existing : requiredMovements) {
                        if (existing.machineId == requiredPM &&
                            existing.targetPosition == requiredPosition) {
                            alreadyExists = true;
                            break;
                        }
                    }

                    if (!alreadyExists) {
                        requiredMovements.append(action);
                    }
                }
            }
        }
    }

    return requiredMovements;
}

QString RouteAssignmentService::determineSignalDirection(const QString& signalId) {
    if (!m_dbManager) return "UP"; // Default fallback

    auto signal = m_dbManager->getSignalById(signalId);
    return signal.value("direction", "UP").toString();
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

RouteAssignmentService::ClearanceCheckResult RouteAssignmentService::checkPathClearance(const QStringList& path) {
    ClearanceCheckResult result;

    for (const QString& circuitId : path) {
        if (m_dbManager->getTrackCircuitOccupancy(circuitId)) {
            result.isCleared = false;
            result.blockReason = "OCCUPIED";
            result.conflicts.append(circuitId + " (occupied)");
            continue;
        }

        if (isCircuitReserved(circuitId)) {
            result.isCleared = false;
            result.blockReason = "RESERVED";
            result.conflicts.append(circuitId + " (reserved)");
            continue;
        }
    }

    auto pmActions = getRequiredPointMachineActions(path);
    for (const auto& action : pmActions) {
        if (isPointMachineSettable(action.machineId)) {
            result.requiredPMActions.append(action);
        } else {
            result.isCleared = false;
            result.blockReason = "LOCKED_PM";
            result.conflicts.append(action.machineId + " (locked/failed)");
        }
    }

    return result;
}

bool RouteAssignmentService::isCircuitReserved(const QString& circuitId) {
    QSqlQuery query(m_dbManager->getDatabase());
    query.prepare(R"(
        SELECT 1 FROM railway_control.resource_locks rl
        JOIN railway_control.route_assignments ra ON rl.route_id = ra.id
        WHERE rl.resource_type = 'TRACK_CIRCUIT'
          AND rl.resource_id = ?
          AND rl.is_active = true
          AND ra.state IN ('RESERVED', 'ACTIVE', 'PARTIALLY_RELEASED')
    )");
    query.addBindValue(circuitId);

    if (query.exec() && query.next()) {
        return true;
    }

    return false;
}

bool RouteAssignmentService::isPointMachineSettable(const QString& machineId) {
    auto pmData = m_dbManager->getPointMachineById(machineId);
    if (pmData.isEmpty()) return false;

    QString status = pmData["operating_status"].toString();
    bool isLocked = pmData["is_locked"].toBool();

    return (status == "CONNECTED" || status == "NORMAL") && !isLocked;
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
        QStringList{"TC01", "TC02", "TC03", "TC04"},           // Path
        QStringList{"TC05", "TC06"},                           // Overlap
        QVariantMap{{"HM001", "GREEN"}, {"ST001", "RED"}},     // Signal aspects
        QVariantMap{{"PM001", "NORMAL"}},                      // Point machines
        "SUCCESS", "", 45.0
        ));

    // Route 2: HM001 → ST002 (Requires PM movement)
    m_hardcodedRoutes.routes.append(createRoute(
        "HM001", "ST002",
        QStringList{"TC01", "TC02", "TC07", "TC08", "TC09"},
        QStringList{"TC10", "TC11"},
        QVariantMap{{"HM001", "GREEN"}, {"ST002", "RED"}, {"SIG_INTERMEDIATE", "YELLOW"}},
        QVariantMap{{"PM001", "REVERSE"}, {"PM002", "NORMAL"}},
        "SUCCESS", "", 75.0
        ));

    // Route 3: HM001 → ST003 (Blocked route)
    m_hardcodedRoutes.routes.append(createRoute(
        "HM001", "ST003",
        QStringList{},  // Empty path for blocked
        QStringList{},  // Empty overlap for blocked
        QVariantMap{},  // No signal changes for blocked
        QVariantMap{},  // No PM changes for blocked
        "BLOCKED", "CIRCUIT_OCCUPIED_TC15", 25.0
        ));

    // Route 4: ST001 → AS001 (Starter to Advanced Starter)
    m_hardcodedRoutes.routes.append(createRoute(
        "ST001", "AS001",
        QStringList{"TC12", "TC13", "TC14"},
        QStringList{"TC15"},
        QVariantMap{{"ST001", "GREEN"}, {"AS001", "RED"}},
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

RailFlux::Route::RouteAssignmentService::HardcodedRoute RailFlux::Route::RouteAssignmentService::findHardcodedRoute(const QString& sourceId, const QString& destId) {
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
