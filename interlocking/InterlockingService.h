#pragma once
#include <QObject>
#include <QElapsedTimer>
#include <QTimer>
#include <QDateTime>
#include <memory>
#include <deque>
#include <mutex>

class DatabaseManager;
class SignalBranch;
class TrackCircuitBranch;
class PointMachineBranch;
class InterlockingRuleEngine;

class ValidationResult {
    Q_GADGET
    Q_PROPERTY(bool isAllowed READ isAllowed)
    Q_PROPERTY(QString reason READ getReason)
    Q_PROPERTY(QString ruleId READ getRuleId)
    Q_PROPERTY(int severity READ getSeverity)

public:
    enum class Status { ALLOWED, BLOCKED, CONDITIONAL, MANUAL_OVERRIDE };
    enum class Severity { INFO = 0, WARNING = 1, CRITICAL = 2, EMERGENCY = 3 };

private:
    Status m_status = Status::BLOCKED;
    Severity m_severity = Severity::CRITICAL;
    QString m_reason;
    QString m_ruleId;
    QStringList m_affectedEntities;
    QDateTime m_evaluationTime;

public:
    ValidationResult(Status status = Status::BLOCKED, const QString& reason = "Unknown", Severity severity = Severity::CRITICAL);

    // Status checking
    bool isAllowed() const { return m_status == Status::ALLOWED; }
    bool isBlocked() const { return m_status == Status::BLOCKED; }

    // Getters
    QString getReason() const { return m_reason; }
    QString getRuleId() const { return m_ruleId; }
    int getSeverity() const { return static_cast<int>(m_severity); }
    QStringList getAffectedEntities() const { return m_affectedEntities; }

    // Builder pattern
    ValidationResult& setRuleId(const QString& ruleId) { m_ruleId = ruleId; return *this; }
    ValidationResult& addAffectedEntity(const QString& entityId) { m_affectedEntities.append(entityId); return *this; }

    // Factory methods
    static ValidationResult allowed(const QString& reason = "Operation permitted");
    static ValidationResult blocked(const QString& reason, const QString& ruleId = "");

    // QML integration
    Q_INVOKABLE QVariantMap toVariantMap() const;
};

class InterlockingService : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isOperational READ isOperational NOTIFY operationalStateChanged)
    Q_PROPERTY(int activeInterlocks READ getActiveInterlocksCount NOTIFY activeInterlocksChanged)
    Q_PROPERTY(double averageResponseTime READ getAverageResponseTime NOTIFY performanceChanged)

public:
    explicit InterlockingService(DatabaseManager* dbManager, QObject* parent = nullptr);
    ~InterlockingService();

    //   MAIN VALIDATION INTERFACE: Only for operator-initiated actions
    Q_INVOKABLE ValidationResult validateMainSignalOperation(const QString& signalId,
                                                         const QString& currentAspect,
                                                         const QString& requestedAspect,
                                                         const QString& operatorId = "HMI_USER");

    Q_INVOKABLE ValidationResult validateSubsidiarySignalOperation(const QString& signalId,
                                                     const QString& aspectType,
                                                     const QString& currentAspect,
                                                     const QString& requestedAspect,
                                                     const QString& operatorId = "HMI_USER");

    Q_INVOKABLE ValidationResult validatePointMachineOperation(const QString& machineId,
                                                               const QString& currentPosition,
                                                               const QString& requestedPosition,
                                                               const QString& operatorId = "HMI_USER");

    // Add this method to InterlockingService class
    Q_INVOKABLE ValidationResult validatePairedPointMachineOperation(const QString& machineId,
                                                                    const QString& pairedMachineId,
                                                                    const QString& currentPosition,
                                                                    const QString& pairedCurrentPosition,
                                                                    const QString& requestedPosition,
                                                                    const QString& operatorId);

    // === ROUTE ASSIGNMENT VALIDATION ===
    Q_INVOKABLE ValidationResult validateRouteRequest(const QString& sourceSignalId,
                                                       const QString& destSignalId,
                                                       const QString& direction,
                                                       const QStringList& proposedPath,
                                                       const QString& operatorId = "ROUTE_SYSTEM");

    Q_INVOKABLE ValidationResult validateRouteActivation(const QString& routeId,
                                                         const QStringList& assignedCircuits,
                                                         const QStringList& lockedPointMachines,
                                                         const QString& operatorId = "ROUTE_SYSTEM");

    Q_INVOKABLE ValidationResult validateRouteRelease(const QString& routeId,
                                                      const QStringList& assignedCircuits,
                                                      const QString& releaseReason,
                                                      const QString& operatorId = "ROUTE_SYSTEM");

    Q_INVOKABLE ValidationResult validateResourceConflict(const QString& resourceType,
                                                          const QString& resourceId,
                                                          const QString& requestingRouteId,
                                                          const QVariantList& existingLocks);

    //   REMOVED: validateTrackSegmentAssignment - trackSegment occupancy is hardware-driven, no validation needed

    //   SYSTEM MANAGEMENT
    Q_INVOKABLE bool initialize();
    Q_INVOKABLE bool isOperational() const { return m_isOperational; }
    Q_INVOKABLE double getAverageResponseTime() const;
    Q_INVOKABLE int getActiveInterlocksCount() const;

    InterlockingRuleEngine* getRuleEngine() const;

public slots:
    //   REACTIVE INTERLOCKING: Called when hardware detects trackSegment occupancy changes
    void reactToTrackSegmentOccupancyChange(const QString& trackSegmentId, bool wasOccupied, bool isOccupied);

signals:
    //   OPERATIONAL SIGNALS
    void operationBlocked(const QString& entityId, const QString& reason);
    void automaticProtectionActivated(const QString& entityId, const QString& reason);
    void operationalStateChanged(bool isOperational);
    void activeInterlocksChanged(int count);
    void performanceChanged();

    //   SAFETY SIGNALS
    void criticalSafetyViolation(const QString& entityId, const QString& violation);
    void systemFreezeRequired(const QString& trackSegmentId, const QString& reason, const QString& details);

private slots:
    //   FAILURE HANDLING: Internal slot for handling critical failures
    void handleCriticalFailure(const QString& entityId, const QString& reason);
    void handleInterlockingFailure(const QString& trackSegmentId, const QString& failedSignals, const QString& error);

private:
    DatabaseManager* m_dbManager;
    std::unique_ptr<SignalBranch> m_signalBranch;
    std::unique_ptr<TrackCircuitBranch> m_trackSegmentBranch;
    std::unique_ptr<PointMachineBranch> m_pointBranch;

    //   PERFORMANCE MONITORING
    bool m_isOperational = false;
    mutable std::mutex m_performanceMutex;
    std::deque<double> m_responseTimeHistory;
    static constexpr size_t MAX_RESPONSE_HISTORY = 1000;
    static constexpr int TARGET_RESPONSE_TIME_MS = 50;

    //   HELPER METHODS
    void recordResponseTime(double responseTimeMs);
    void logPerformanceWarning(const QString& operation, double responseTimeMs);
};

Q_DECLARE_METATYPE(ValidationResult)
