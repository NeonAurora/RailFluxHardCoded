#ifndef TRACKCIRCUITBRANCH_H
#define TRACKCIRCUITBRANCH_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QSqlQuery>
#include <QDateTime>
#include "InterlockingService.h"  //   Use existing ValidationResult

class DatabaseManager;

class TrackCircuitBranch : public QObject
{
    Q_OBJECT

public:
    explicit TrackCircuitBranch(DatabaseManager* dbManager, QObject* parent = nullptr);

    //   MAIN FUNCTION: Reactive enforcement when trackSegment becomes occupied
    void enforceTrackSegmentOccupancyInterlocking(const QString& trackSegmentId, bool wasOccupied, bool isOccupied);

    //   UTILITY: Basic trackSegment section checks (for safety verification)
    ValidationResult checkTrackSegmentExists(const QString& trackSegmentId);
    ValidationResult checkTrackSegmentActive(const QString& trackSegmentId);

signals:
    void systemFreezeRequired(const QString& trackSegmentId, const QString& reason, const QString& details);
    void automaticInterlockingCompleted(const QString& trackSegmentId, const QStringList& affectedSignals);
    void interlockingFailure(const QString& trackSegmentId, const QString& failedSignals, const QString& error);

private:
    DatabaseManager* m_dbManager;

    //   TRACK SEGMENT SECTION STATE: Simplified structure for hardware-based occupancy
    struct TrackSegmentState {
        bool isOccupied;
        bool isAssigned;
        bool isActive;
        QString occupiedBy;
        QString trackSegmentType;
        QStringList protectingSignals;
    };

    //   CORE METHODS: Track Segment section state and protection
    TrackSegmentState getTrackSegmentState(const QString& trackSegmentId);
    QStringList getProtectingSignalsFromThreeSources(const QString& trackSegmentId);
    QStringList getProtectingSignalsFromInterlockingRules(const QString& trackSegmentId);
    QStringList getProtectingSignalsFromTrackCircuits(const QString& trackSegmentId);
    QStringList getProtectingSignalsFromTrackSegments(const QString& trackSegmentId);

    //   ENFORCEMENT METHODS: Automatic signal control
    bool enforceSignalToRed(const QString& signalId, const QString& reason);
    bool enforceMultipleSignalsToRed(const QStringList& signalIds, const QString& reason);
    bool verifySignalIsRed(const QString& signalId);

    //   FAILURE HANDLING: Critical safety system failures
    void handleInterlockingFailure(const QString& trackSegmentId, const QString& failedSignals, const QString& error);
    void logCriticalFailure(const QString& trackSegmentId, const QString& details);
    void emitSystemFreeze(const QString& trackSegmentId, const QString& reason, const QString& details);
    void checkProtectingSignalsConsistency(
        const QString& trackSegmentId,
        const QStringList& fromInterlockingRules,
        const QStringList& fromTrackCircuits,
        const QStringList& fromTrackSegments);

    //   UTILITY METHODS: Safety checks
    bool areAllSignalsAtRed(const QStringList& signalIds);
    QString formatFailureDetails(const QString& trackSegmentId, const QStringList& failedSignals, const QString& error);
};

#endif // TRACKCIRCUITBRANCH_H
