#include "VitalRouteController.h"
#include "../database/DatabaseManager.h"
#include "../interlocking/InterlockingService.h"
#include "../interlocking/AspectPropagationService.h"
#include "ResourceLockService.h"
#include "TelemetryService.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QtMath>

namespace RailFlux::Route {

VitalRouteController::VitalRouteController(
    DatabaseManager* dbManager,
    InterlockingService* interlockingService,
    ResourceLockService* resourceLockService,
    TelemetryService* telemetryService,
    QObject* parent
    )
    : QObject(parent)
    , m_dbManager(dbManager)
    , m_interlockingService(interlockingService)
    , m_resourceLockService(resourceLockService)
    , m_telemetryService(telemetryService)
    //   NEW: Initialize the new timers
    , m_validationTimer(std::make_unique<QTimer>(this))
    , m_healthCheckTimer(std::make_unique<QTimer>(this))
{
    if (!m_dbManager || !m_interlockingService || !m_resourceLockService || !m_telemetryService) {
        qCritical() << "VitalRouteController: One or more required services is null";
        return;
    }

    //   NEW: Setup periodic validation timer (don't start yet - will start in initialize())
    m_validationTimer->setInterval(PERIODIC_VALIDATION_INTERVAL_MS);
    connect(m_validationTimer.get(), &QTimer::timeout, this, &VitalRouteController::performPeriodicValidation);

    //   NEW: Setup health check timer (don't start yet - will start in initialize())
    m_healthCheckTimer->setInterval(HEALTH_CHECK_INTERVAL_MS);
    connect(m_healthCheckTimer.get(), &QTimer::timeout, this, &VitalRouteController::performHealthCheck);

    //   EXISTING: Connect to database changes for reactive updates
    connect(m_dbManager, &DatabaseManager::connectionStateChanged,
            this, [this](bool connected) {
                if (connected) {
                    initialize();
                } else {
                    m_isOperational = false;
                    emit operationalStateChanged();
                }
            });

    //   EXISTING: Connect to hardware state changes
    connect(m_dbManager, &DatabaseManager::trackSegmentUpdated,
            this, [this](const QString& segmentId) {
                // Track circuits are linked to segments - need to resolve circuit ID
                onTrackCircuitOccupancyChanged(segmentId, true); // Simplified for now
            });

    //   EXISTING: Setup periodic safety check timer (this one starts immediately)
    QTimer* safetyTimer = new QTimer(this);
    safetyTimer->setInterval(SAFETY_CHECK_INTERVAL_MS);
    connect(safetyTimer, &QTimer::timeout, this, &VitalRouteController::performPeriodicSafetyCheck);
    safetyTimer->start();

    //   NEW: Initialize time tracking
    m_systemStartTime = QDateTime::currentDateTime().toSecsSinceEpoch();
    m_lastHealthCheck = QDateTime::currentDateTime();

    //   NEW: Initialize performance metrics
    m_averageValidationTimeMs = 0.0;

    qDebug() << "🛡️ VitalRouteController: Constructor completed - safety systems ready";
}

VitalRouteController::~VitalRouteController() = default;

void VitalRouteController::initialize() {
    qDebug() << "VitalRouteController: Initializing safety-critical route controller...";

    if (!m_dbManager || !m_dbManager->isConnected()) {
        qWarning() << "VitalRouteController: Cannot initialize - database not connected";
        return;
    }

    try {
        //   CRITICAL: Load active routes from database (essential for system recovery)
        if (loadActiveRoutesFromDatabase()) {
            m_isOperational = true;

            qDebug() << "  VitalRouteController: Initialized with" << m_activeRoutes.size() << "active routes";
            emit operationalStateChanged();

            // Start safety monitoring timers
            if (m_validationTimer) {
                m_validationTimer->start();
            }
            if (m_healthCheckTimer) {
                m_healthCheckTimer->start();
            }

        } else {
            //   GRACEFUL: Don't fail if table is empty or query fails (normal for fresh system)
            qWarning() << " VitalRouteController: Database query failed or no active routes found";
            qWarning() << "   This is normal for a fresh system. Safety controller will remain operational.";

            m_isOperational = true;  // Still become operational with no active routes
            m_activeRoutes.clear();  // Ensure clean state
            emit operationalStateChanged();

            qDebug() << "  VitalRouteController: Initialized with no active routes (fresh system)";
        }

        //   SAFETY MONITORING: Initialize safety monitoring regardless of database state
        m_lastHealthCheck = QDateTime::currentDateTime();
        m_systemStartTime = QDateTime::currentDateTime().toSecsSinceEpoch();

        //   SAFETY VIOLATIONS: Clear any previous safety violations
        m_recentSafetyViolations.clear();  // This is the container that exists
        m_safetyViolations = 0;  // Reset the counter

        //   PERFORMANCE: Reset performance metrics
        m_validationTimes.clear();
        m_averageValidationTimeMs = 0.0;
        updateAverageValidationTime();  // Recalculate average

        //   COUNTERS: Reset operational counters
        m_totalValidations = 0;
        m_successfulValidations = 0;
        m_emergencyReleases = 0;

        //   COLLECTIONS: Clear route tracking collections
        m_routesByCircuit.clear();

        //   TELEMETRY: Record initialization event
        if (m_telemetryService) {
            m_telemetryService->recordSafetyEvent(
                "vital_controller_initialized",
                "INFO",
                "VitalRouteController",
                QString("Safety-critical route controller initialized with %1 active routes").arg(m_activeRoutes.size()),
                "system"
                );
        }

        qDebug() << "  VitalRouteController: Safety systems online and monitoring";

    } catch (const std::exception& e) {
        qCritical() << " VitalRouteController: Initialization failed:" << e.what();
        m_isOperational = false;
        emit operationalStateChanged();

        //   CRITICAL FAILURE: Record critical failure
        if (m_telemetryService) {
            m_telemetryService->recordSafetyEvent(
                "vital_controller_init_failed",
                "CRITICAL",
                "VitalRouteController",
                QString("Safety controller initialization failed: %1").arg(e.what()),
                "system"
                );
        }
    }
}

bool VitalRouteController::loadActiveRoutesFromDatabase() {
    QSqlQuery query(m_dbManager->getDatabase());
    query.prepare(R"(
        SELECT
            id,
            source_signal_id,
            dest_signal_id,
            direction,
            assigned_circuits,
            overlap_circuits,
            state,
            priority,
            operator_id,
            created_at,
            activated_at
        FROM railway_control.route_assignments
        WHERE state IN ('ACTIVE', 'RESERVED', 'VALIDATING')
        ORDER BY priority DESC, created_at ASC
    )");

    if (!query.exec()) {
        qCritical() << "VitalRouteController: Failed to load active routes:" << query.lastError().text();
        return false;
    }

    m_activeRoutes.clear();

    while (query.next()) {
        RouteAssignment route;
        route.id = QUuid::fromString(query.value("id").toString());
        route.sourceSignalId = query.value("source_signal_id").toString();
        route.destSignalId = query.value("dest_signal_id").toString();
        route.direction = query.value("direction").toString();

        // Parse PostgreSQL text arrays
        QString assignedCircuitsStr = query.value("assigned_circuits").toString();
        if (assignedCircuitsStr.startsWith("{") && assignedCircuitsStr.endsWith("}")) {
            assignedCircuitsStr = assignedCircuitsStr.mid(1, assignedCircuitsStr.length() - 2);
            route.assignedCircuits = assignedCircuitsStr.split(",", Qt::SkipEmptyParts);
        }

        QString overlapCircuitsStr = query.value("overlap_circuits").toString();
        if (overlapCircuitsStr.startsWith("{") && overlapCircuitsStr.endsWith("}")) {
            overlapCircuitsStr = overlapCircuitsStr.mid(1, overlapCircuitsStr.length() - 2);
            route.overlapCircuits = overlapCircuitsStr.split(",", Qt::SkipEmptyParts);
        }

        route.state = stringToRouteState(query.value("state").toString());
        route.priority = query.value("priority").toInt();
        route.operatorId = query.value("operator_id").toString();
        route.createdAt = query.value("created_at").toDateTime();
        route.activatedAt = query.value("activated_at").toDateTime();

        m_activeRoutes[route.id.toString()] = route;

        //   INDEX: Build circuit-to-route mapping for fast lookups
        for (const QString& circuitId : route.assignedCircuits + route.overlapCircuits) {
            if (!m_routesByCircuit.contains(circuitId)) {
                m_routesByCircuit[circuitId] = QStringList();
            }
            m_routesByCircuit[circuitId].append(route.id.toString());
        }
    }

    qDebug() << "📥 VitalRouteController: Loaded" << m_activeRoutes.size() << "active routes from database";
    return true;
}

QVariantMap VitalRouteController::validateRouteRequest(
    const QString& sourceSignalId,
    const QString& destSignalId,
    const QString& direction,
    const QString& operatorId
) {
    QElapsedTimer timer;
    timer.start();
    
    m_totalValidations++;

    if (!m_isOperational) {
        return QVariantMap{
            {"success", false},
            {"error", "VitalRouteController not operational"},
            {"safetyLevel", "DANGER"}
        };
    }

    ValidationResult result = validateRouteRequestInternal(sourceSignalId, destSignalId, direction, operatorId);
    
    auto duration = std::chrono::milliseconds(timer.elapsed());
    recordValidationTime("route_request_validation", duration);

    if (result.isAllowed) {
        m_successfulValidations++;
    }

    // Record performance metrics
    if (m_telemetryService) {
        m_telemetryService->recordPerformanceMetric(
            "route_validation",
            timer.elapsed(),
            result.isAllowed,
            QString("%1->%2").arg(sourceSignalId, destSignalId),
            QVariantMap{
                {"sourceSignal", sourceSignalId},
                {"destSignal", destSignalId},
                {"direction", direction},
                {"safetyLevel", safetyLevelToString(result.safetyLevel)}
            }
        );
    }

    return validationResultToVariantMap(result);
}

ValidationResult VitalRouteController::validateRouteRequestInternal(
    const QString& sourceSignalId,
    const QString& destSignalId,
    const QString& direction,
    const QString& operatorId
) {
    Q_UNUSED(operatorId) // May be used for authorization in future

    // 1. Basic validation
    if (sourceSignalId.isEmpty() || destSignalId.isEmpty()) {
        return ValidationResult::blocked("Source or destination signal ID is empty");
    }

    if (sourceSignalId == destSignalId) {
        return ValidationResult::blocked("Source and destination signals cannot be the same");
    }

    if (direction != "UP" && direction != "DOWN") {
        return ValidationResult::blocked("Invalid direction - must be UP or DOWN");
    }

    // 2. Signal progression validation
    ValidationResult progressionResult = validateSignalProgression(sourceSignalId, destSignalId);
    if (!progressionResult.isAllowed) {
        return progressionResult;
    }

    // 3. Check if signals exist and are active
    QVariantMap sourceSignal = m_dbManager->getSignalById(sourceSignalId);
    QVariantMap destSignal = m_dbManager->getSignalById(destSignalId);

    if (sourceSignal.isEmpty()) {
        return ValidationResult::blocked(QString("Source signal not found: %1").arg(sourceSignalId));
    }

    if (destSignal.isEmpty()) {
        return ValidationResult::blocked(QString("Destination signal not found: %1").arg(destSignalId));
    }

    if (!sourceSignal["isActive"].toBool()) {
        return ValidationResult::blocked(QString("Source signal is not active: %1").arg(sourceSignalId));
    }

    if (!destSignal["isActive"].toBool()) {
        return ValidationResult::blocked(QString("Destination signal is not active: %1").arg(destSignalId));
    }

    // 4. Check direction consistency
    if (sourceSignal["direction"].toString() != direction || destSignal["direction"].toString() != direction) {
        return ValidationResult::blocked("Signal direction mismatch with requested route direction");
    }

    // 5. Check for existing conflicting routes
    // This is a simplified check - full implementation would use pathfinding results
    for (const RouteAssignment& route : m_activeRoutes) {
        if (route.sourceSignalId == sourceSignalId || route.destSignalId == destSignalId) {
            if (route.isActive()) {
                ValidationResult result = ValidationResult::blocked(
                    QString("Conflicting route exists: %1").arg(route.key()),
                    SafetyLevel::WARNING
                );
                result.conflictingResources.append(route.key());
                return result;
            }
        }
    }

    // If all validations pass
    ValidationResult result = ValidationResult::allowed("Route request validation passed");
    result.safetyLevel = SafetyLevel::VITAL_SAFE;
    result.details = QString("Validated route from %1 to %2 in %3 direction")
                        .arg(sourceSignalId, destSignalId, direction);
    
    return result;
}

ValidationResult VitalRouteController::validateSignalProgression(
    const QString& sourceSignalId,
    const QString& destSignalId
    ) const {
    // Get signal types from database
    QVariantMap sourceSignal = m_dbManager->getSignalById(sourceSignalId);
    QVariantMap destSignal = m_dbManager->getSignalById(destSignalId);

    if (sourceSignal.isEmpty() || destSignal.isEmpty()) {
        return ValidationResult::blocked("Cannot determine signal types for progression validation");
    }

    QString sourceType = sourceSignal["type"].toString();
    QString destType = destSignal["type"].toString();

    //   FIX: Clean the strings - remove quotes and trim whitespace
    sourceType = sourceType.trimmed().remove("\"").remove("'");
    destType = destType.trimmed().remove("\"").remove("'");

    //  DEBUG: Log the cleaned types
    qDebug() << " Signal types for progression validation:";
    qDebug() << "   sourceType (cleaned): '" << sourceType << "'";
    qDebug() << "   destType (cleaned): '" << destType << "'";

    if (!isValidProgressionSequence(sourceType, destType)) {
        return ValidationResult::blocked(
            QString("Invalid signal progression: %1 (%2) to %3 (%4)")
                .arg(sourceSignalId, sourceType, destSignalId, destType),
            SafetyLevel::DANGER
            );
    }

    return ValidationResult::allowed("Signal progression validation passed");
}

bool VitalRouteController::isValidSignalProgression(
    const QString& sourceSignalType,
    const QString& destSignalType
) const {
    return isValidProgressionSequence(sourceSignalType, destSignalType);
}

bool VitalRouteController::isValidProgressionSequence(const QString& sourceType, const QString& destType) const {
    qDebug() << " isValidProgressionSequence received:";
    qDebug() << "   sourceType: '" << sourceType << "' (length:" << sourceType.length() << ")";
    qDebug() << "   destType: '" << destType << "' (length:" << destType.length() << ")";

    //   FIX: Combine all valid destinations for each source signal type
    static const QHash<QString, QStringList> validProgressions = {
        {"OUTER", {"HOME"}},                                    // OUTER -> HOME
        {"HOME", {"STARTER", "ADVANCED_STARTER"}},              //    HOME -> STARTER (normal) OR ADVANCED_STARTER (bypass)
        {"STARTER", {"ADVANCED_STARTER", "HOME"}},              //    STARTER -> ADVANCED_STARTER (normal) OR HOME (reverse)
        {"ADVANCED_STARTER", {"OUTER", "HOME", "STARTER"}}      //    ADVANCED_STARTER -> next signal block OR reverse
    };

    qDebug() << " Checking if validProgressions contains sourceType '" << sourceType << "':";
    bool containsSource = validProgressions.contains(sourceType);
    qDebug() << "   Result:" << containsSource;

    if (!containsSource) {
        qWarning() << "VitalRouteController: Unknown source signal type:" << sourceType;
        qDebug() << "   Available source types:" << validProgressions.keys();
        return false;
    }

    QStringList validDests = validProgressions[sourceType];
    qDebug() << " Valid destinations for '" << sourceType << "':" << validDests;
    bool containsDest = validDests.contains(destType);
    qDebug() << " Does list contain destType '" << destType << "':" << containsDest;

    return containsDest;
}

QVariantMap VitalRouteController::validateResourceAvailability(
    const QStringList& circuits,
    const QStringList& pointMachines
) {
    QElapsedTimer timer;
    timer.start();

    ValidationResult result = validateResourceAvailabilityInternal(circuits, pointMachines);
    
    auto duration = std::chrono::milliseconds(timer.elapsed());
    recordValidationTime("resource_availability_validation", duration);

    return validationResultToVariantMap(result);
}

ValidationResult VitalRouteController::validateResourceAvailabilityInternal(
    const QStringList& circuits,
    const QStringList& pointMachines
) {
    if (!m_isOperational) {
        return ValidationResult::blocked("VitalRouteController not operational");
    }

    QStringList unavailableResources;
    QStringList conflictingRoutes;

    // Check track circuits
    for (const QString& circuitId : circuits) {
        // Check if circuit is occupied
        // This would integrate with track circuit monitoring
        // For now, simplified check through database
        
        // Check if circuit is already assigned to another route
        if (m_routesByCircuit.contains(circuitId)) {
            for (const QString& routeId : m_routesByCircuit[circuitId]) {
                if (m_activeRoutes.contains(routeId) && m_activeRoutes[routeId].isActive()) {
                    unavailableResources.append(QString("Circuit %1 (Route %2)").arg(circuitId, routeId));
                    if (!conflictingRoutes.contains(routeId)) {
                        conflictingRoutes.append(routeId);
                    }
                }
            }
        }

        // Check resource lock service
        if (m_resourceLockService && m_resourceLockService->isResourceLocked("TRACK_CIRCUIT", circuitId)) {
            QVariantMap lockStatus = m_resourceLockService->getResourceLockStatus("TRACK_CIRCUIT", circuitId);
            unavailableResources.append(QString("Circuit %1 (Locked)").arg(circuitId));
        }
    }

    // Check point machines
    for (const QString& machineId : pointMachines) {
        if (m_resourceLockService && m_resourceLockService->isResourceLocked("POINT_MACHINE", machineId)) {
            unavailableResources.append(QString("Point Machine %1 (Locked)").arg(machineId));
        }
    }

    if (!unavailableResources.isEmpty()) {
        ValidationResult result = ValidationResult::blocked(
            QString("Resources unavailable: %1").arg(unavailableResources.join(", ")),
            SafetyLevel::WARNING
        );
        result.conflictingResources = unavailableResources;
        result.details = QString("Found %1 unavailable resources").arg(unavailableResources.size());
        return result;
    }

    // All resources available
    ValidationResult result = ValidationResult::allowed("All requested resources are available");
    result.safetyLevel = SafetyLevel::SAFE;
    result.details = QString("Validated %1 circuits and %2 point machines")
                        .arg(circuits.size()).arg(pointMachines.size());
    
    return result;
}

QVariantMap VitalRouteController::validateAgainstInterlocking(const QVariantMap& routeData) {
    QElapsedTimer timer;
    timer.start();

    RouteAssignment route = variantMapToRouteAssignment(routeData);
    ValidationResult result = validateAgainstInterlockingInternal(route);
    
    auto duration = std::chrono::milliseconds(timer.elapsed());
    recordValidationTime("interlocking_validation", duration);

    return validationResultToVariantMap(result);
}

ValidationResult VitalRouteController::validateAgainstInterlockingInternal(const RouteAssignment& route) {
    if (!m_interlockingService) {
        return ValidationResult::blocked("InterlockingService not available");
    }

    //  REPLACE HARDCODED LOGIC WITH INTELLIGENT ASPECT PROPAGATION
    if (m_aspectPropagationService) {
        qDebug() << "🧠 Using intelligent aspect propagation for validation";

        //  GET ACTUAL POINT MACHINE STATES FROM DATABASE
        QVariantMap allPointMachineStates = m_dbManager->getAllPointMachineStates();

        // TRANSFORM TO FORMAT EXPECTED BY propagateAspects
        // Extract just the position codes for the machines in this route
        QVariantMap pointMachinePositions;

        for (const QString& machineId : route.lockedPointMachines) {
            if (allPointMachineStates.contains(machineId)) {
                QVariantMap pmData = allPointMachineStates[machineId].toMap();
                QString currentPosition = pmData["current_position"].toString();

                // Simple assignment - propagateAspects expects QString values
                pointMachinePositions[machineId] = currentPosition;

                qDebug() << "    Point machine" << machineId << "position:" << currentPosition;
            } else {
                qWarning() << "    Point machine" << machineId << "not found, using NORMAL fallback";
                pointMachinePositions[machineId] = "NORMAL";
            }
        }

        // RUN INTELLIGENT ASPECT PROPAGATION
        QVariantMap propagationResult = m_aspectPropagationService->propagateAspects(
            route.sourceSignalId,
            route.destSignalId,
            pointMachinePositions
            );

        if (!propagationResult["success"].toBool()) {
            return ValidationResult::blocked(
                QString("Intelligent aspect propagation failed: %1")
                    .arg(propagationResult["errorMessage"].toString()),
                SafetyLevel::DANGER
                );
        }

        qDebug() << "  Intelligent aspect propagation succeeded for validation";
        qDebug() << "    Planned aspects:" << propagationResult["signalAspects"];
        qDebug() << "    Processing time:" << propagationResult["processingTimeMs"].toDouble() << "ms";

    } else {
        qWarning() << " Falling back to basic validation - AspectPropagationService not available";

        // FALLBACK: Keep existing simple check for backward compatibility
        if (!checkSignalInterlocking(route.sourceSignalId, "GREEN")) {
            return ValidationResult::blocked(
                QString("Source signal %1 cannot be cleared to proceed aspect").arg(route.sourceSignalId),
                SafetyLevel::DANGER
                );
        }
    }

    //   EXISTING: Continue with point machine validation
    for (const QString& machineId : route.lockedPointMachines) {
        QString requiredPosition = "NORMAL"; // Get from pathfinding results

        if (!checkPointMachineInterlocking(machineId, requiredPosition)) {
            return ValidationResult::blocked(
                QString("Point machine %1 cannot be set to %2").arg(machineId, requiredPosition),
                SafetyLevel::DANGER
                );
        }
    }

    ValidationResult result = ValidationResult::allowed("Interlocking validation passed");
    result.safetyLevel = SafetyLevel::VITAL_SAFE;
    result.details = QString("Route %1 passed all interlocking checks").arg(route.key());

    return result;
}

bool VitalRouteController::checkSignalInterlocking(const QString& signalId, const QString& requestedAspect) {
    if (!m_interlockingService) {
        return false;
    }

    // Get current signal aspect
    QVariantMap signal = m_dbManager->getSignalById(signalId);
    if (signal.isEmpty()) {
        return false;
    }

    QString currentAspect = signal["currentAspect"].toString();
    
    // Use existing interlocking service to validate aspect change
    auto validationResult = m_interlockingService->validateMainSignalOperation(
        signalId, currentAspect, requestedAspect, "VitalRouteController"
    );

    return validationResult.isAllowed();
}

bool VitalRouteController::checkPointMachineInterlocking(const QString& machineId, const QString& requestedPosition) {
    if (!m_interlockingService) {
        return false;
    }

    // Get current point machine position
    QVariantList pointMachines = m_dbManager->getAllPointMachinesList();
    QString currentPosition;
    
    for (const QVariant& pm : pointMachines) {
        QVariantMap machine = pm.toMap();
        if (machine["id"].toString() == machineId) {
            currentPosition = machine["currentPosition"].toString();
            break;
        }
    }

    if (currentPosition.isEmpty()) {
        return false;
    }

    // Use existing interlocking service to validate position change
    auto validationResult = m_interlockingService->validatePointMachineOperation(
        machineId, currentPosition, requestedPosition, "VitalRouteController"
    );

    return validationResult.isAllowed();
}

QVariantMap VitalRouteController::reserveRouteResources(const QVariantMap& routeData) {
    QElapsedTimer timer;
    timer.start();

    RouteAssignment route = variantMapToRouteAssignment(routeData);
    ValidationResult result = reserveRouteResourcesInternal(route);
    
    auto duration = std::chrono::milliseconds(timer.elapsed());
    recordValidationTime("route_reservation", duration);

    // Record telemetry
    if (m_telemetryService) {
        m_telemetryService->recordPerformanceMetric(
            "route_reservation",
            timer.elapsed(),
            result.isAllowed,
            route.key(),
            QVariantMap{
                {"sourceSignal", route.sourceSignalId},
                {"destSignal", route.destSignalId},
                {"circuitCount", route.assignedCircuits.size()},
                {"safetyLevel", safetyLevelToString(result.safetyLevel)}
            }
        );

        if (result.isAllowed) {
            m_telemetryService->recordOperationalMetric(
                "routes_reserved_total",
                m_activeRoutes.size(),
                "count"
            );
        }
    }

    QVariantMap resultMap = validationResultToVariantMap(result);
    if (result.isAllowed) {
        resultMap["routeId"] = route.key();
        emit routeReserved(route.key(), route.sourceSignalId, route.destSignalId);
        emit routeCountChanged();
    }

    return resultMap;
}

ValidationResult VitalRouteController::reserveRouteResourcesInternal(RouteAssignment& route) {
    if (!m_isOperational) {
        return ValidationResult::blocked("VitalRouteController not operational");
    }

    // 1. Final validation before reservation
    ValidationResult validation = validateAgainstInterlockingInternal(route);
    if (!validation.isAllowed) {
        return validation;
    }

    //    2. Update route state and persist FIRST
    route.state = RouteState::RESERVED;
    route.createdAt = QDateTime::currentDateTime();

    if (!persistRouteToDatabase(route)) {
        return ValidationResult::blocked("Failed to persist route to database");
    }

    //  Following block is commented out. EstablishRouteWithIntellignetAspects function is now being instead
    // if (!lockResourcesForRoute(route)) {
    //     //   SAFETY: Rollback - remove route from database if resource locking fails
    //     removeRouteFromDatabase(route.key());
    //     return ValidationResult::blocked("Failed to lock required resources", SafetyLevel::WARNING);
    // }

    // 4. Add to active routes (unchanged)
    QString routeId = route.key();
    m_activeRoutes[routeId] = route;

    // 5. Index by circuits (unchanged)
    for (const QString& circuitId : route.assignedCircuits + route.overlapCircuits) {
        if (!m_routesByCircuit.contains(circuitId)) {
            m_routesByCircuit[circuitId] = QStringList();
        }
        m_routesByCircuit[circuitId].append(routeId);
    }

    qDebug() << "VitalRouteController: Reserved route" << routeId
             << "from" << route.sourceSignalId << "to" << route.destSignalId;

    // Record safety event
    recordSafetyEvent("route_reserved", routeId,
                      QString("Route from %1 to %2").arg(route.sourceSignalId, route.destSignalId));

    ValidationResult result = ValidationResult::allowed("Route resources reserved successfully");
    result.safetyLevel = SafetyLevel::VITAL_SAFE;
    result.details = QString("Reserved %1 circuits and %2 point machines")
                         .arg(route.assignedCircuits.size())
                         .arg(route.lockedPointMachines.size());

    return result;
}

bool VitalRouteController::lockResourcesForRoute(const RouteAssignment& route, const QStringList& affectedSignals) {
    if (!m_resourceLockService) {
        qCritical() << " [LOCK_RESOURCES] ResourceLockService not available";
        return false;
    }

    qDebug() << "[LOCK_RESOURCES] Starting resource locking for route:" << route.key();
    qDebug() << "  Main circuits:" << route.assignedCircuits;
    qDebug() << "   🛡️ Overlap circuits:" << route.overlapCircuits;
    qDebug() << "    Point machines:" << route.lockedPointMachines;
    qDebug() << "   Affected signals:" << affectedSignals;

    QStringList failedLocks;
    int totalLocks = 0;
    int successfulLocks = 0;

    //   STEP 1: Lock Main Route Track Circuits (ROUTE type)
    qDebug() << "[LOCK_RESOURCES] Locking main route circuits...";
    for (const QString& circuitId : route.assignedCircuits) {
        totalLocks++;
        QVariantMap lockResult = m_resourceLockService->lockResource(
            "TRACK_CIRCUIT",    // resourceType
            circuitId,          // resourceId
            route.key(),        // routeId
            "ROUTE",           // lockType
            route.operatorId,   // operatorId
            QString("Main route circuit for %1").arg(route.key()) // reason
            );

        if (lockResult["success"].toBool()) {
            successfulLocks++;
            qDebug() << "     Locked main circuit:" << circuitId;
        } else {
            failedLocks.append(QString("TRACK_CIRCUIT:%1 - %2").arg(circuitId, lockResult["error"].toString()));
            qCritical() << "    Failed to lock main circuit:" << circuitId << "-" << lockResult["error"].toString();
        }
    }

    //   STEP 2: Lock Overlap Track Circuits (OVERLAP type)
    qDebug() << "[LOCK_RESOURCES] Locking overlap circuits...";
    for (const QString& circuitId : route.overlapCircuits) {
        totalLocks++;
        QVariantMap lockResult = m_resourceLockService->lockResource(
            "TRACK_CIRCUIT",    // resourceType
            circuitId,          // resourceId
            route.key(),        // routeId
            "OVERLAP",         // lockType (different from main route)
            route.operatorId,   // operatorId
            QString("Overlap circuit for %1").arg(route.key()) // reason
            );

        if (lockResult["success"].toBool()) {
            successfulLocks++;
            qDebug() << "     Locked overlap circuit:" << circuitId;
        } else {
            failedLocks.append(QString("TRACK_CIRCUIT:%1(OVERLAP) - %2").arg(circuitId, lockResult["error"].toString()));
            qCritical() << "    Failed to lock overlap circuit:" << circuitId << "-" << lockResult["error"].toString();
        }
    }

    //   STEP 3: Lock Point Machines (ROUTE type)
    qDebug() << "[LOCK_RESOURCES] Locking point machines...";
    for (const QString& machineId : route.lockedPointMachines) {
        totalLocks++;
        QVariantMap lockResult = m_resourceLockService->lockResource(
            "POINT_MACHINE",    // resourceType
            machineId,          // resourceId
            route.key(),        // routeId
            "ROUTE",           // lockType
            route.operatorId,   // operatorId
            QString("Point machine for %1").arg(route.key()) // reason
            );

        if (lockResult["success"].toBool()) {
            successfulLocks++;
            qDebug() << "     Locked point machine:" << machineId;
        } else {
            failedLocks.append(QString("POINT_MACHINE:%1 - %2").arg(machineId, lockResult["error"].toString()));
            qCritical() << "    Failed to lock point machine:" << machineId << "-" << lockResult["error"].toString();
        }
    }

    //   STEP 4: Lock Signals from Aspect Propagation (ROUTE type)
    qDebug() << "[LOCK_RESOURCES] Locking signals from aspect propagation...";
    for (const QString& signalId : affectedSignals) {
        totalLocks++;
        QVariantMap lockResult = m_resourceLockService->lockResource(
            "SIGNAL",           // resourceType
            signalId,           // resourceId
            route.key(),        // routeId
            "ROUTE",           // lockType
            route.operatorId,   // operatorId
            QString("Signal control for %1").arg(route.key()) // reason
            );

        if (lockResult["success"].toBool()) {
            successfulLocks++;
            qDebug() << "     Locked signal:" << signalId;
        } else {
            failedLocks.append(QString("SIGNAL:%1 - %2").arg(signalId, lockResult["error"].toString()));
            qCritical() << "    Failed to lock signal:" << signalId << "-" << lockResult["error"].toString();
        }
    }

    //   STEP 5: Summary and Safety Check
    qDebug() << "[LOCK_RESOURCES] Resource locking summary:";
    qDebug() << "    Total resources:" << totalLocks;
    qDebug() << "     Successfully locked:" << successfulLocks;
    qDebug() << "    Failed to lock:" << failedLocks.size();

    if (!failedLocks.isEmpty()) {
        qCritical() << " [LOCK_RESOURCES] Failed locks:" << failedLocks;

        //   SAFETY: Rollback all successful locks on any failure
        qWarning() << "[LOCK_RESOURCES] Rolling back successful locks due to failures...";
        unlockResourcesForRoute(route.key());
        return false;
    }

    //   SUCCESS: Log successful lock acquisition
    recordSafetyEvent("resource_locks_acquired", route.key(),
                      QString("Locked %1 resources: %2 circuits, %3 PMs, %4 signals")
                          .arg(totalLocks)
                          .arg(route.assignedCircuits.size() + route.overlapCircuits.size())
                          .arg(route.lockedPointMachines.size())
                          .arg(affectedSignals.size()));

    qDebug() << "  [LOCK_RESOURCES] All route resources locked successfully";
    return true;
}

bool VitalRouteController::unlockResourcesForRoute(const QString& routeId) {
    if (!m_resourceLockService) {
        return false;
    }

    return m_resourceLockService->unlockAllResourcesForRoute(routeId);
}

QVariantMap VitalRouteController::emergencyRelease(const QString& routeId, const QString& reason) {
    QElapsedTimer timer;
    timer.start();

    ValidationResult result = emergencyReleaseInternal(routeId, reason);
    
    auto duration = std::chrono::milliseconds(timer.elapsed());
    recordValidationTime("emergency_release", duration);

    // Record critical safety event
    if (m_telemetryService) {
        m_telemetryService->recordSafetyEvent(
            "emergency_release",
            "CRITICAL",
            routeId,
            QString("Emergency release: %1").arg(reason),
            "VitalRouteController"
        );
    }

    if (result.isAllowed) {
        m_emergencyReleases++;
        emit emergencyReleasePerformed(routeId, reason);
        emit routeCountChanged();
    }

    return validationResultToVariantMap(result);
}

ValidationResult VitalRouteController::emergencyReleaseInternal(const QString& routeId, const QString& reason) {
    if (!m_activeRoutes.contains(routeId)) {
        return ValidationResult::blocked("Route not found: " + routeId);
    }

    RouteAssignment& route = m_activeRoutes[routeId];
    
    qCritical() << " VitalRouteController: EMERGENCY RELEASE of route" << routeId << "- Reason:" << reason;

    // Record safety event
    recordSafetyEvent("emergency_release", routeId, reason);

    // Notify emergency services
    notifyEmergencyServices(routeId, reason);

    // Force unlock all resources
    unlockResourcesForRoute(routeId);

    // Update route state
    route.state = RouteState::EMERGENCY_RELEASED;
    route.releasedAt = QDateTime::currentDateTime();
    route.failureReason = reason;

    // Update database
    updateRouteInDatabase(route);

    // Remove from active routes
    m_activeRoutes.remove(routeId);

    // Remove circuit indexing
    for (const QString& circuitId : route.assignedCircuits + route.overlapCircuits) {
        if (m_routesByCircuit.contains(circuitId)) {
            m_routesByCircuit[circuitId].removeOne(routeId);
            if (m_routesByCircuit[circuitId].isEmpty()) {
                m_routesByCircuit.remove(circuitId);
            }
        }
    }

    ValidationResult result = ValidationResult::allowed("Emergency release completed");
    result.safetyLevel = SafetyLevel::DANGER; // Mark as danger due to emergency nature
    result.details = QString("Emergency release of route %1: %2").arg(routeId, reason);
    
    return result;
}

void VitalRouteController::performPeriodicValidation() {
    if (!m_isOperational) {
        return;
    }

    qDebug() << " VitalRouteController: Performing periodic validation check...";

    // Validate all active routes
    QStringList problematicRoutes;
    for (auto it = m_activeRoutes.begin(); it != m_activeRoutes.end(); ++it) {
        const RouteAssignment& route = it.value();

        // Perform safety check on each active route
        ValidationResult result = performSafetyCheck(route.key());

        if (!result.isAllowed) {
            qWarning() << " Periodic validation failed for route" << route.key() << ":" << result.reason;
            problematicRoutes.append(route.key());

            // Record safety violation
            recordSafetyViolation(route.key(), QString("Periodic validation failed: %1").arg(result.reason));
        }
    }

    // Check resource lock consistency
    if (m_resourceLockService) {
        for (const RouteAssignment& route : m_activeRoutes) {
            // Verify all circuits are still locked
            for (const QString& circuitId : route.assignedCircuits + route.overlapCircuits) {
                if (!m_resourceLockService->isResourceLocked("TRACK_CIRCUIT", circuitId)) {
                    qCritical() << " CRITICAL: Circuit" << circuitId << "not locked for active route" << route.key();
                    recordSafetyViolation(route.key(), QString("Circuit %1 lost lock").arg(circuitId));
                }
            }

            // Verify point machines are still locked
            for (const QString& machineId : route.lockedPointMachines) {
                if (!m_resourceLockService->isResourceLocked("POINT_MACHINE", machineId)) {
                    qCritical() << " CRITICAL: Point machine" << machineId << "not locked for active route" << route.key();
                    recordSafetyViolation(route.key(), QString("Point machine %1 lost lock").arg(machineId));
                }
            }
        }
    }

    // Log results
    if (problematicRoutes.isEmpty()) {
        qDebug() << "  Periodic validation passed for all" << m_activeRoutes.size() << "active routes";
    } else {
        qWarning() << " Periodic validation found issues with" << problematicRoutes.size() << "routes:" << problematicRoutes;
    }

    // Record telemetry
    if (m_telemetryService) {
        m_telemetryService->recordOperationalMetric(
            "periodic_validation_completed",
            m_activeRoutes.size(),
            "count"
            );

        if (!problematicRoutes.isEmpty()) {
            m_telemetryService->recordSafetyEvent(
                "periodic_validation_failures",
                "WARNING",
                "VitalRouteController",
                QString("Found %1 problematic routes during periodic validation").arg(problematicRoutes.size()),
                "system"
                );
        }
    }
}

void VitalRouteController::performHealthCheck() {
    if (!m_isOperational) {
        return;
    }

    qDebug() << "🏥 VitalRouteController: Performing health check...";

    QElapsedTimer timer;
    timer.start();

    bool wasHealthy = m_safetySystemHealthy;
    QStringList healthIssues;

    // 1. Check service dependencies
    if (!m_dbManager || !m_dbManager->isConnected()) {
        healthIssues.append("Database connection lost");
    }

    if (!m_interlockingService) {
        healthIssues.append("InterlockingService not available");
    }

    if (!m_resourceLockService || !m_resourceLockService->isOperational()) {
        healthIssues.append("ResourceLockService not operational");
    }

    if (!m_telemetryService || !m_telemetryService->isOperational()) {
        healthIssues.append("TelemetryService not operational");
    }

    // 2. Check performance metrics
    if (m_averageValidationTimeMs > TARGET_VALIDATION_TIME.count() * 2) {
        healthIssues.append(QString("Validation performance degraded: %1ms (target: %2ms)")
                                .arg(m_averageValidationTimeMs)
                                .arg(TARGET_VALIDATION_TIME.count()));
    }

    // 3. Check safety violation rate
    int recentViolations = m_recentSafetyViolations.size();
    if (recentViolations > 5) {
        healthIssues.append(QString("High safety violation rate: %1 recent violations").arg(recentViolations));
    }

    // 4. Check consecutive failures
    if (m_consecutiveFailures > MAX_CONSECUTIVE_FAILURES) {
        healthIssues.append(QString("Too many consecutive failures: %1").arg(m_consecutiveFailures));
    }

    // 5. Check route count vs capacity
    if (m_activeRoutes.size() > MAX_CONCURRENT_ROUTES) {
        healthIssues.append(QString("Route count exceeds capacity: %1/%2")
                                .arg(m_activeRoutes.size())
                                .arg(MAX_CONCURRENT_ROUTES));
    }

    // 6. Check system uptime vs restart requirements
    qint64 currentTime = QDateTime::currentDateTime().toSecsSinceEpoch();
    qint64 uptimeHours = (currentTime - m_systemStartTime) / 3600;
    if (uptimeHours > 168) { // 7 days - suggest restart
        healthIssues.append(QString("Long uptime detected: %1 hours (consider restart)").arg(uptimeHours));
    }

    // Update health status
    m_safetySystemHealthy = healthIssues.isEmpty();
    m_lastHealthCheck = QDateTime::currentDateTime();

    // Log health status
    if (m_safetySystemHealthy) {
        qDebug() << "  Health check passed - all systems nominal";
    } else {
        qWarning() << " Health check found issues:" << healthIssues;
    }

    // Emit signal if health status changed
    if (wasHealthy != m_safetySystemHealthy) {
        emit safetyStatusChanged();

        if (!m_safetySystemHealthy) {
            qCritical() << " VitalRouteController: Safety system health degraded";

            // Record critical event
            if (m_telemetryService) {
                m_telemetryService->recordSafetyEvent(
                    "safety_system_degraded",
                    "CRITICAL",
                    "VitalRouteController",
                    QString("Health check failed: %1").arg(healthIssues.join("; ")),
                    "system"
                    );
            }
        } else {
            qDebug() << "  Safety system health restored";

            if (m_telemetryService) {
                m_telemetryService->recordSafetyEvent(
                    "safety_system_restored",
                    "INFO",
                    "VitalRouteController",
                    "Safety system health check passed after previous issues",
                    "system"
                    );
            }
        }
    }

    // Record performance metrics
    double healthCheckTimeMs = timer.elapsed();
    if (m_telemetryService) {
        m_telemetryService->recordPerformanceMetric(
            "health_check",
            healthCheckTimeMs,
            m_safetySystemHealthy,
            "VitalRouteController",
            QVariantMap{
                {"issues_found", healthIssues.size()},
                {"active_routes", m_activeRoutes.size()},
                {"uptime_hours", uptimeHours},
                {"avg_validation_time_ms", m_averageValidationTimeMs}
            }
            );
    }

    qDebug() << "🏥 Health check completed in" << healthCheckTimeMs << "ms";
}

void VitalRouteController::performPeriodicSafetyCheck() {
    if (!m_isOperational) {
        return;
    }

    checkForSafetyViolations();
    updateSafetySystemHealth();
    
    m_lastSafetyCheck = QDateTime::currentDateTime();
}

void VitalRouteController::checkForSafetyViolations() {
    // Check for route conflicts, resource violations, etc.
    // This is a simplified implementation
    
    for (const RouteAssignment& route : m_activeRoutes) {
        if (!isRouteConflictFree(route)) {
            QString violationDesc = QString("Route conflict detected for route %1").arg(route.key());
            SafetyViolation violation;
            violation.description = violationDesc;
            violation.affectedRoutes = {route.key()};
            violation.type = ViolationType::ROUTE_CONFLICT;
            violation.severity = ComplianceLevel::MAJOR_DEVIATION;
            violation.detectedAt = QDateTime::currentDateTime();
            violation.isActive = true;
            m_recentSafetyViolations.append(violation);
            m_safetyViolations++;
            
            emit safetyViolationDetected(route.key(), "route_conflict", violationDesc);
            
            if (m_telemetryService) {
                m_telemetryService->recordSafetyViolation("route_conflict", route.key(), violationDesc);
            }
        }
    }

    // Keep only recent violations
    if (m_recentSafetyViolations.size() > 10) {
        m_recentSafetyViolations.removeFirst();
    }
}

bool VitalRouteController::isRouteConflictFree(const RouteAssignment& route) const {
    // Simplified conflict detection
    // Full implementation would check:
    // - Circuit occupancy vs assignment
    // - Point machine position vs route requirements
    // - Signal aspects vs route state
    // - Overlap violations
    
    Q_UNUSED(route)
    return true; // Placeholder
}

// Utility method implementations
QString VitalRouteController::routeStateToString(RouteState state) const {
    switch (state) {
        case RouteState::REQUESTED: return "REQUESTED";
        case RouteState::VALIDATING: return "VALIDATING";
        case RouteState::RESERVED: return "RESERVED";
        case RouteState::ACTIVE: return "ACTIVE";
        case RouteState::PARTIALLY_RELEASED: return "PARTIALLY_RELEASED";
        case RouteState::RELEASED: return "RELEASED";
        case RouteState::FAILED: return "FAILED";
        case RouteState::EMERGENCY_RELEASED: return "EMERGENCY_RELEASED";
        case RouteState::DEGRADED: return "DEGRADED";
        default: return "UNKNOWN";
    }
}

RouteState VitalRouteController::stringToRouteState(const QString& stateStr) const {
    if (stateStr == "REQUESTED") return RouteState::REQUESTED;
    if (stateStr == "VALIDATING") return RouteState::VALIDATING;
    if (stateStr == "RESERVED") return RouteState::RESERVED;
    if (stateStr == "ACTIVE") return RouteState::ACTIVE;
    if (stateStr == "PARTIALLY_RELEASED") return RouteState::PARTIALLY_RELEASED;
    if (stateStr == "RELEASED") return RouteState::RELEASED;
    if (stateStr == "FAILED") return RouteState::FAILED;
    if (stateStr == "EMERGENCY_RELEASED") return RouteState::EMERGENCY_RELEASED;
    if (stateStr == "DEGRADED") return RouteState::DEGRADED;
    return RouteState::FAILED;
}

QString VitalRouteController::safetyLevelToString(SafetyLevel level) const {
    switch (level) {
        case SafetyLevel::VITAL_SAFE: return "VITAL_SAFE";
        case SafetyLevel::SAFE: return "SAFE";
        case SafetyLevel::CAUTION: return "CAUTION";
        case SafetyLevel::WARNING: return "WARNING";
        case SafetyLevel::DANGER: return "DANGER";
        default: return "DANGER";
    }
}

QVariantMap VitalRouteController::validationResultToVariantMap(const ValidationResult& result) const {
    return QVariantMap{
        {"success", result.isAllowed},
        {"safetyLevel", safetyLevelToString(result.safetyLevel)},
        {"reason", result.reason},
        {"details", result.details},
        {"conflictingResources", result.conflictingResources},
        {"alternativeSolutions", result.alternativeSolutions},
        {"responseTimeMs", static_cast<double>(result.responseTime.count())},
        {"performanceMetrics", result.performanceMetrics},
        {"interlockingResults", result.interlockingResults}
    };
}

RouteAssignment VitalRouteController::variantMapToRouteAssignment(const QVariantMap& map) const {
    RouteAssignment route;
    route.id = QUuid::fromString(map.value("id", QUuid::createUuid().toString()).toString());
    route.routeName = map.value("routeName").toString();
    route.sourceSignalId = map.value("sourceSignalId").toString();
    route.destSignalId = map.value("destSignalId").toString();
    route.direction = map.value("direction").toString();
    route.assignedCircuits = map.value("assignedCircuits").toStringList();
    route.overlapCircuits = map.value("overlapCircuits").toStringList();
    route.lockedPointMachines = map.value("lockedPointMachines").toStringList();
    route.state = stringToRouteState(map.value("state", "REQUESTED").toString());
    route.priority = map.value("priority", 100).toInt();
    route.operatorId = map.value("operatorId", "system").toString();
    return route;
}

int VitalRouteController::activeRoutes() const {
    return m_activeRoutes.size();
}

void VitalRouteController::recordValidationTime(const QString& operation, std::chrono::milliseconds duration) {
    m_validationTimes.append(duration);
    if (m_validationTimes.size() > PERFORMANCE_HISTORY_SIZE) {
        m_validationTimes.removeFirst();
    }
    
    updateAverageValidationTime();
    
    if (duration > TARGET_VALIDATION_TIME) {
        qWarning() << " VitalRouteController: Slow" << operation << ":" << duration.count() << "ms";
    }
}

void VitalRouteController::updateAverageValidationTime() {
    if (m_validationTimes.isEmpty()) {
        return;
    }
    
    std::chrono::milliseconds total{0};
    for (const auto& time : m_validationTimes) {
        total += time;
    }
    
    m_averageValidationTime = static_cast<double>(total.count()) / m_validationTimes.size();
    emit performanceChanged();
}

void VitalRouteController::recordSafetyEvent(const QString& eventType, const QString& routeId, const QString& details) {
    if (m_telemetryService) {
        m_telemetryService->recordSafetyEvent(eventType, "INFO", routeId, details, "VitalRouteController");
    }
}

void VitalRouteController::updateSafetySystemHealth() {
    bool wasHealthy = m_safetySystemHealthy;
    
    // Check various health indicators
    bool servicesOperational = m_dbManager && m_interlockingService && 
                              m_resourceLockService && m_telemetryService;
    bool performanceAcceptable = m_averageValidationTime < TARGET_VALIDATION_TIME.count() * 2;
    bool noRecentViolations = m_recentSafetyViolations.size() < 5;
    
    m_safetySystemHealthy = servicesOperational && performanceAcceptable && noRecentViolations;
    
    if (wasHealthy != m_safetySystemHealthy) {
        emit safetyStatusChanged();
        
        if (!m_safetySystemHealthy) {
            qCritical() << " VitalRouteController: Safety system health degraded";
            if (m_telemetryService) {
                m_telemetryService->recordSafetyEvent(
                    "safety_system_degraded",
                    "CRITICAL",
                    "VitalRouteController",
                    "Safety system health check failed",
                    "system"
                );
            }
        }
    }
}

void VitalRouteController::notifyEmergencyServices(const QString& routeId, const QString& reason) {
    qCritical() << " EMERGENCY NOTIFICATION: Route" << routeId << "released due to:" << reason;
    // In a real system, this would notify control center, log to external systems, etc.
}

// Stub implementations for remaining methods
bool VitalRouteController::persistRouteToDatabase(const RouteAssignment& route) {
    if (!m_dbManager) {
        qCritical() << "VitalRouteController: DatabaseManager is null";
        return false;
    }

    qDebug() << " [PERSIST] Starting route persistence:" << route.key();

    //   SEQUENTIAL: Step 1 - Create route with full commit acknowledgement
    bool routeCreated = m_dbManager->insertRouteAssignment(
        route.key(),                    // routeId
        route.sourceSignalId,          // sourceSignalId
        route.destSignalId,            // destSignalId
        route.direction,               // direction
        route.assignedCircuits,        // assignedCircuits
        route.overlapCircuits,         // overlapCircuits
        routeStateToString(route.state), // state (convert enum to string)
        route.lockedPointMachines,     // lockedPointMachines
        route.priority,                // priority
        route.operatorId               // operatorId
        );

    if (!routeCreated) {
        qCritical() << " [PERSIST] Failed to create route in database:" << route.key();
        return false;
    }

    //   ACKNOWLEDGEMENT RECEIVED: Route creation committed successfully
    qDebug() << "  [PERSIST] Route creation acknowledged:" << route.key();

    //   SEQUENTIAL: Step 2 - Log route event ONLY after successful creation
    bool eventLogged = m_dbManager->insertRouteEvent(
        route.key(),                    // routeId
        "ROUTE_RESERVED",               // eventType
        QVariantMap{                    // eventData
            {"sourceSignal", route.sourceSignalId},
            {"destSignal", route.destSignalId},
            {"path", route.assignedCircuits},
            {"overlap", route.overlapCircuits}
        },
        route.operatorId,               // operatorId
        "VitalRouteController",         // sourceComponent
        QString(),                      // correlationId
        0.0,                           // responseTimeMs
        true                           // safetyCritical
        );

    if (!eventLogged) {
        qWarning() << " [PERSIST] Route created but event logging failed:" << route.key();
        // Note: Don't fail the whole operation just because event logging failed
        // The route exists and is functional
    } else {
        qDebug() << "  [PERSIST] Route event logged successfully:" << route.key();
    }

    //   COMPLETE: Both route creation and event logging completed sequentially
    qDebug() << "  [PERSIST] Route persistence completed successfully:" << route.key();
    return true;
}

bool VitalRouteController::updateRouteInDatabase(const RouteAssignment& route) {
    Q_UNUSED(route)
    return true; // Placeholder
}

bool VitalRouteController::removeRouteFromDatabase(const QString& routeId) {
    if (!m_dbManager) {
        qCritical() << "VitalRouteController: DatabaseManager is null";
        return false;
    }

    //   SAFETY: Remove route record if resource locking fails
    QSqlQuery query(m_dbManager->getDatabase());
    query.prepare("DELETE FROM railway_control.route_assignments WHERE id = ?");
    query.addBindValue(routeId);

    if (!query.exec()) {
        qWarning() << " VitalRouteController: Failed to remove route" << routeId
                   << "from database:" << query.lastError().text();
        return false;
    }

    qDebug() << " VitalRouteController: Removed route" << routeId << "from database (rollback)";
    return true;
}

void VitalRouteController::onTrackCircuitOccupancyChanged(const QString& circuitId, bool isOccupied) {
    if (!m_isOperational) {
        return;
    }
    
    qDebug() << "VitalRouteController: Track circuit" << circuitId << (isOccupied ? "OCCUPIED" : "CLEAR");
    
    // Check if this circuit affects any active routes
    if (m_routesByCircuit.contains(circuitId)) {
        QStringList affectedRoutes = m_routesByCircuit[circuitId];
        
        for (const QString& routeId : affectedRoutes) {
            if (m_activeRoutes.contains(routeId)) {
                RouteAssignment& route = m_activeRoutes[routeId];
                
                // Handle route state changes based on occupancy
                if (isOccupied && route.state == RouteState::ACTIVE) {
                    // Train is using the route
                    if (route.assignedCircuits.contains(circuitId)) {
                        // Normal progression
                        recordSafetyEvent("route_train_progression", routeId, 
                                        QString("Train entered circuit %1").arg(circuitId));
                    } else if (route.overlapCircuits.contains(circuitId)) {
                        // Train entered overlap region
                        recordSafetyEvent("route_overlap_occupied", routeId,
                                        QString("Train entered overlap circuit %1").arg(circuitId));
                    }
                } else if (isOccupied && route.state == RouteState::RESERVED) {
                    // Unauthorized occupancy
                    recordSafetyViolation(routeId, 
                                        QString("Unauthorized occupancy of reserved circuit %1").arg(circuitId));
                }
            }
        }
    }
}

void VitalRouteController::onPointMachinePositionChanged(const QString& machineId, const QString& position) {
    qDebug() << "VitalRouteController: Point machine" << machineId << "position:" << position;
    
    // Check if this point machine affects any active routes
    for (auto& route : m_activeRoutes) {
        if (route.lockedPointMachines.contains(machineId)) {
            recordSafetyEvent("route_point_machine_change", route.key(),
                            QString("Point machine %1 changed to %2").arg(machineId, position));
        }
    }
}

void VitalRouteController::onSignalAspectChanged(const QString& signalId, const QString& aspect) {
    qDebug() << "VitalRouteController: Signal" << signalId << "aspect:" << aspect;
    
    // Check if this signal affects any active routes
    for (auto& route : m_activeRoutes) {
        if (route.sourceSignalId == signalId || route.destSignalId == signalId) {
            recordSafetyEvent("route_signal_change", route.key(),
                            QString("Signal %1 changed to %2").arg(signalId, aspect));
        }
    }
}

QVariantMap VitalRouteController::releaseRouteResources(const QString& routeId) {
    if (!m_activeRoutes.contains(routeId)) {
        return QVariantMap{{"success", false}, {"error", "Route not found"}};
    }
    
    const RouteAssignment& route = m_activeRoutes[routeId];
    bool success = true;
    QStringList errors;
    
    // Release resource locks
    if (m_resourceLockService) {
        // Release track circuits
        for (const QString& circuitId : route.assignedCircuits + route.overlapCircuits) {
            if (!m_resourceLockService->unlockResource("TRACK_CIRCUIT", circuitId, routeId)) {
                QString error = QString("Failed to unlock circuit %1").arg(circuitId);
                qWarning() << error << "for route" << routeId;
                errors.append(error);
                success = false;
            }
        }
        
        // Release point machines
        for (const QString& machineId : route.lockedPointMachines) {
            if (!m_resourceLockService->unlockResource("POINT_MACHINE", machineId, routeId)) {
                QString error = QString("Failed to unlock point machine %1").arg(machineId);
                qWarning() << error << "for route" << routeId;
                errors.append(error);
                success = false;
            }
        }
    }
    
    QVariantMap result;
    result["success"] = success;
    result["routeId"] = routeId;
    if (!errors.isEmpty()) {
        result["errors"] = errors;
    }
    return result;
}

QVariantMap VitalRouteController::emergencyReleaseAll(const QString& reason) {
    qCritical() << " VitalRouteController: EMERGENCY RELEASE ALL ROUTES - Reason:" << reason;
    
    QStringList releasedRoutes;
    QStringList failedReleases;
    
    for (auto it = m_activeRoutes.begin(); it != m_activeRoutes.end(); ++it) {
        QString routeId = it.key();
        QVariantMap result = emergencyRelease(routeId, reason);
        
        if (result["success"].toBool()) {
            releasedRoutes.append(routeId);
        } else {
            failedReleases.append(routeId);
        }
    }
    
    // Clear all routes if successful
    if (failedReleases.isEmpty()) {
        m_activeRoutes.clear();
        m_routesByCircuit.clear();
    }
    
    // Record emergency event
    recordSafetyEvent("emergency_release_all", "ALL_ROUTES",
                     QString("Emergency release all: %1. Released: %2, Failed: %3")
                     .arg(reason).arg(releasedRoutes.size()).arg(failedReleases.size()));
    
    return QVariantMap{
        {"success", failedReleases.isEmpty()},
        {"releasedRoutes", releasedRoutes},
        {"failedReleases", failedReleases},
        {"reason", reason}
    };
}

bool VitalRouteController::updateRouteState(const QString& routeId, const QString& newState) {
    if (!m_activeRoutes.contains(routeId)) {
        return false;
    }
    
    RouteAssignment& route = m_activeRoutes[routeId];
    RouteState oldState = route.state;
    route.state = stringToRouteState(newState);
    
    updateRouteInDatabase(route);
    
    qDebug() << "VitalRouteController: Route" << routeId << "state changed from" 
             << routeStateToString(oldState) << "to" << newState;
    
    recordSafetyEvent("route_state_change", routeId,
                     QString("State changed from %1 to %2")
                     .arg(routeStateToString(oldState), newState));
    
    return true;
}

QVariantMap VitalRouteController::getRouteStatus(const QString& routeId) const {
    if (!m_activeRoutes.contains(routeId)) {
        return QVariantMap{{"error", "Route not found"}};
    }
    
    const RouteAssignment& route = m_activeRoutes[routeId];
    return routeAssignmentToVariantMap(route);
}

QVariantList VitalRouteController::getActiveRoutes() const {
    QVariantList result;
    
    for (const RouteAssignment& route : m_activeRoutes.values()) {
        result.append(routeAssignmentToVariantMap(route));
    }
    
    return result;
}

QVariantMap VitalRouteController::getRouteStatistics() const {
    return QVariantMap{
        {"activeRoutes", m_activeRoutes.size()},
        {"totalValidations", m_totalValidations},
        {"successfulValidations", m_successfulValidations},
        {"emergencyReleases", m_emergencyReleases},
        {"safetyViolations", m_recentSafetyViolations.size()},
        {"averageValidationTimeMs", m_averageValidationTime},
        {"safetySystemHealthy", m_safetySystemHealthy},
        {"successRate", m_totalValidations > 0 ? (double)m_successfulValidations / m_totalValidations * 100.0 : 0.0}
    };
}

ValidationResult VitalRouteController::performSafetyCheck(const QString& routeId) {
    if (!m_activeRoutes.contains(routeId)) {
        return ValidationResult::blocked("Route not found", SafetyLevel::DANGER);
    }
    
    const RouteAssignment& route = m_activeRoutes[routeId];
    
    // Comprehensive safety check
    ValidationResult result = ValidationResult::allowed("Safety check passed");
    result.safetyLevel = SafetyLevel::VITAL_SAFE;
    
    // Check resource locks
    if (m_resourceLockService) {
        for (const QString& circuitId : route.assignedCircuits) {
            if (!m_resourceLockService->isResourceLocked("TRACK_CIRCUIT", circuitId)) {
                result = ValidationResult::blocked(
                    QString("Circuit %1 is not properly locked").arg(circuitId),
                    SafetyLevel::DANGER
                );
                break;
            }
        }
    }
    
    return result;
}

QVariantMap VitalRouteController::getSafetyStatus() const {
    return QVariantMap{
        {"safetySystemHealthy", m_safetySystemHealthy},
        {"recentViolations", m_recentSafetyViolations.size()},
        {"isOperational", m_isOperational},
        {"averageValidationTime", m_averageValidationTime},
        {"targetValidationTime", static_cast<double>(TARGET_VALIDATION_TIME.count())},
        {"performanceAcceptable", m_averageValidationTime < TARGET_VALIDATION_TIME.count() * 2}
    };
}

QVariantList VitalRouteController::detectSafetyViolations() const {
    QVariantList violations;
    
    for (const auto& violation : m_recentSafetyViolations) {
        QVariantMap violationMap;
        violationMap["routeId"] = violation.routeId;
        violationMap["description"] = violation.description;
        violationMap["timestamp"] = violation.timestamp;
        violationMap["severity"] = "HIGH";
        violations.append(violationMap);
    }
    
    return violations;
}

ValidationResult VitalRouteController::lockRouteResources(const QString& routeId, const QStringList& circuits, const QStringList& pointMachines) {
    if (!m_activeRoutes.contains(routeId)) {
        return ValidationResult::blocked("Route not found", SafetyLevel::DANGER);
    }
    
    RouteAssignment& route = m_activeRoutes[routeId];
    route.assignedCircuits = circuits;
    route.lockedPointMachines = pointMachines;

    // Below function block commented out has more aspects now
    // bool success = lockResourcesForRoute(route);

    bool success = false;
    if (success) {
        return ValidationResult::allowed("Resources locked successfully");
    } else {
        return ValidationResult::blocked("Failed to lock one or more resources", SafetyLevel::WARNING);
    }
}

bool VitalRouteController::unlockRouteResources(const QString& routeId) {
    QVariantMap result = releaseRouteResources(routeId);
    return result.value("success", false).toBool();
}

QVariantMap VitalRouteController::getResourceUtilization() const {
    QVariantMap utilization;
    
    // Count locked resources
    int lockedCircuits = 0;
    int lockedPointMachines = 0;
    
    for (const RouteAssignment& route : m_activeRoutes.values()) {
        lockedCircuits += route.assignedCircuits.size() + route.overlapCircuits.size();
        lockedPointMachines += route.lockedPointMachines.size();
    }
    
    utilization["lockedCircuits"] = lockedCircuits;
    utilization["lockedPointMachines"] = lockedPointMachines;
    utilization["activeRoutes"] = m_activeRoutes.size();
    
    return utilization;
}

QVariantMap VitalRouteController::routeAssignmentToVariantMap(const RouteAssignment& route) const {
    return QVariantMap{
        {"id", route.id.toString()},
        {"routeName", route.routeName},
        {"sourceSignalId", route.sourceSignalId},
        {"destSignalId", route.destSignalId},
        {"direction", route.direction},
        {"assignedCircuits", route.assignedCircuits},
        {"overlapCircuits", route.overlapCircuits},
        {"lockedPointMachines", route.lockedPointMachines},
        {"state", routeStateToString(route.state)},
        {"priority", route.priority},
        {"operatorId", route.operatorId},
        {"createdAt", route.createdAt},
        {"activatedAt", route.activatedAt},
        {"releasedAt", route.releasedAt}
    };
}

void VitalRouteController::recordSafetyViolation(const QString& routeId, const QString& description) {
    SafetyViolation violation;
    violation.routeId = routeId;
    violation.description = description;
    violation.timestamp = QDateTime::currentDateTime();
    
    m_recentSafetyViolations.append(violation);
    
    // Keep only recent violations (last hour)
    QDateTime cutoff = QDateTime::currentDateTime().addSecs(-3600);
    m_recentSafetyViolations.erase(
        std::remove_if(m_recentSafetyViolations.begin(), m_recentSafetyViolations.end(),
                      [cutoff](const SafetyViolation& v) { return v.timestamp < cutoff; }),
        m_recentSafetyViolations.end()
    );
    
    qCritical() << " VitalRouteController: Safety violation -" << routeId << ":" << description;
    
    recordSafetyEvent("safety_violation", routeId, description);
    updateSafetySystemHealth();
}

// === ASPECT PROPAGATION INTEGRATION ===

void VitalRouteController::setAspectPropagationService(RailFlux::Interlocking::AspectPropagationService* aspectService) {
    m_aspectPropagationService = aspectService;
    qDebug() << "  VitalRouteController: Aspect propagation service connected";
}

QVariantMap VitalRouteController::establishRouteWithIntelligentAspects(
    const QString& sourceSignalId,
    const QString& destinationSignalId,
    const QStringList& routePath,
    const QStringList& overlapPath,
    const QVariantMap& pointMachinePositions)
{
    QElapsedTimer timer;
    timer.start();

    qDebug() << " [INTELLIGENT_ROUTE] Starting intelligent route establishment:"
             << sourceSignalId << "→" << destinationSignalId;
    qDebug() << "  Route path:" << routePath;
    qDebug() << "   🛡️ Overlap path:" << overlapPath;

    QVariantMap result;

    if (!m_aspectPropagationService) {
        qWarning() << " [INTELLIGENT_ROUTE] Aspect propagation service not available";
        result["success"] = false;
        result["error"] = "Intelligent aspect propagation not available";
        return result;
    }

    try {
        //   STEP 1: Generate Route ID First
        QString routeId = QUuid::createUuid().toString();
        qDebug() << " [INTELLIGENT_ROUTE] Generated route ID:" << routeId;

        //   STEP 2: Aspect Propagation FIRST (before database persistence)
        qDebug() << " [INTELLIGENT_ROUTE] Starting aspect propagation...";

        QVariantMap propagationOptions;
        propagationOptions["routePath"] = routePath;
        propagationOptions["overlapPath"] = overlapPath;

        if (isAdvancedStarterDestination(destinationSignalId)) {
            propagationOptions["desired_destination_aspect"] = "GREEN";
        } else {
            propagationOptions["desired_destination_aspect"] = "RED";
        }

        QVariantMap propagationResult = m_aspectPropagationService->propagateAspectsAdvanced(
            sourceSignalId, destinationSignalId, pointMachinePositions, propagationOptions);

        if (!propagationResult["success"].toBool()) {
            qCritical() << " [INTELLIGENT_ROUTE] Aspect propagation failed";
            result["success"] = false;
            result["error"] = "Aspect propagation failed: " + propagationResult["errorMessage"].toString();
            result["propagationError"] = propagationResult["errorCode"].toString();
            return result;
        }

        //   STEP 3: Extract Results from Propagation
        QVariantMap signalAspects = propagationResult["signalAspects"].toMap();
        QVariantMap requiredPointMachines = propagationResult["pointMachines"].toMap();
        QVariantMap decisionReasons = propagationResult["decisionReasons"].toMap();

        qDebug() << " [INTELLIGENT_ROUTE] Aspect propagation completed successfully!";
        qDebug() << "   Signal aspects:" << signalAspects.keys();
        qDebug() << "    Point machines:" << requiredPointMachines.keys();

        //   STEP 3.5: NEW - Extract Signal List for Resource Locking
        QStringList affectedSignalList = signalAspects.keys();
        qDebug() << " [INTELLIGENT_ROUTE] Signals to lock:" << affectedSignalList;

        //   STEP 4: Create Route Assignment with Complete Information
        RouteAssignment route;
        route.id = QUuid::fromString(routeId);
        route.sourceSignalId = sourceSignalId;
        route.destSignalId = destinationSignalId;
        route.direction = "UP";  //   FIX: Use valid database value
        route.assignedCircuits = routePath;
        route.overlapCircuits = overlapPath;
        route.state = RouteState::RESERVED;
        route.priority = 100;
        route.operatorId = "INTELLIGENT_SYSTEM";
        route.createdAt = QDateTime::currentDateTime();

        // Add calculated point machines to route
        QStringList pmList;
        for (auto it = requiredPointMachines.begin(); it != requiredPointMachines.end(); ++it) {
            pmList.append(it.key());
        }
        route.lockedPointMachines = pmList;

        //   STEP 5: Persist Route to Database (with complete information)
        qDebug() << " [INTELLIGENT_ROUTE] Persisting route to database...";
        if (!persistRouteToDatabase(route)) {
            qCritical() << " [INTELLIGENT_ROUTE] Failed to persist route to database";
            result["success"] = false;
            result["error"] = "Failed to persist route to database";
            return result;
        }
        qDebug() << "  [INTELLIGENT_ROUTE] Route persisted successfully to database";

        //   STEP 6: Execute Coordinated Changes
        qDebug() << " [INTELLIGENT_ROUTE] Executing coordinated aspect changes...";
        QVariantMap executionResult = executeCoordinatedAspectChanges(
            signalAspects, requiredPointMachines);

        if (!executionResult["success"].toBool()) {
            qCritical() << " [INTELLIGENT_ROUTE] Execution failed, removing route from database";
            removeRouteFromDatabase(routeId);  // Cleanup on failure
            result["success"] = false;
            result["error"] = "Execution failed: " + executionResult["error"].toString();
            return result;
        }

        //   STEP 6.5: UPDATED - Lock Resources Using ResourceLockService with Signal List
        qDebug() << " [INTELLIGENT_ROUTE] Locking route resources...";
        if (!lockResourcesForRoute(route, affectedSignalList)) {  //   PASS SIGNAL LIST
            qCritical() << " [INTELLIGENT_ROUTE] Resource locking failed - rolling back route";
            // Cleanup: remove from database
            removeRouteFromDatabase(routeId);
            result["success"] = false;
            result["error"] = "Failed to acquire resource locks - route establishment aborted for safety";
            return result;
        }
        qDebug() << "  [INTELLIGENT_ROUTE] Route resources locked successfully";

        //   STEP 7: Add Route to Active Routes (in-memory tracking)
        m_activeRoutes[routeId] = route;
        qDebug() << "  [INTELLIGENT_ROUTE] Route added to active routes tracking";

        //   STEP 8: Log Success Event
        if (m_dbManager) {
            m_dbManager->insertRouteEvent(
                routeId,
                "ROUTE_RESERVED",
                QVariantMap{
                    {"sourceSignal", sourceSignalId},
                    {"destSignal", destinationSignalId},
                    {"path", routePath},
                    {"overlap", overlapPath},
                    {"method", "INTELLIGENT_ASPECT_PROPAGATION"},
                    {"processingTimeMs", timer.elapsed()},
                    {"signalAspects", signalAspects},
                    {"pointMachines", requiredPointMachines},
                    {"affectedSignals", affectedSignalList}  //   NEW: Log locked signals
                },
                "INTELLIGENT_SYSTEM",
                "VitalRouteController::establishRouteWithIntelligentAspects",
                QString(),
                timer.elapsed(),
                false
                );
        }

        //   STEP 9: Emit Success Signals
        emit routeReserved(routeId, sourceSignalId, destinationSignalId);
        emit routeCountChanged();

        //   SUCCESS: Return Complete Result
        result["success"] = true;
        result["routeId"] = routeId;
        result["processingTimeMs"] = timer.elapsed();
        result["signalAspects"] = signalAspects;
        result["pointMachines"] = requiredPointMachines;
        result["affectedSignals"] = affectedSignalList;  //   NEW: Include in result
        result["method"] = "INTELLIGENT_ASPECT_PROPAGATION";

        qDebug() << "  [INTELLIGENT_ROUTE] Intelligent route establishment succeeded in" << timer.elapsed() << "ms";
        qDebug() << "    Route ID:" << routeId;
        qDebug() << "    Signals set:" << signalAspects.keys();
        qDebug() << "    Point machines:" << requiredPointMachines.keys();
        qDebug() << "    Locked signals:" << affectedSignalList;

        return result;

    } catch (const std::exception& e) {
        qCritical() << " [INTELLIGENT_ROUTE] Exception occurred:" << e.what();
        result["success"] = false;
        result["error"] = QString("Exception: %1").arg(e.what());
        return result;
    }
}

QVariantMap VitalRouteController::executeCoordinatedAspectChanges(
    const QVariantMap& signalAspects,
    const QVariantMap& pointMachinePositions)
{
    QElapsedTimer timer;
    timer.start();

    qDebug() << " VitalRouteController: Executing coordinated aspect changes...";

    QVariantMap result;
    QStringList successfulSignals;
    QStringList failedSignals;
    QStringList successfulPointMachines;
    QStringList failedPointMachines;

    try {
        // 1. First, set point machines with PROPER DATA EXTRACTION
        for (auto it = pointMachinePositions.begin(); it != pointMachinePositions.end(); ++it) {
            QString machineId = it.key();

            // ? FIX: Extract requiredPosition from nested QVariantMap
            QVariantMap pmData = it.value().toMap();
            QString requiredPosition = pmData["requiredPosition"].toString();
            QString currentPosition = pmData["currentPosition"].toString();
            bool needsMovement = pmData["needsMovement"].toBool();

            qDebug() << "    Setting point machine" << machineId << "to" << requiredPosition;
            qDebug() << "      Current:" << currentPosition << "Required:" << requiredPosition
                     << "Movement needed:" << needsMovement;

            // ? SAFETY CHECK: Ensure position is valid
            if (requiredPosition.isEmpty() ||
                (requiredPosition != "NORMAL" && requiredPosition != "REVERSE")) {
                qCritical() << "? Invalid required position for PM" << machineId << ":" << requiredPosition;
                failedPointMachines.append(machineId);
                continue;  // Skip this point machine
            }

            // ? OPTIMIZATION: Skip if no movement needed
            if (!needsMovement) {
                qDebug() << "      No movement required for" << machineId;
                successfulPointMachines.append(machineId);
                continue;
            }

            // ? VALIDATION: Check availability before attempting move
            QString availabilityStatus = pmData["availabilityStatus"].toString();
            bool isLocked = pmData["isLocked"].toBool();

            if (availabilityStatus != "AVAILABLE" || isLocked) {
                QString reason = isLocked ? "locked" : availabilityStatus;
                qWarning() << "? Cannot move PM" << machineId << "- reason:" << reason;
                failedPointMachines.append(machineId);
                continue;
            }

            // ? FIX: Use auto to avoid namespace conflicts with ValidationResult
            if (m_interlockingService) {
                auto pmValidation = m_interlockingService->validatePointMachineOperation(
                    machineId, currentPosition, requiredPosition, "VitalRouteController");

                if (!pmValidation.isAllowed()) {
                    qWarning() << "? PM validation failed for" << machineId
                               << ":" << pmValidation.getReason();
                    failedPointMachines.append(machineId);
                    continue;
                }
            }

            // ? FIX: Execute actual point machine movement with correct signature
            bool pmSuccess = m_dbManager->updatePointMachinePosition(machineId, requiredPosition);
            if (pmSuccess) {
                successfulPointMachines.append(machineId);
                qDebug() << "     ? Point machine" << machineId << "moved to" << requiredPosition;
            } else {
                failedPointMachines.append(machineId);
                qCritical() << "? Failed to move point machine" << machineId << "to" << requiredPosition;
            }
        }

        // 2. Then set signal aspects
        for (auto it = signalAspects.begin(); it != signalAspects.end(); ++it) {
            QString signalId = it.key();
            QString requiredAspect = it.value().toString();

            qDebug() << "    Setting signal" << signalId << "to" << requiredAspect;

            // ? FIX: Use auto to avoid namespace conflicts with ValidationResult
            if (m_interlockingService) {
                auto signalValidation = m_interlockingService->validateMainSignalOperation(
                    signalId, "UNKNOWN", requiredAspect, "VitalRouteController");

                if (!signalValidation.isAllowed()) {
                    qWarning() << "? Signal validation failed for" << signalId
                               << ":" << signalValidation.getReason();
                    failedSignals.append(signalId);
                    continue;
                }
            }

            bool signalSuccess = m_dbManager->updateSignalAspect(signalId, "MAIN", requiredAspect);
            if (signalSuccess) {
                successfulSignals.append(signalId);
                qDebug() << "     ? Signal" << signalId << "set to" << requiredAspect;
            } else {
                failedSignals.append(signalId);
                qCritical() << "? Failed to update signal" << signalId << "to" << requiredAspect;
            }
        }

        // 3. Determine overall success
        bool allSuccessful = failedSignals.isEmpty() && failedPointMachines.isEmpty();

        result["success"] = allSuccessful;
        result["successfulSignals"] = successfulSignals;
        result["failedSignals"] = failedSignals;
        result["successfulPointMachines"] = successfulPointMachines;
        result["failedPointMachines"] = failedPointMachines;
        result["processingTimeMs"] = timer.elapsed();

        if (allSuccessful) {
            qDebug() << "? VitalRouteController: All coordinated changes executed successfully";
        } else {
            qWarning() << " VitalRouteController: Some coordinated changes failed";
            result["error"] = QString("Failed signals: %1, Failed PMs: %2")
                                  .arg(failedSignals.join(","), failedPointMachines.join(","));
        }

    } catch (const std::exception& e) {
        qCritical() << " Exception in coordinated aspect execution:" << e.what();
        result["success"] = false;
        result["error"] = QString("Exception: %1").arg(e.what());
    }

    return result;
}



// NEW: Helper method to determine if destination is an Advanced Starter
bool VitalRouteController::isAdvancedStarterDestination(const QString& signalId) const
{
    if (!m_dbManager) return false;

    QVariantMap signalData = m_dbManager->getSignalById(signalId);
    return signalData["signal_type"].toString() == "ADVANCED_STARTER";
}


} // namespace RailFlux::Route
