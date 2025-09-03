#pragma once
#include <QObject>
#include <QDateTime>
#include "InterlockingService.h"

class DatabaseManager;

class PointMachineBranch : public QObject {
    Q_OBJECT

public:
    explicit PointMachineBranch(DatabaseManager* dbManager, QObject* parent = nullptr);

    // === PRIMARY VALIDATION METHODS ===
    ValidationResult validatePositionChange(const QString& machineId,
                                            const QString& currentPosition,
                                            const QString& requestedPosition,
                                            const QString& operatorId);

    // === NEW: PAIRED OPERATION VALIDATION ===
    ValidationResult validatePairedOperation(const QString& machineId,
                                             const QString& pairedMachineId,
                                             const QString& currentPosition,
                                             const QString& pairedCurrentPosition,
                                             const QString& newPosition,
                                             const QString& operatorId);

private:
    DatabaseManager* m_dbManager;

    // === CORE VALIDATION RULES ===
    ValidationResult checkPointMachineExists(const QString& machineId);
    ValidationResult checkPointMachineActive(const QString& machineId);
    ValidationResult checkOperationalStatus(const QString& machineId);
    ValidationResult checkLockingStatus(const QString& machineId);
    ValidationResult checkProtectingSignals(const QString& machineId, const QString& requestedPosition);
    ValidationResult checkTrackSegmentOccupancy(const QString& machineId, const QString& requestedPosition);
    ValidationResult checkRouteConflicts(const QString& machineId, const QString& requestedPosition);
    ValidationResult checkTimeLocking(const QString& machineId);
    ValidationResult checkDetectionLocking(const QString& machineId);
    ValidationResult checkConflictingPoints(const QString& machineId, const QString& requestedPosition);

    // === NEW: PAIRED-SPECIFIC VALIDATIONS ===
    ValidationResult checkPairedTrackSegmentOccupancy(const QString& machineId,
                                                      const QString& pairedMachineId,
                                                      const QString& newPosition);
    ValidationResult checkPairedConflicts(const QString& machineId,
                                          const QString& pairedMachineId,
                                          const QString& newPosition);

    // === HELPER METHODS ===
    QStringList getProtectingSignals(const QString& machineId);
    QStringList getAffectedTrackSegments(const QString& machineId, const QString& position);
    QStringList getConflictingPointMachines(const QString& machineId);
    bool areAllProtectingSignalsAtRed(const QStringList& signalIds);
    bool areAffectedTrackSegmentsClear(const QStringList& trackSegmentIds);
    bool isInTransition(const QString& machineId);

    // === NEW: PAIRED HELPER METHODS ===
    QString getCurrentPointPosition(const QString& machineId);
    QStringList getCombinedAffectedTrackSegments(const QString& machineId,
                                                 const QString& pairedMachineId,
                                                 const QString& position);

    // === DATA STRUCTURES ===
    struct PointMachineState {
        QString currentPosition;
        QString operatingStatus;
        bool isLocked;
        bool isActive;
        bool timeLockingActive;
        QDateTime timeLockExpiry;
        QStringList detectionLocks;
    };

    struct RouteConflictInfo {
        bool hasConflict;
        QString conflictingRoute;
        QString conflictReason;
    };

    PointMachineState getPointMachineState(const QString& machineId);
    RouteConflictInfo analyzeRouteImpact(const QString& machineId, const QString& requestedPosition);
};
