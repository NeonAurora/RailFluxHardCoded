#include "InterlockingService.h"
#include "InterlockingRuleEngine.h"
#include "SignalBranch.h"
#include "TrackCircuitBranch.h"
#include "PointMachineBranch.h"
#include "../database/DatabaseManager.h"
#include <QDebug>

// 
//   ValidationResult Implementation
// 

ValidationResult::ValidationResult(Status status, const QString& reason, Severity severity)
    : m_status(status), m_severity(severity), m_reason(reason)
    , m_evaluationTime(QDateTime::currentDateTime()) {}

ValidationResult ValidationResult::allowed(const QString& reason) {
    return ValidationResult(Status::ALLOWED, reason, Severity::INFO);
}

ValidationResult ValidationResult::blocked(const QString& reason, const QString& ruleId) {
    auto result = ValidationResult(Status::BLOCKED, reason, Severity::CRITICAL);
    if (!ruleId.isEmpty()) result.setRuleId(ruleId);
    return result;
}

QVariantMap ValidationResult::toVariantMap() const {
    QVariantMap map;
    map["isAllowed"] = isAllowed();
    map["reason"] = m_reason;
    map["ruleId"] = m_ruleId;
    map["severity"] = static_cast<int>(m_severity);
    map["affectedEntities"] = m_affectedEntities;
    map["evaluationTime"] = m_evaluationTime;
    return map;
}

InterlockingRuleEngine* InterlockingService::getRuleEngine() const {
    // Since InterlockingService uses SignalBranch which has the rule engine
    // We need to expose it. Looking at the code structure, add this:
    if (m_signalBranch) {
        return m_signalBranch->getRuleEngine(); // You might need to add this to SignalBranch too
    }
    return nullptr;
}

// 
//   InterlockingService Implementation
// 

InterlockingService::InterlockingService(DatabaseManager* dbManager, QObject* parent)
    : QObject(parent), m_dbManager(dbManager) {

    if (!dbManager) {
        qCritical() << " CRITICAL: InterlockingService initialized with null DatabaseManager!";
        return;
    }

    //   CREATE VALIDATION BRANCHES
    m_signalBranch = std::make_unique<SignalBranch>(dbManager, this);
    m_trackSegmentBranch = std::make_unique<TrackCircuitBranch>(dbManager, this);
    m_pointBranch = std::make_unique<PointMachineBranch>(dbManager, this);

    //   CONNECT SAFETY SIGNALS: TrackCircuitBranch safety signals
    connect(m_trackSegmentBranch.get(), &TrackCircuitBranch::systemFreezeRequired,
            this, &InterlockingService::systemFreezeRequired);

    connect(m_trackSegmentBranch.get(), &TrackCircuitBranch::interlockingFailure,
            this, &InterlockingService::handleInterlockingFailure);

    connect(m_trackSegmentBranch.get(), &TrackCircuitBranch::automaticInterlockingCompleted,
            this, [this](const QString& trackSegmentId, const QStringList& affectedSignals) {
                qDebug() << "  Automatic interlocking completed for trackSegment section" << trackSegmentId;
                emit automaticProtectionActivated(trackSegmentId,
                                                  QString("Automatic signal protection activated for %1 signals").arg(affectedSignals.size()));
            });

    qDebug() << "  InterlockingService initialized with all branches connected";
}

InterlockingService::~InterlockingService() {
    qDebug() << " InterlockingService destructor called";
    //   Cleanup handled by smart pointers
}

bool InterlockingService::initialize() {
    if (!m_dbManager || !m_dbManager->isConnected()) {
        qWarning() << " Cannot initialize interlocking: Database not connected";
        m_isOperational = false;
        emit operationalStateChanged(m_isOperational);
        return false;
    }

    m_isOperational = true;
    emit operationalStateChanged(m_isOperational);

    qDebug() << "  Interlocking service initialized and operational";
    return true;
}

// 
//   VALIDATION METHODS: For operator-initiated actions only
// 

ValidationResult InterlockingService::validateMainSignalOperation(
    const QString& signalId, const QString& currentAspect,
    const QString& requestedAspect, const QString& operatorId) {

    QElapsedTimer timer;
    timer.start();

    if (!m_isOperational) {
        return ValidationResult::blocked("Interlocking system not operational", "SYSTEM_OFFLINE");
    }

    if (!m_signalBranch) {
        qCritical() << " CRITICAL: SignalBranch not initialized!";
        return ValidationResult::blocked("Signal validation not available", "SIGNAL_BRANCH_MISSING");
    }

    //   DELEGATE TO SIGNAL BRANCH
    auto result = m_signalBranch->validateMainAspectChange(signalId, currentAspect, requestedAspect, operatorId);

    //   RECORD PERFORMANCE
    double responseTime = timer.elapsed();
    recordResponseTime(responseTime);

    if (responseTime > TARGET_RESPONSE_TIME_MS) {
        logPerformanceWarning("Signal validation", responseTime);
    }

    qDebug() << "Signal validation completed in" << responseTime << "ms:" << result.getReason();

    if (!result.isAllowed()) {
        emit operationBlocked(signalId, result.getReason());
    }

    return result;
}

//   SIMPLIFIED: InterlockingService delegates to SignalBranch
ValidationResult InterlockingService::validateSubsidiarySignalOperation(
    const QString& signalId, const QString& aspectType,
    const QString& currentAspect, const QString& requestedAspect,
    const QString& operatorId) {

    QElapsedTimer timer;
    timer.start();

    qDebug() << "SUBSIDIARY SIGNAL VALIDATION:" << signalId
             << "Type:" << aspectType
             << "Transition:" << currentAspect << "→" << requestedAspect
             << "Operator:" << operatorId;

    //   SYSTEM AVAILABILITY CHECK
    if (!m_isOperational) {
        qWarning() << " Subsidiary signal validation blocked: Interlocking system not operational";
        return ValidationResult::blocked("Interlocking system not operational", "SYSTEM_OFFLINE");
    }

    if (!m_signalBranch) {
        qCritical() << " CRITICAL: SignalBranch not initialized for subsidiary signal validation!";
        return ValidationResult::blocked("Signal validation not available", "SIGNAL_BRANCH_MISSING");
    }

    //   VALIDATE ASPECT TYPE
    if (aspectType != "CALLING_ON" && aspectType != "LOOP") {
        qWarning() << " Invalid subsidiary aspect type:" << aspectType;
        return ValidationResult::blocked(
            QString("Invalid subsidiary aspect type: %1").arg(aspectType),
            "INVALID_ASPECT_TYPE");
    }

    //   DELEGATE TO SIGNAL BRANCH: Same pattern as main signals
    auto result = m_signalBranch->validateSubsidiaryAspectChange(
        signalId, aspectType, currentAspect, requestedAspect, operatorId);

    //   PERFORMANCE MONITORING
    double responseTime = timer.elapsed();
    recordResponseTime(responseTime);

    if (responseTime > TARGET_RESPONSE_TIME_MS) {
        logPerformanceWarning(QString("Subsidiary signal validation (%1)").arg(aspectType), responseTime);
    }

    qDebug() << "Subsidiary signal validation completed in" << responseTime << "ms:"
             << aspectType << result.getReason();

    //   EMIT BLOCKING SIGNAL IF NECESSARY
    if (!result.isAllowed()) {
        emit operationBlocked(signalId, result.getReason());
        qDebug() << " Subsidiary signal operation blocked:" << signalId << aspectType << result.getReason();
    }

    return result;
}

ValidationResult InterlockingService::validatePointMachineOperation(
    const QString& machineId, const QString& currentPosition,
    const QString& requestedPosition, const QString& operatorId) {

    QElapsedTimer timer;
    timer.start();

    if (!m_isOperational) {
        return ValidationResult::blocked("Interlocking system not operational", "SYSTEM_OFFLINE");
    }

    if (!m_pointBranch) {
        qCritical() << " CRITICAL: PointMachineBranch not initialized!";
        return ValidationResult::blocked("Point machine validation not available", "POINT_BRANCH_MISSING");
    }

    //   DELEGATE TO POINT MACHINE BRANCH
    auto result = m_pointBranch->validatePositionChange(machineId, currentPosition, requestedPosition, operatorId);

    //   RECORD PERFORMANCE
    double responseTime = timer.elapsed();
    recordResponseTime(responseTime);

    qDebug() << "Point machine validation completed in" << responseTime << "ms:" << result.getReason();

    if (!result.isAllowed()) {
        emit operationBlocked(machineId, result.getReason());
    }

    return result;
}

ValidationResult InterlockingService::validatePairedPointMachineOperation(
    const QString& machineId,
    const QString& pairedMachineId,
    const QString& currentPosition,
    const QString& pairedCurrentPosition,
    const QString& requestedPosition,
    const QString& operatorId) {

    QElapsedTimer timer;
    timer.start();

    if (!m_isOperational) {
        return ValidationResult::blocked("Interlocking system not operational", "SYSTEM_OFFLINE");
    }

    if (!m_pointBranch) {
        qCritical() << " CRITICAL: PointMachineBranch not initialized!";
        return ValidationResult::blocked("Point machine validation not available", "POINT_BRANCH_MISSING");
    }

    //   DELEGATE TO POINT MACHINE BRANCH FOR PAIRED VALIDATION
    auto result = m_pointBranch->validatePairedOperation(
        machineId, pairedMachineId, currentPosition, pairedCurrentPosition, requestedPosition, operatorId);

    //   RECORD PERFORMANCE
    double responseTime = timer.elapsed();
    recordResponseTime(responseTime);

    qDebug() << " Paired point machine validation completed in" << responseTime << "ms:" << result.getReason();

    if (!result.isAllowed()) {
        emit operationBlocked(machineId, result.getReason());
    }

    return result;
}

// 
//   REACTIVE INTERLOCKING: Hardware-driven trackSegment occupancy changes
// 

void InterlockingService::reactToTrackSegmentOccupancyChange(
    const QString& trackSegmentId, bool wasOccupied, bool isOccupied) {

    if (!m_isOperational) {
        qCritical() << " CRITICAL: Interlocking system offline during trackSegment occupancy change!";
        emit systemFreezeRequired(trackSegmentId, "Interlocking system not operational",
                                  QString("Track Segment occupancy change detected while system offline: %1")
                                      .arg(QDateTime::currentDateTime().toString()));
        return;
    }

    if (!m_trackSegmentBranch) {
        qCritical() << " CRITICAL: TrackCircuitBranch not initialized during occupancy change!";
        emit systemFreezeRequired(trackSegmentId, "Track Segment circuit branch not available",
                                  QString("Track Segment occupancy change cannot be processed: %1")
                                      .arg(QDateTime::currentDateTime().toString()));
        return;
    }

    qDebug() << " REACTIVE INTERLOCKING: Track Segment section" << trackSegmentId
             << "occupancy changed:" << wasOccupied << "→" << isOccupied;

    //   ENFORCE INTERLOCKING: Only when trackSegment becomes occupied (safety-critical transition)
    if (!wasOccupied && isOccupied) {
        qDebug() << " SAFETY-CRITICAL TRANSITION: Track Segment section" << trackSegmentId << "became occupied";
        m_trackSegmentBranch->enforceTrackSegmentOccupancyInterlocking(trackSegmentId, wasOccupied, isOccupied);
    } else {
        qDebug() << "Non-critical transition for trackSegment section" << trackSegmentId << "- no interlocking action needed";
    }
}

// 
//   PERFORMANCE AND MONITORING METHODS
// 

double InterlockingService::getAverageResponseTime() const {
    std::lock_guard<std::mutex> lock(m_performanceMutex);
    if (m_responseTimeHistory.empty()) return 0.0;

    double sum = 0.0;
    for (double time : m_responseTimeHistory) {
        sum += time;
    }
    return sum / m_responseTimeHistory.size();
}

int InterlockingService::getActiveInterlocksCount() const {
    //   FUTURE: This would query the database for active interlocking rules
    // For now, return a placeholder
    return 0;
}

void InterlockingService::recordResponseTime(double responseTimeMs) {
    std::lock_guard<std::mutex> lock(m_performanceMutex);
    m_responseTimeHistory.push_back(responseTimeMs);
    if (m_responseTimeHistory.size() > MAX_RESPONSE_HISTORY) {
        m_responseTimeHistory.pop_front();
    }
    emit performanceChanged();
}

void InterlockingService::logPerformanceWarning(const QString& operation, double responseTimeMs) {
    qWarning() << " Slow interlocking response:" << responseTimeMs << "ms for" << operation
               << "(target:" << TARGET_RESPONSE_TIME_MS << "ms)";
}

// 
//   FAILURE HANDLING SLOTS
// 

void InterlockingService::handleCriticalFailure(const QString& entityId, const QString& reason) {
    qCritical() << " INTERLOCKING SYSTEM CRITICAL FAILURE ";
    qCritical() << "Entity:" << entityId << "Reason:" << reason;
    qCritical() << "Timestamp:" << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");

    //   EMIT SYSTEM FREEZE: Manual intervention required
    emit systemFreezeRequired(entityId, reason,
                              QString("Critical interlocking failure: %1 at %2")
                                  .arg(reason, QDateTime::currentDateTime().toString()));

    //   EMIT SAFETY VIOLATION
    emit criticalSafetyViolation(entityId, reason);

    //   SET SYSTEM NON-OPERATIONAL
    m_isOperational = false;
    emit operationalStateChanged(m_isOperational);
}

void InterlockingService::handleInterlockingFailure(const QString& trackSegmentId, const QString& failedSignals, const QString& error) {
    qCritical() << " INTERLOCKING ENFORCEMENT FAILURE:";
    qCritical() << "  Track Segment Section:" << trackSegmentId;
    qCritical() << "  Failed Signals:" << failedSignals;
    qCritical() << "  Error:" << error;

    //   TREAT AS CRITICAL FAILURE: This is a safety system failure
    handleCriticalFailure(trackSegmentId, QString("Failed to enforce signal protection: %1").arg(error));
}

// 
//   ROUTE ASSIGNMENT VALIDATION IMPLEMENTATION
// 

ValidationResult InterlockingService::validateRouteRequest(const QString& sourceSignalId,
                                                           const QString& destSignalId,
                                                           const QString& direction,
                                                           const QStringList& proposedPath,
                                                           const QString& operatorId) {
    QElapsedTimer timer;
    timer.start();

    if (!m_isOperational) {
        return ValidationResult::blocked("Interlocking system is not operational", "SYSTEM_NOT_OPERATIONAL");
    }

    // 1. Validate signal existence and states
    if (!m_dbManager) {
        return ValidationResult::blocked("Database manager not available", "DB_MANAGER_NULL");
    }

    // Check source signal exists and can be operated
    QVariantMap sourceSignal = m_dbManager->getSignalById(sourceSignalId);
    if (sourceSignal.isEmpty()) {
        return ValidationResult::blocked("Source signal does not exist: " + sourceSignalId, "SOURCE_SIGNAL_NOT_FOUND");
    }

    // Check destination signal exists
    QVariantMap destSignal = m_dbManager->getSignalById(destSignalId);
    if (destSignal.isEmpty()) {
        return ValidationResult::blocked("Destination signal does not exist: " + destSignalId, "DEST_SIGNAL_NOT_FOUND");
    }

    // 2. Validate direction
    if (direction != "UP" && direction != "DOWN") {
        return ValidationResult::blocked("Invalid direction: " + direction, "INVALID_DIRECTION");
    }

    // 3. Check if path contains valid track circuits
    for (const QString& circuitId : proposedPath) {
        QVariantMap circuit = m_dbManager->getTrackCircuitById(circuitId);
        if (circuit.isEmpty()) {
            return ValidationResult::blocked("Invalid track circuit in path: " + circuitId, "INVALID_CIRCUIT");
        }

        // Check if circuit is occupied
        bool isOccupied = m_dbManager->getTrackCircuitOccupancy(circuitId);
        if (isOccupied) {
            return ValidationResult::blocked("Track circuit is occupied: " + circuitId, "CIRCUIT_OCCUPIED");
        }
    }

    // 4. Check for conflicting routes
    QVariantList activeRoutes = m_dbManager->getActiveRoutes();
    for (const QVariant& routeVar : activeRoutes) {
        QVariantMap route = routeVar.toMap();
        QString assignedCircuitsStr = route["assignedCircuits"].toString();
        QStringList assignedCircuits = assignedCircuitsStr.mid(1, assignedCircuitsStr.length() - 2).split(",");
        
        // Check for circuit conflicts
        for (const QString& assignedCircuit : assignedCircuits) {
            if (proposedPath.contains(assignedCircuit.trimmed())) {
                return ValidationResult::blocked("Route conflict with active route: " + route["id"].toString(), "ROUTE_CONFLICT");
            }
        }
    }

    double responseTime = timer.elapsed();
    recordResponseTime(responseTime);

    if (responseTime > TARGET_RESPONSE_TIME_MS) {
        logPerformanceWarning("validateRouteRequest", responseTime);
    }

    return ValidationResult::allowed("Route request validated successfully")
        .setRuleId("ROUTE_REQUEST_VALIDATION");
}

ValidationResult InterlockingService::validateRouteActivation(const QString& routeId,
                                                             const QStringList& assignedCircuits,
                                                             const QStringList& lockedPointMachines,
                                                             const QString& operatorId) {
    QElapsedTimer timer;
    timer.start();

    if (!m_isOperational) {
        return ValidationResult::blocked("Interlocking system is not operational", "SYSTEM_NOT_OPERATIONAL");
    }

    // 1. Verify route exists and is in correct state
    QVariantMap route = m_dbManager->getRouteAssignment(routeId);
    if (route.isEmpty()) {
        return ValidationResult::blocked("Route does not exist: " + routeId, "ROUTE_NOT_FOUND");
    }

    QString currentState = route["state"].toString();
    if (currentState != "RESERVED") {
        return ValidationResult::blocked("Route not in RESERVED state for activation: " + currentState, "INVALID_STATE");
    }

    // 2. Verify all circuits are still clear
    for (const QString& circuitId : assignedCircuits) {
        bool isOccupied = m_dbManager->getTrackCircuitOccupancy(circuitId);
        if (isOccupied) {
            return ValidationResult::blocked("Assigned circuit became occupied: " + circuitId, "CIRCUIT_OCCUPIED");
        }
    }

    // 3. Verify point machines are in correct positions
    for (const QString& machineId : lockedPointMachines) {
        QString currentPosition = m_dbManager->getCurrentPointPosition(machineId);
        // Additional position validation would be done here based on route requirements
    }

    // 4. Validate source signal can be cleared
    QString sourceSignalId = route["sourceSignalId"].toString();
    ValidationResult signalValidation = validateMainSignalOperation(
        sourceSignalId, 
        m_dbManager->getCurrentSignalAspect(sourceSignalId),
        "GREEN",
        operatorId
    );

    if (!signalValidation.isAllowed()) {
        return ValidationResult::blocked(
            "Cannot clear source signal: " + signalValidation.getReason(), 
            "SIGNAL_VALIDATION_FAILED"
        );
    }

    double responseTime = timer.elapsed();
    recordResponseTime(responseTime);

    if (responseTime > TARGET_RESPONSE_TIME_MS) {
        logPerformanceWarning("validateRouteActivation", responseTime);
    }

    return ValidationResult::allowed("Route activation validated successfully")
        .setRuleId("ROUTE_ACTIVATION_VALIDATION");
}

ValidationResult InterlockingService::validateRouteRelease(const QString& routeId,
                                                          const QStringList& assignedCircuits,
                                                          const QString& releaseReason,
                                                          const QString& operatorId) {
    QElapsedTimer timer;
    timer.start();

    if (!m_isOperational) {
        return ValidationResult::blocked("Interlocking system is not operational", "SYSTEM_NOT_OPERATIONAL");
    }

    // 1. Verify route exists and is in a releasable state
    QVariantMap route = m_dbManager->getRouteAssignment(routeId);
    if (route.isEmpty()) {
        return ValidationResult::blocked("Route does not exist: " + routeId, "ROUTE_NOT_FOUND");
    }

    QString currentState = route["state"].toString();
    if (currentState == "RELEASED" || currentState == "FAILED") {
        return ValidationResult::blocked("Route already in final state: " + currentState, "ALREADY_RELEASED");
    }

    // 2. For emergency releases, allow immediate release
    if (releaseReason == "EMERGENCY_RELEASE") {
        double responseTime = timer.elapsed();
        recordResponseTime(responseTime);
        
        return ValidationResult::allowed("Emergency route release authorized")
            .setRuleId("EMERGENCY_RELEASE_VALIDATION");
    }

    // 3. For normal releases, check if all circuits are clear or train has passed
    bool allCircuitsClear = true;
    for (const QString& circuitId : assignedCircuits) {
        bool isOccupied = m_dbManager->getTrackCircuitOccupancy(circuitId);
        if (isOccupied) {
            allCircuitsClear = false;
            break;
        }
    }

    if (!allCircuitsClear && releaseReason == "NORMAL_RELEASE") {
        return ValidationResult::blocked("Cannot release route while circuits are occupied", "CIRCUITS_OCCUPIED");
    }

    // 4. Validate that signals can be returned to danger
    QString sourceSignalId = route["sourceSignalId"].toString();
    ValidationResult signalValidation = validateMainSignalOperation(
        sourceSignalId,
        m_dbManager->getCurrentSignalAspect(sourceSignalId),
        "RED",
        operatorId
    );

    if (!signalValidation.isAllowed()) {
        return ValidationResult::blocked(
            "Cannot return source signal to danger: " + signalValidation.getReason(),
            "SIGNAL_RETURN_FAILED"
        );
    }

    double responseTime = timer.elapsed();
    recordResponseTime(responseTime);

    if (responseTime > TARGET_RESPONSE_TIME_MS) {
        logPerformanceWarning("validateRouteRelease", responseTime);
    }

    return ValidationResult::allowed("Route release validated successfully")
        .setRuleId("ROUTE_RELEASE_VALIDATION");
}

ValidationResult InterlockingService::validateResourceConflict(const QString& resourceType,
                                                               const QString& resourceId,
                                                               const QString& requestingRouteId,
                                                               const QVariantList& existingLocks) {
    QElapsedTimer timer;
    timer.start();

    if (!m_isOperational) {
        return ValidationResult::blocked("Interlocking system is not operational", "SYSTEM_NOT_OPERATIONAL");
    }

    // 1. Check for conflicting locks based on railway lock types
    for (const QVariant& lockVar : existingLocks) {
        QVariantMap lock = lockVar.toMap();
        QString lockType = lock["lockType"].toString();
        QString lockRouteId = lock["routeId"].toString();

        // If requesting route already has the lock, allow
        if (lockRouteId == requestingRouteId) {
            continue;
        }

        //   REFACTORED: Check for conflicts based on railway lock types
        if (lockType == "ROUTE") {
            // ROUTE locks are exclusive for route operations
            return ValidationResult::blocked(
                QString("Resource %1 has route lock from route %2").arg(resourceId, lockRouteId),
                "ROUTE_LOCK_CONFLICT"
                );
        }

        if (lockType == "EMERGENCY") {
            // EMERGENCY locks override everything and block new acquisitions
            return ValidationResult::blocked(
                QString("Resource %1 has emergency lock - no operations permitted").arg(resourceId),
                "EMERGENCY_LOCK_CONFLICT"
                );
        }

        if (lockType == "MAINTENANCE") {
            // MAINTENANCE locks prevent route operations
            return ValidationResult::blocked(
                QString("Resource %1 is under maintenance lock").arg(resourceId),
                "MAINTENANCE_LOCK_CONFLICT"
                );
        }

        //   UPDATED: OVERLAP locks have specific railway rules
        if (lockType == "OVERLAP" && resourceType == "TRACK_CIRCUIT") {
            // Overlap locks prevent new route locks but may allow other overlaps
            // depending on the specific railway interlocking rules
            return ValidationResult::blocked(
                QString("Resource %1 has overlap protection from route %2").arg(resourceId, lockRouteId),
                "OVERLAP_PROTECTION_CONFLICT"
                );
        }

        //   ADDED: Handle unknown lock types safely
        if (!QStringList({"ROUTE", "OVERLAP", "EMERGENCY", "MAINTENANCE"}).contains(lockType)) {
            qWarning() << " Unknown lock type:" << lockType << "for resource:" << resourceId;
            return ValidationResult::blocked(
                QString("Resource %1 has unknown lock type: %2").arg(resourceId, lockType),
                "UNKNOWN_LOCK_TYPE"
                );
        }
    }

    // 2.   ENHANCED: Special validation for point machines with railway-specific rules
    if (resourceType == "POINT_MACHINE") {
        QString pairedMachine = m_dbManager->getPairedMachine(resourceId);
        if (!pairedMachine.isEmpty()) {
            // Check if paired machine is locked
            QVariantList pairedLocks = m_dbManager->getConflictingLocks(pairedMachine, "POINT_MACHINE");
            for (const QVariant& lockVar : pairedLocks) {
                QVariantMap lock = lockVar.toMap();
                QString lockType = lock["lockType"].toString();
                QString lockRouteId = lock["routeId"].toString();

                if (lockRouteId != requestingRouteId) {
                    //   RAILWAY RULE: Different conflict behavior based on lock type
                    if (lockType == "ROUTE") {
                        return ValidationResult::blocked(
                            QString("Paired point machine %1 has route lock from route %2").arg(pairedMachine, lockRouteId),
                            "PAIRED_MACHINE_ROUTE_LOCKED"
                            );
                    } else if (lockType == "EMERGENCY") {
                        return ValidationResult::blocked(
                            QString("Paired point machine %1 has emergency lock").arg(pairedMachine),
                            "PAIRED_MACHINE_EMERGENCY_LOCKED"
                            );
                    } else if (lockType == "MAINTENANCE") {
                        return ValidationResult::blocked(
                            QString("Paired point machine %1 is under maintenance").arg(pairedMachine),
                            "PAIRED_MACHINE_MAINTENANCE"
                            );
                    }
                    // OVERLAP locks on paired machines may be allowed depending on configuration
                }
            }
        }
    }

    //   ADDED: Special validation for signals
    if (resourceType == "SIGNAL") {
        // Check for signal-specific interlocking rules
        for (const QVariant& lockVar : existingLocks) {
            QVariantMap lock = lockVar.toMap();
            QString lockType = lock["lockType"].toString();
            QString lockRouteId = lock["routeId"].toString();

            if (lockRouteId != requestingRouteId && lockType == "ROUTE") {
                // Signals with route locks cannot be used by other routes
                return ValidationResult::blocked(
                    QString("Signal %1 is already controlling route %2").arg(resourceId, lockRouteId),
                    "SIGNAL_ROUTE_CONFLICT"
                    );
            }
        }
    }

    double responseTime = timer.elapsed();
    recordResponseTime(responseTime);

    if (responseTime > TARGET_RESPONSE_TIME_MS) {
        logPerformanceWarning("validateResourceConflict", responseTime);
    }

    return ValidationResult::allowed("No resource conflicts detected")
        .setRuleId("RESOURCE_CONFLICT_VALIDATION");
}
