#pragma once

#include <QObject>
#include <QVariantMap>
#include <QVariantList>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QTimer>
#include <QUuid>
#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <memory>

// Forward declarations
class DatabaseManager;

namespace RailFlux::Route {

enum class OverlapType {
    FIXED,             // Fixed overlap distance
    VARIABLE,          // Variable based on train data
    FLANK_PROTECTION   // Flank protection overlap
};

enum class OverlapState {
    RESERVED,          // Overlap circuits reserved
    ACTIVE,            // Train using overlap region
    RELEASING,         // Release timer running
    RELEASED           // Overlap fully released
};

struct OverlapDefinition {
    QString signalId;
    QStringList overlapCircuitIds;
    QStringList releaseTriggerCircuitIds;
    OverlapType type = OverlapType::FIXED;
    int holdSeconds = 30;
    bool isActive = true;
    
    QString key() const { return signalId; }
};

struct ActiveOverlap {
    QUuid routeId;
    QString signalId;
    QStringList reservedCircuits;
    QStringList releaseTriggerCircuits;
    OverlapState state = OverlapState::RESERVED;
    QDateTime reservedAt;
    QDateTime releaseTimerStarted;
    QDateTime scheduledReleaseAt;
    int holdSeconds = 30;
    QString operatorId;
    
    bool isExpired() const {
        return scheduledReleaseAt.isValid() && QDateTime::currentDateTime() > scheduledReleaseAt;
    }
    
    QString key() const { return QString("%1:%2").arg(routeId.toString(), signalId); }
};

struct OverlapCalculationRequest {
    QString sourceSignalId;
    QString destSignalId;
    QString direction;
    QVariantMap trainData;  //characteristics
    QString operatorId = "system";
};

struct OverlapCalculationResult {
    bool success = false;
    QString error;
    QStringList overlapCircuits;
    QStringList releaseTriggerCircuits;
    int calculatedHoldSeconds = 30;
    double calculationTimeMs = 0.0;
    QString calculationMethod; // "FIXED", "DYNAMIC", "SAFETY_MARGIN"
};

class OverlapService : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isOperational READ isOperational NOTIFY operationalStateChanged)
    Q_PROPERTY(int activeOverlaps READ activeOverlaps NOTIFY overlapCountChanged)
    Q_PROPERTY(int pendingReleases READ pendingReleases NOTIFY overlapCountChanged)
    Q_PROPERTY(double averageHoldTimeSeconds READ averageHoldTimeSeconds NOTIFY statisticsChanged)

public:
    explicit OverlapService(DatabaseManager* dbManager, QObject* parent = nullptr);
    ~OverlapService();

    // Properties
    bool isOperational() const { return m_isOperational; }
    int activeOverlaps() const { return m_activeOverlaps.size(); }
    int pendingReleases() const;
    double averageHoldTimeSeconds() const { return m_averageHoldTime; }

    // Overlap calculation and management
    Q_INVOKABLE QVariantMap calculateOverlap(
        const QString& sourceSignalId,
        const QString& destSignalId,
        const QString& direction,
        const QVariantMap& trainData = QVariantMap()
    );

    Q_INVOKABLE QVariantMap reserveOverlap(
        const QString& routeId,
        const QString& signalId,
        const QStringList& overlapCircuits,
        const QStringList& releaseTriggerCircuits,
        const QString& operatorId = "system"
    );

    Q_INVOKABLE bool activateOverlap(
        const QString& routeId,
        const QString& signalId
    );

    Q_INVOKABLE bool startOverlapRelease(
        const QString& routeId,
        const QString& signalId,
        const QString& triggerReason = "automatic"
    );

    Q_INVOKABLE bool releaseOverlap(
        const QString& routeId,
        const QString& signalId,
        bool immediate = false
    );

    Q_INVOKABLE bool forceReleaseOverlap(
        const QString& routeId,
        const QString& signalId,
        const QString& operatorId,
        const QString& reason
    );

    // Overlap status queries
    Q_INVOKABLE QVariantMap getOverlapStatus(
        const QString& routeId,
        const QString& signalId
    ) const;

    Q_INVOKABLE QVariantList getActiveOverlaps() const;
    Q_INVOKABLE QVariantList getPendingReleases() const;
    Q_INVOKABLE bool hasActiveOverlap(const QString& circuitId) const;

    // Overlap definitions management
    Q_INVOKABLE QVariantMap getOverlapDefinition(const QString& signalId) const;
    Q_INVOKABLE QVariantList getAllOverlapDefinitions() const;
    Q_INVOKABLE bool updateOverlapDefinition(
        const QString& signalId,
        const QStringList& overlapCircuits,
        const QStringList& releaseTriggers,
        int holdSeconds
    );

    // Release trigger monitoring
    Q_INVOKABLE void checkReleaseTriggers();
    Q_INVOKABLE bool isReleaseTriggerSatisfied(
        const QString& routeId,
        const QString& signalId
    ) const;

    // Statistics and monitoring
    Q_INVOKABLE QVariantMap getOverlapStatistics() const;
    Q_INVOKABLE QVariantList getOverlapHistory(
        const QString& signalId = QString(),
        int limitDays = 7
    ) const;

public slots:
    void initialize();
    void refreshOverlapDefinitions();
    void onTrackCircuitOccupancyChanged(const QString& circuitId, bool isOccupied);
    void onRouteStateChanged(const QString& routeId, const QString& newState);
    void processScheduledReleases();

signals:
    void operationalStateChanged();
    void overlapCountChanged();
    void statisticsChanged();
    
    void overlapReserved(const QString& routeId, const QString& signalId, const QStringList& circuits);
    void overlapActivated(const QString& routeId, const QString& signalId);
    void overlapReleaseStarted(const QString& routeId, const QString& signalId, int remainingSeconds);
    void overlapReleased(const QString& routeId, const QString& signalId);
    void overlapForceReleased(const QString& routeId, const QString& signalId, const QString& reason);
    
    void releaseTriggerDetected(const QString& routeId, const QString& signalId, const QString& triggerCircuit);
    void overlapViolation(const QString& routeId, const QString& signalId, const QString& violationType, const QString& details);

private:
    // Core overlap logic
    OverlapCalculationResult calculateOverlapInternal(const OverlapCalculationRequest& request);
    OverlapCalculationResult calculateFixedOverlap(const QString& signalId);
    OverlapCalculationResult calculateDynamicOverlap(const OverlapCalculationRequest& request);
    OverlapCalculationResult calculateSafetyMarginOverlap(const OverlapCalculationRequest& request);

    // Release trigger logic
    bool checkCircuitSequenceForRelease(const QStringList& triggerCircuits, const QString& routeId) const;
    bool hasTrainPassedTriggerPoint(const QString& routeId, const QString& circuitId) const;
    void updateReleaseTriggerHistory(const QString& routeId, const QString& circuitId, bool isOccupied);

    // Database operations
    bool loadOverlapDefinitionsFromDatabase();
    bool persistOverlapToDatabase(const ActiveOverlap& overlap);
    bool updateOverlapStateInDatabase(const ActiveOverlap& overlap);
    bool removeOverlapFromDatabase(const QString& routeId, const QString& signalId);

    // Overlap validation
    bool validateOverlapRequest(const QString& routeId, const QString& signalId, const QStringList& circuits, QString& error);
    bool areCircuitsAvailableForOverlap(const QStringList& circuits) const;
    bool checkOverlapConflicts(const QStringList& circuits, const QString& excludeRouteId = QString()) const;

    // Timer management
    void scheduleOverlapRelease(const QString& routeId, const QString& signalId, int delaySeconds);
    void cancelScheduledRelease(const QString& routeId, const QString& signalId);

    // Utility methods
    QString overlapStateToString(OverlapState state) const;
    OverlapState stringToOverlapState(const QString& stateStr) const;
    QString overlapTypeToString(OverlapType type) const;
    OverlapType stringToOverlapType(const QString& typeStr) const;
    QVariantMap overlapToVariantMap(const ActiveOverlap& overlap) const;
    QVariantMap overlapDefinitionToVariantMap(const OverlapDefinition& definition) const;

    // Performance monitoring
    void recordOverlapOperation(const QString& operation, double timeMs);
    void updateAverageHoldTime();

private:
    DatabaseManager* m_dbManager;
    bool m_isOperational = false;

    // Overlap data structures
    QHash<QString, OverlapDefinition> m_overlapDefinitions; // signalId -> definition
    QHash<QString, ActiveOverlap> m_activeOverlaps; // routeId:signalId -> overlap
    QHash<QString, QList<ActiveOverlap*>> m_circuitOverlaps; // circuitId -> overlaps using this circuit

    // Release trigger tracking
    QHash<QString, QHash<QString, QList<QPair<QDateTime, bool>>>> m_triggerHistory; // routeId -> circuitId -> [(timestamp, isOccupied)]

    // Timer for scheduled releases
    QTimer* m_releaseTimer;
    
    // Configuration
    static constexpr int RELEASE_TIMER_INTERVAL_MS = 1000; // 1 second
    static constexpr int DEFAULT_OVERLAP_HOLD_SECONDS = 30;
    static constexpr int MAX_OVERLAP_HOLD_SECONDS = 300; // 5 minutes
    static constexpr int TRIGGER_HISTORY_RETENTION_MINUTES = 60;
    static constexpr double SAFETY_MARGIN_MULTIPLIER = 1.5;

    // Performance statistics
    mutable int m_totalOverlapOperations = 0;
    mutable double m_totalOverlapTime = 0.0;
    mutable double m_averageHoldTime = 30.0;
    mutable int m_successfulReleases = 0;
    mutable int m_forceReleases = 0;
    mutable int m_overlapViolations = 0;
};

} // namespace RailFlux::Route
