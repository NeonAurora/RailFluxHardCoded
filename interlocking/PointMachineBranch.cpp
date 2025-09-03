#include "PointMachineBranch.h"
#include "../database/DatabaseManager.h"
#include <QDebug>
#include <QDateTime>

PointMachineBranch::PointMachineBranch(DatabaseManager* dbManager, QObject* parent)
    : QObject(parent), m_dbManager(dbManager) {}

ValidationResult PointMachineBranch::validatePositionChange(
    const QString& machineId, const QString& currentPosition,
    const QString& requestedPosition, const QString& operatorId) {

    qDebug() << "Validating point machine operation:" << machineId
             << "from" << currentPosition << "to" << requestedPosition;

    // 1. Basic point machine validation
    auto existsResult = checkPointMachineExists(machineId);
    if (!existsResult.isAllowed()) return existsResult;

    auto activeResult = checkPointMachineActive(machineId);
    if (!activeResult.isAllowed()) return activeResult;

    // 2. Check if change is actually needed
    if (currentPosition == requestedPosition) {
        return ValidationResult::allowed("No change required - point already in requested position");
    }

    // 3. Operational status validation
    auto operationalResult = checkOperationalStatus(machineId);
    if (!operationalResult.isAllowed()) return operationalResult;

    // 4. Locking status validation
    auto lockingResult = checkLockingStatus(machineId);
    if (!lockingResult.isAllowed()) return lockingResult;

    // 5. Time-based locking validation
    auto timeLockResult = checkTimeLocking(machineId);
    if (!timeLockResult.isAllowed()) return timeLockResult;

    // 6. Detection locking validation
    auto detectionResult = checkDetectionLocking(machineId);
    if (!detectionResult.isAllowed()) return detectionResult;

    // 7. Protecting signals validation
    auto signalResult = checkProtectingSignals(machineId, requestedPosition);
    if (!signalResult.isAllowed()) return signalResult;

    // 8. Track Segment occupancy validation
    auto trackSegmentResult = checkTrackSegmentOccupancy(machineId, requestedPosition);
    if (!trackSegmentResult.isAllowed()) return trackSegmentResult;

    // 9. Conflicting point machines validation
    auto conflictResult = checkConflictingPoints(machineId, requestedPosition);
    if (!conflictResult.isAllowed()) return conflictResult;

    // 10. Route conflicts validation
    auto routeResult = checkRouteConflicts(machineId, requestedPosition);
    if (!routeResult.isAllowed()) return routeResult;

    return ValidationResult::allowed("All point machine validations passed");
}

// === NEW: PAIRED OPERATION VALIDATION ===
ValidationResult PointMachineBranch::validatePairedOperation(
    const QString& machineId,
    const QString& pairedMachineId,
    const QString& currentPosition,
    const QString& pairedCurrentPosition,
    const QString& newPosition,
    const QString& operatorId) {

    qDebug() << " Validating paired point machine operation:"
             << machineId << "+" << pairedMachineId
             << "to position:" << newPosition;

    // === STEP 1: Validate both machines individually ===
    auto result1 = validatePositionChange(machineId, currentPosition, newPosition, operatorId);
    if (!result1.isAllowed()) {
        qDebug() << " Primary machine validation failed:" << result1.getReason();
        return result1;
    }

    auto result2 = validatePositionChange(pairedMachineId, pairedCurrentPosition, newPosition, operatorId);
    if (!result2.isAllowed()) {
        qDebug() << " Paired machine validation failed:" << result2.getReason();
        return result2;
    }

    // === STEP 2: Paired-specific validations ===

    // Check combined track segment occupancy
    auto pairedTrackResult = checkPairedTrackSegmentOccupancy(machineId, pairedMachineId, newPosition);
    if (!pairedTrackResult.isAllowed()) return pairedTrackResult;

    // Check paired conflicts
    auto pairedConflictResult = checkPairedConflicts(machineId, pairedMachineId, newPosition);
    if (!pairedConflictResult.isAllowed()) return pairedConflictResult;

    qDebug() << "  Paired operation validation passed for" << machineId << "+" << pairedMachineId;
    return ValidationResult::allowed("Paired operation validation passed");
}

// === NEW: PAIRED-SPECIFIC VALIDATIONS ===
ValidationResult PointMachineBranch::checkPairedTrackSegmentOccupancy(
    const QString& machineId,
    const QString& pairedMachineId,
    const QString& newPosition) {

    QStringList combinedAffectedSegments = getCombinedAffectedTrackSegments(
        machineId, pairedMachineId, newPosition);

    for (const QString& segmentId : combinedAffectedSegments) {
        auto segmentData = m_dbManager->getTrackSegmentById(segmentId);
        if (!segmentData.isEmpty() && segmentData["occupied"].toBool()) {
            return ValidationResult::blocked(
                       QString("Cannot operate paired machines %1+%2: combined affected track segment %3 is occupied by %4")
                           .arg(machineId, pairedMachineId, segmentId, segmentData["occupiedBy"].toString()),
                       "PAIRED_OPERATION_TRACK_OCCUPIED"
                       ).addAffectedEntity(segmentId);
        }
    }

    return ValidationResult::allowed();
}

ValidationResult PointMachineBranch::checkPairedConflicts(
    const QString& machineId,
    const QString& pairedMachineId,
    const QString& newPosition) {

    // Check if operating both machines simultaneously creates geometric conflicts
    QStringList machine1Conflicts = getConflictingPointMachines(machineId);
    QStringList machine2Conflicts = getConflictingPointMachines(pairedMachineId);

    // Remove the paired machines from each other's conflict lists
    machine1Conflicts.removeAll(pairedMachineId);
    machine2Conflicts.removeAll(machineId);

    // Check conflicts for machine 1
    for (const QString& conflictingMachineId : machine1Conflicts) {
        auto conflictingData = m_dbManager->getPointMachineById(conflictingMachineId);
        if (!conflictingData.isEmpty()) {
            QString conflictingPosition = conflictingData["position"].toString();
            if (conflictingPosition != "NORMAL") {
                return ValidationResult::blocked(
                           QString("Cannot operate paired machines %1+%2: %1 conflicts with %3 in %4 position")
                               .arg(machineId, pairedMachineId, conflictingMachineId, conflictingPosition),
                           "PAIRED_OPERATION_CONFLICT"
                           ).addAffectedEntity(conflictingMachineId);
            }
        }
    }

    // Check conflicts for machine 2
    for (const QString& conflictingMachineId : machine2Conflicts) {
        auto conflictingData = m_dbManager->getPointMachineById(conflictingMachineId);
        if (!conflictingData.isEmpty()) {
            QString conflictingPosition = conflictingData["position"].toString();
            if (conflictingPosition != "NORMAL") {
                return ValidationResult::blocked(
                           QString("Cannot operate paired machines %1+%2: %2 conflicts with %3 in %4 position")
                               .arg(machineId, pairedMachineId, conflictingMachineId, conflictingPosition),
                           "PAIRED_OPERATION_CONFLICT"
                           ).addAffectedEntity(conflictingMachineId);
            }
        }
    }

    return ValidationResult::allowed();
}

// === NEW: PAIRED HELPER METHODS ===
QString PointMachineBranch::getCurrentPointPosition(const QString& machineId) {
    auto pmData = m_dbManager->getPointMachineById(machineId);
    if (!pmData.isEmpty()) {
        return pmData["position"].toString();
    }
    return QString();
}

QStringList PointMachineBranch::getCombinedAffectedTrackSegments(
    const QString& machineId,
    const QString& pairedMachineId,
    const QString& position) {

    QStringList machine1Segments = getAffectedTrackSegments(machineId, position);
    QStringList machine2Segments = getAffectedTrackSegments(pairedMachineId, position);

    // Combine and remove duplicates
    QStringList combined = machine1Segments + machine2Segments;
    combined.removeDuplicates();

    return combined;
}

// === EXISTING VALIDATION METHODS (unchanged) ===
ValidationResult PointMachineBranch::checkPointMachineExists(const QString& machineId) {
    auto pmData = m_dbManager->getPointMachineById(machineId);
    if (pmData.isEmpty()) {
        return ValidationResult::blocked("Point machine not found: " + machineId, "POINT_MACHINE_NOT_FOUND");
    }
    return ValidationResult::allowed();
}

ValidationResult PointMachineBranch::checkPointMachineActive(const QString& machineId) {
    auto pmData = m_dbManager->getPointMachineById(machineId);
    if (pmData.isEmpty()) {
        return ValidationResult::blocked("Point machine not found: " + machineId, "POINT_MACHINE_NOT_FOUND");
    }

    //  Check if the map contains isActive field, if not assume active
    if (pmData.contains("isActive") && !pmData["isActive"].toBool()) {
        return ValidationResult::blocked("Point machine is not active: " + machineId, "POINT_MACHINE_INACTIVE");
    }

    // If no isActive field exists, consider the machine active (since it exists in DB)
    return ValidationResult::allowed();
}

ValidationResult PointMachineBranch::checkOperationalStatus(const QString& machineId) {
    auto pmState = getPointMachineState(machineId);

    if (pmState.operatingStatus == "IN_TRANSITION") {
        return ValidationResult::blocked(
            QString("Point machine %1 is already in transition").arg(machineId),
            "POINT_MACHINE_IN_TRANSITION"
            );
    }

    if (pmState.operatingStatus == "FAILED") {
        return ValidationResult::blocked(
            QString("Point machine %1 has failed status").arg(machineId),
            "POINT_MACHINE_FAILED"
            );
    }

    if (pmState.operatingStatus == "LOCKED_OUT") {
        return ValidationResult::blocked(
            QString("Point machine %1 is locked out").arg(machineId),
            "POINT_MACHINE_LOCKED_OUT"
            );
    }

    return ValidationResult::allowed();
}

ValidationResult PointMachineBranch::checkLockingStatus(const QString& machineId) {
    auto pmState = getPointMachineState(machineId);

    if (pmState.isLocked) {
        return ValidationResult::blocked(
            QString("Point machine %1 is locked").arg(machineId),
            "POINT_MACHINE_LOCKED"
            );
    }

    return ValidationResult::allowed();
}

ValidationResult PointMachineBranch::checkTimeLocking(const QString& machineId) {
    auto pmState = getPointMachineState(machineId);

    if (pmState.timeLockingActive) {
        QDateTime now = QDateTime::currentDateTime();
        if (pmState.timeLockExpiry > now) {
            return ValidationResult::blocked(
                QString("Point machine %1 is time-locked until %2")
                    .arg(machineId, pmState.timeLockExpiry.toString()),
                "POINT_MACHINE_TIME_LOCKED"
                );
        }
    }

    return ValidationResult::allowed();
}

ValidationResult PointMachineBranch::checkDetectionLocking(const QString& machineId) {
    auto pmState = getPointMachineState(machineId);

    // Check if any detection locks are active
    for (const QString& lockingTrackSegmentId : pmState.detectionLocks) {
        auto trackSegmentData = m_dbManager->getTrackSegmentById(lockingTrackSegmentId);
        if (!trackSegmentData.isEmpty() && trackSegmentData["occupied"].toBool()) {
            return ValidationResult::blocked(
                       QString("Point machine %1 is detection-locked by occupied trackSegment %2")
                           .arg(machineId, lockingTrackSegmentId),
                       "POINT_MACHINE_DETECTION_LOCKED"
                       ).addAffectedEntity(lockingTrackSegmentId);
        }
    }

    return ValidationResult::allowed();
}

ValidationResult PointMachineBranch::checkProtectingSignals(const QString& machineId, const QString& requestedPosition) {
    QStringList protectingSignals = getProtectingSignals(machineId);

    if (!protectingSignals.isEmpty()) {
        if (!areAllProtectingSignalsAtRed(protectingSignals)) {
            QStringList nonRedSignals;
            for (const QString& signalId : protectingSignals) {
                auto signalData = m_dbManager->getSignalById(signalId);
                if (!signalData.isEmpty()) {
                    QString aspect = signalData["currentAspect"].toString();
                    if (aspect != "RED") {
                        nonRedSignals.append(QString("%1(%2)").arg(signalId, aspect));
                    }
                }
            }

            return ValidationResult::blocked(
                QString("Cannot operate point machine %1: protecting signals not at RED: %2")
                    .arg(machineId, nonRedSignals.join(", ")),
                "PROTECTING_SIGNALS_NOT_RED"
                );
        }
    }

    return ValidationResult::allowed();
}

ValidationResult PointMachineBranch::checkTrackSegmentOccupancy(const QString& machineId, const QString& requestedPosition) {
    QStringList affectedTrackSegments = getAffectedTrackSegments(machineId, requestedPosition);

    for (const QString& trackSegmentId : affectedTrackSegments) {
        auto trackSegmentData = m_dbManager->getTrackSegmentById(trackSegmentId);
        if (!trackSegmentData.isEmpty() && trackSegmentData["occupied"].toBool()) {
            return ValidationResult::blocked(
                       QString("Cannot operate point machine %1: affected trackSegment %2 is occupied by %3")
                           .arg(machineId, trackSegmentId, trackSegmentData["occupiedBy"].toString()),
                       "AFFECTED_TRACK_SEGMENT_OCCUPIED"
                       ).addAffectedEntity(trackSegmentId);
        }
    }

    return ValidationResult::allowed();
}

ValidationResult PointMachineBranch::checkConflictingPoints(const QString& machineId, const QString& requestedPosition) {
    QStringList conflictingMachines = getConflictingPointMachines(machineId);

    for (const QString& conflictingMachineId : conflictingMachines) {
        auto conflictingPMData = m_dbManager->getPointMachineById(conflictingMachineId);
        if (!conflictingPMData.isEmpty()) {
            QString conflictingPosition = conflictingPMData["position"].toString();

            // Implement specific conflict rules based on your layout
            if (conflictingPosition != "NORMAL") {
                return ValidationResult::blocked(
                           QString("Cannot operate point machine %1: conflicts with %2 in %3 position")
                               .arg(machineId, conflictingMachineId, conflictingPosition),
                           "CONFLICTING_POINT_MACHINE"
                           ).addAffectedEntity(conflictingMachineId);
            }
        }
    }

    return ValidationResult::allowed();
}

ValidationResult PointMachineBranch::checkRouteConflicts(const QString& machineId, const QString& requestedPosition) {
    auto routeConflict = analyzeRouteImpact(machineId, requestedPosition);

    if (routeConflict.hasConflict) {
        return ValidationResult::blocked(
            QString("Cannot operate point machine %1: %2")
                .arg(machineId, routeConflict.conflictReason),
            "ROUTE_CONFLICT"
            );
    }

    return ValidationResult::allowed();
}

// === EXISTING HELPER METHODS (unchanged) ===
PointMachineBranch::PointMachineState PointMachineBranch::getPointMachineState(const QString& machineId) {
    PointMachineState state;
    auto pmData = m_dbManager->getPointMachineById(machineId);

    if (!pmData.isEmpty()) {
        state.currentPosition = pmData["position"].toString();
        state.operatingStatus = pmData["operatingStatus"].toString();
        state.isActive = pmData["isActive"].toBool();

        // Default values for fields not yet in database
        state.isLocked = false;
        state.timeLockingActive = false;
        state.timeLockExpiry = QDateTime();
        state.detectionLocks = QStringList();
    }

    return state;
}

QStringList PointMachineBranch::getProtectingSignals(const QString& machineId) {
    auto pmData = m_dbManager->getPointMachineById(machineId);
    if (!pmData.isEmpty()) {
        // TODO: Add protected_signals field to database schema
        // return pmData["protectedSignals"].toStringList();
    }
    return QStringList();
}

QStringList PointMachineBranch::getAffectedTrackSegments(const QString& machineId, const QString& position) {
    auto pmData = m_dbManager->getPointMachineById(machineId);
    if (!pmData.isEmpty()) {
        QVariantMap rootTrackSegment = pmData["rootTrackSegment"].toMap();
        QVariantMap normalTrackSegment = pmData["normalTrackSegment"].toMap();
        QVariantMap reverseTrackSegment = pmData["reverseTrackSegment"].toMap();

        QStringList trackSegments;
        trackSegments.append(rootTrackSegment["trackSegmentId"].toString());

        if (position == "NORMAL") {
            trackSegments.append(normalTrackSegment["trackSegmentId"].toString());
        } else {
            trackSegments.append(reverseTrackSegment["trackSegmentId"].toString());
        }

        return trackSegments;
    }
    return QStringList();
}

QStringList PointMachineBranch::getConflictingPointMachines(const QString& machineId) {
    auto pmData = m_dbManager->getPointMachineById(machineId);
    if (!pmData.isEmpty()) {
        // TODO: Add conflicting_points field to database schema
        // return pmData["conflictingPoints"].toStringList();
    }
    return QStringList();
}

bool PointMachineBranch::areAllProtectingSignalsAtRed(const QStringList& signalIds) {
    for (const QString& signalId : signalIds) {
        auto signalData = m_dbManager->getSignalById(signalId);
        if (!signalData.isEmpty()) {
            QString aspect = signalData["currentAspect"].toString();
            if (aspect != "RED") {
                return false;
            }
        }
    }
    return true;
}

PointMachineBranch::RouteConflictInfo PointMachineBranch::analyzeRouteImpact(
    const QString& machineId, const QString& requestedPosition) {

    RouteConflictInfo info;
    info.hasConflict = false;
    info.conflictingRoute = "";
    info.conflictReason = "";

    // TODO: Implement route conflict analysis
    return info;
}
