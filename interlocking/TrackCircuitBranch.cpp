#include "TrackCircuitBranch.h"
#include "../database/DatabaseManager.h"
#include <QDebug>
#include <QThread>
#include <QDateTime>

TrackCircuitBranch::TrackCircuitBranch(DatabaseManager* dbManager, QObject* parent)
    : QObject(parent), m_dbManager(dbManager) {

    if (!dbManager) {
        qCritical() << " CRITICAL: TrackCircuitBranch initialized with null DatabaseManager!";
    }

    qDebug() << "  TrackCircuitBranch initialized for automatic interlocking enforcement";
}

// 
//   MAIN REACTIVE ENFORCEMENT METHOD
// 

void TrackCircuitBranch::enforceTrackSegmentOccupancyInterlocking(
    const QString& trackSegmentId, bool wasOccupied, bool isOccupied) {

    //   SAFETY: Only react to critical transition (trackSegment becoming occupied)
    if (wasOccupied || !isOccupied) {
        qDebug() << "No interlocking action needed for track segment" << trackSegmentId
                 << "- transition:" << wasOccupied << "→" << isOccupied;
        return;
    }

    qDebug() << " AUTOMATIC INTERLOCKING TRIGGERED: Track segment" << trackSegmentId
             << "became occupied - enforcing signal protection";

    //   SAFETY: Verify track segment exists and is operational
    auto existsResult = checkTrackSegmentExists(trackSegmentId);
    if (!existsResult.isAllowed()) {
        qCritical() << " CRITICAL: Track segment" << trackSegmentId << "not found during interlocking enforcement!";
        handleInterlockingFailure(trackSegmentId, "N/A", "Track segment not found: " + existsResult.getReason());
        return;
    }

    auto activeResult = checkTrackSegmentActive(trackSegmentId);
    if (!activeResult.isAllowed()) {
        qWarning() << " Track segment" << trackSegmentId << "is not active - skipping interlocking enforcement";
        return;
    }

    //   SAFETY: Get protecting signals from multiple sources for redundancy
    QStringList protectingSignals = getProtectingSignalsFromThreeSources(trackSegmentId);

    if (protectingSignals.isEmpty()) {
        qWarning() << " SAFETY WARNING: No protecting signals found for occupied track segment" << trackSegmentId;
        qWarning() << " This could indicate a configuration error or unprotected track segment";
        return;
    }

    qDebug() << "ENFORCING PROTECTION: Setting" << protectingSignals.size()
             << "protecting signals to RED for track segment" << trackSegmentId;
    qDebug() << "Protecting signals:" << protectingSignals;

    //   SAFETY: Force all protecting signals to RED - no validation, just enforce
    bool allSucceeded = enforceMultipleSignalsToRed(protectingSignals,
                                                    QString("AUTOMATIC: Track segment %1 occupied").arg(trackSegmentId));

    if (allSucceeded) {
        qDebug() << "  AUTOMATIC INTERLOCKING SUCCESSFUL: All protecting signals set to RED for track segment" << trackSegmentId;
        emit automaticInterlockingCompleted(trackSegmentId, protectingSignals);
    } else {
        qCritical() << " AUTOMATIC INTERLOCKING FAILED for track segment" << trackSegmentId;
        // handleInterlockingFailure is called within enforceMultipleSignalsToRed
    }
}

// 
//   TRACK SEGMENT SEGMENT VALIDATION METHODS
// 

ValidationResult TrackCircuitBranch::checkTrackSegmentExists(const QString& trackSegmentId) {
    auto trackSegmentData = m_dbManager->getTrackSegmentById(trackSegmentId);  //   Use existing method name
    if (trackSegmentData.isEmpty()) {
        return ValidationResult::blocked("Track segment not found: " + trackSegmentId, "TRACK_SEGMENT_NOT_FOUND");
    }
    return ValidationResult::allowed("Track segment exists");
}

ValidationResult TrackCircuitBranch::checkTrackSegmentActive(const QString& trackSegmentId) {
    auto trackSegmentState = getTrackSegmentState(trackSegmentId);
    if (!trackSegmentState.isActive) {
        return ValidationResult::blocked("Track segment is not active: " + trackSegmentId, "TRACK_SEGMENT_INACTIVE");
    }
    return ValidationResult::allowed("Track segment is active");
}

// 
//   TRACK SEGMENT SEGMENT STATE AND PROTECTION METHODS
// 

TrackCircuitBranch::TrackSegmentState TrackCircuitBranch::getTrackSegmentState(const QString& trackSegmentId) {
    TrackSegmentState state;
    auto trackSegmentData = m_dbManager->getTrackSegmentById(trackSegmentId);  //   Use existing method name

    if (!trackSegmentData.isEmpty()) {
        state.isOccupied = trackSegmentData["occupied"].toBool();
        state.isAssigned = trackSegmentData["assigned"].toBool();
        state.isActive = trackSegmentData["isActive"].toBool();
        state.occupiedBy = trackSegmentData["occupiedBy"].toString();
        state.trackSegmentType = trackSegmentData["trackSegmentType"].toString();

        //   Parse protecting signals array from database
        QString protectingSignalsStr = trackSegmentData["protectingSignals"].toString();
        if (!protectingSignalsStr.isEmpty() && protectingSignalsStr != "{}") {
            protectingSignalsStr = protectingSignalsStr.mid(1, protectingSignalsStr.length() - 2); // Remove { }
            state.protectingSignals = protectingSignalsStr.split(",", Qt::SkipEmptyParts);
            for (QString& signal : state.protectingSignals) {
                signal = signal.trimmed();
            }
        }
    }

    return state;
}

QStringList TrackCircuitBranch::getProtectingSignalsFromThreeSources(const QString& trackSegmentId) {
    //   SOURCE 1: Interlocking Rules table
    QStringList fromInterlockingRules = getProtectingSignalsFromInterlockingRules(trackSegmentId);

    //   SOURCE 2: Track Circuits table
    QStringList fromTrackCircuits = getProtectingSignalsFromTrackCircuits(trackSegmentId);

    //   SOURCE 3: Track Segments table
    QStringList fromTrackSegments = getProtectingSignalsFromTrackSegments(trackSegmentId);

    qDebug() << " PROTECTING SIGNALS for track segment" << trackSegmentId << ":";
    qDebug() << "   From interlocking rules:" << fromInterlockingRules;
    qDebug() << "   From track circuits:" << fromTrackCircuits;
    qDebug() << "   From track segments:" << fromTrackSegments;

    //   CONSISTENCY CHECK: Verify all sources match (triggers system freeze if not)
    checkProtectingSignalsConsistency(trackSegmentId, fromInterlockingRules, fromTrackCircuits, fromTrackSegments);

    //   UPDATED: AND Logic - Use authoritative source (prefer explicit config)
    QStringList authoritativeSignals;

    // Priority order: Interlocking Rules > Track Circuits > Track Segments
    if (!fromInterlockingRules.isEmpty()) {
        authoritativeSignals = fromInterlockingRules;
        qDebug() << "   Using interlocking rules as authoritative source";
    } else if (!fromTrackCircuits.isEmpty()) {
        authoritativeSignals = fromTrackCircuits;
        qDebug() << "   Using track circuits as authoritative source";
    } else if (!fromTrackSegments.isEmpty()) {
        authoritativeSignals = fromTrackSegments;
        qDebug() << "   Using track segments as authoritative source";
    }

    qDebug() << "   Authoritative signals (AND logic):" << authoritativeSignals;

    return authoritativeSignals;
}

//   NEW: Consistency validation method
void TrackCircuitBranch::checkProtectingSignalsConsistency(
    const QString& trackSegmentId,
    const QStringList& fromInterlockingRules,
    const QStringList& fromTrackCircuits,
    const QStringList& fromTrackSegments) {

    // Normalize lists for comparison (sort and trim)
    auto normalizeList = [](QStringList list) {
        for (QString& signal : list) {
            signal = signal.trimmed();
        }
        list.sort();
        return list;
    };

    QStringList rules = normalizeList(fromInterlockingRules);
    QStringList circuits = normalizeList(fromTrackCircuits);
    QStringList segments = normalizeList(fromTrackSegments);

    // Check if any sources have data
    bool hasRulesData = !rules.isEmpty();
    bool hasCircuitsData = !circuits.isEmpty();
    bool hasSegmentsData = !segments.isEmpty();

    if (!hasRulesData && !hasCircuitsData && !hasSegmentsData) {
        qWarning() << " CONSISTENCY WARNING: No protecting signals found in ANY source for track segment" << trackSegmentId;
        return;
    }

    //   UPDATED: AND Logic - All sources must match exactly
    bool isConsistent = true;
    QStringList inconsistencies;
    QStringList activeSources;
    QStringList activeData;

    // Build active sources list for reporting
    if (hasRulesData) {
        activeSources.append("InterlockingRules");
        activeData.append(QString("Rules: %1").arg(rules.join(",")));
    }
    if (hasCircuitsData) {
        activeSources.append("TrackCircuits");
        activeData.append(QString("Circuits: %1").arg(circuits.join(",")));
    }
    if (hasSegmentsData) {
        activeSources.append("TrackSegments");
        activeData.append(QString("Segments: %1").arg(segments.join(",")));
    }

    //   CRITICAL: AND logic - ALL active sources must match exactly
    QStringList referenceData;
    QString referenceSource;

    // Use first available source as reference
    if (hasRulesData) {
        referenceData = rules;
        referenceSource = "InterlockingRules";
    } else if (hasCircuitsData) {
        referenceData = circuits;
        referenceSource = "TrackCircuits";
    } else if (hasSegmentsData) {
        referenceData = segments;
        referenceSource = "TrackSegments";
    }

    //   CRITICAL: Check all other sources against reference
    if (hasRulesData && hasCircuitsData && rules != circuits) {
        isConsistent = false;
        inconsistencies.append("InterlockingRules≠TrackCircuits");
    }
    if (hasRulesData && hasSegmentsData && rules != segments) {
        isConsistent = false;
        inconsistencies.append("InterlockingRules≠TrackSegments");
    }
    if (hasCircuitsData && hasSegmentsData && circuits != segments) {
        isConsistent = false;
        inconsistencies.append("TrackCircuits≠TrackSegments");
    }

    //   CRITICAL: System fault detected - emit system freeze
    if (!isConsistent) {
        QString reason = QString("CRITICAL DATA INCONSISTENCY: Protecting signals mismatch for track segment %1").arg(trackSegmentId);
        QString details = QString("Inconsistencies: %1 | Data: %2 | Sources: %3")
                              .arg(inconsistencies.join(", "))
                              .arg(activeData.join(" | "))
                              .arg(activeSources.join(", "));

        qCritical() << " CRITICAL SYSTEM FAULT: Data inconsistency detected for track segment" << trackSegmentId;
        qCritical() << "   Active sources:" << activeSources;
        qCritical() << "   Inconsistencies:" << inconsistencies;
        qCritical() << "   Data:" << activeData.join(" | ");
        qCritical() << " EMITTING SYSTEM FREEZE - MANUAL INTERVENTION REQUIRED";

        //   EMIT SYSTEM FREEZE: Critical safety fault detected
        logCriticalFailure(trackSegmentId, details);
        emitSystemFreeze(trackSegmentId, reason, details);

    } else {
        qDebug() << "  CONSISTENCY OK: All active sources agree on protecting signals for track segment" << trackSegmentId;
        qDebug() << "   Reference data:" << referenceData.join(",") << "from" << referenceSource;
    }
}

QStringList TrackCircuitBranch::getProtectingSignalsFromInterlockingRules(const QString& trackSegmentId) {
    if (!m_dbManager) return QStringList();

    //   UPDATED: Get circuit ID first, then query interlocking rules by circuit ID
    QString circuitId = m_dbManager->getCircuitIdByTrackSegmentId(trackSegmentId);
    if (circuitId.isEmpty()) {
        qWarning() << " No circuit ID found for track segment" << trackSegmentId;
        return QStringList();
    }

    qDebug() << " Track segment" << trackSegmentId << "belongs to circuit" << circuitId;
    return m_dbManager->getProtectingSignalsFromInterlockingRules(circuitId);
}

QStringList TrackCircuitBranch::getProtectingSignalsFromTrackCircuits(const QString& trackSegmentId) {
    if (!m_dbManager) return QStringList();

    //   UPDATED: Get circuit ID first, then query track circuits by circuit ID
    QString circuitId = m_dbManager->getCircuitIdByTrackSegmentId(trackSegmentId);
    if (circuitId.isEmpty()) {
        qWarning() << " No circuit ID found for track segment" << trackSegmentId;
        return QStringList();
    }

    qDebug() << " Track segment" << trackSegmentId << "belongs to circuit" << circuitId;
    return m_dbManager->getProtectingSignalsFromTrackCircuits(circuitId);
}

//   UNCHANGED: This method stays the same since it queries track_segments directly
QStringList TrackCircuitBranch::getProtectingSignalsFromTrackSegments(const QString& trackSegmentId) {
    if (!m_dbManager) return QStringList();

    //   UNCHANGED: Use DatabaseManager API for track segments lookup
    return m_dbManager->getProtectingSignalsFromTrackSegments(trackSegmentId);
}

// 
//   SIGNAL ENFORCEMENT METHODS
// 

bool TrackCircuitBranch::enforceSignalToRed(const QString& signalId, const QString& reason) {
    qDebug() << "ENFORCING RED: Signal" << signalId << "Reason:" << reason;

    //   SAFETY: Check if signal is already RED to avoid unnecessary operations
    if (verifySignalIsRed(signalId)) {
        qDebug() << "  Signal" << signalId << "already RED - no action needed";
        return true;
    }

    //   FORCE: Use database manager to set signal to RED (bypasses normal validation)
    bool success = m_dbManager->updateSignalAspect(signalId, "MAIN", "RED");

    if (success) {
        qDebug() << "  ENFORCED: Signal" << signalId << "set to RED";

        //   VERIFY: Double-check that signal is actually RED
        QThread::msleep(50); // Brief delay to ensure database update is committed
        if (!verifySignalIsRed(signalId)) {
            qCritical() << " VERIFICATION FAILED: Signal" << signalId << "not confirmed RED after enforcement!";
            return false;
        }
    } else {
        qCritical() << " ENFORCEMENT FAILED: Could not set signal" << signalId << "to RED";
    }

    return success;
}

bool TrackCircuitBranch::enforceMultipleSignalsToRed(const QStringList& signalIds, const QString& reason) {
    if (signalIds.isEmpty()) {
        qWarning() << " No signals to enforce - empty list provided";
        return true;
    }

    bool allSucceeded = true;
    QStringList failedSignals;
    QStringList succeededSignals;

    qDebug() << "ENFORCING MULTIPLE SIGNALS TO RED:" << signalIds.size() << "signals";

    for (const QString& signalId : signalIds) {
        if (enforceSignalToRed(signalId, reason)) {
            succeededSignals.append(signalId);
        } else {
            allSucceeded = false;
            failedSignals.append(signalId);
        }
    }

    if (!allSucceeded) {
        QString trackSegmentId = reason.contains("Track segment") ?
                                     reason.split(" ")[2] : "UNKNOWN"; // Extract track segment ID from reason

        qCritical() << " CRITICAL SAFETY FAILURE: Failed to set signals to RED";
        qCritical() << " Succeeded signals:" << succeededSignals;
        qCritical() << " Failed signals:" << failedSignals;

        handleInterlockingFailure(trackSegmentId, failedSignals.join(","), "Failed to enforce RED aspect on multiple signals");
    }

    return allSucceeded;
}

bool TrackCircuitBranch::verifySignalIsRed(const QString& signalId) {
    auto signalData = m_dbManager->getSignalById(signalId);
    if (!signalData.isEmpty()) {
        QString currentAspect = signalData["currentAspect"].toString();
        return currentAspect == "RED";
    }

    qWarning() << " Could not verify signal" << signalId << "- signal data not found";
    return false;
}

bool TrackCircuitBranch::areAllSignalsAtRed(const QStringList& signalIds) {
    for (const QString& signalId : signalIds) {
        if (!verifySignalIsRed(signalId)) {
            qDebug() << " Signal" << signalId << "is not at RED";
            return false;
        }
    }
    return true;
}

// 
//   FAILURE HANDLING METHODS
// 

void TrackCircuitBranch::handleInterlockingFailure(const QString& trackSegmentId, const QString& failedSignals, const QString& error) {
    QString details = formatFailureDetails(trackSegmentId, failedSignals.split(","), error);

    logCriticalFailure(trackSegmentId, details);
    emitSystemFreeze(trackSegmentId, "Failed to enforce signal protection for occupied track segment", details);

    //   EMIT specific interlocking failure signal
    emit interlockingFailure(trackSegmentId, failedSignals, error);
}

void TrackCircuitBranch::logCriticalFailure(const QString& trackSegmentId, const QString& details) {
    qCritical() << " CRITICAL INTERLOCKING SYSTEM FAILURE ";
    qCritical() << "Track Segment Segment ID:" << trackSegmentId;
    qCritical() << "Failure Details:" << details;
    qCritical() << "Timestamp:" << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    qCritical() << "Thread:" << QThread::currentThread();
    qCritical() << " IMMEDIATE MANUAL INTERVENTION REQUIRED ";
}

void TrackCircuitBranch::emitSystemFreeze(const QString& trackSegmentId, const QString& reason, const QString& details) {
    qCritical() << " EMITTING SYSTEM FREEZE SIGNAL for track segment" << trackSegmentId;
    emit systemFreezeRequired(trackSegmentId, reason, details);
}

QString TrackCircuitBranch::formatFailureDetails(const QString& trackSegmentId, const QStringList& failedSignals, const QString& error) {
    return QString("Track Segment Segment: %1, Failed Signals: %2, Error: %3, Time: %4")
    .arg(trackSegmentId)
        .arg(failedSignals.join(", "))
        .arg(error)
        .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz"));
}
