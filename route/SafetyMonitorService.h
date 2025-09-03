#pragma once

#include <QObject>
#include <QVariantMap>
#include <QVariantList>
#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QTimer>
#include <QElapsedTimer>
#include <QHash>
#include <QQueue>
#include <QDebug>
#include <memory>
#include <chrono>

// Forward declarations
class DatabaseManager;

namespace RailFlux::Route {

// Forward declarations
class TelemetryService;

enum class ComplianceLevel {
    COMPLIANT,
    MINOR_DEVIATION,
    MAJOR_DEVIATION,
    SAFETY_CRITICAL,
    NON_COMPLIANT
};

enum class ViolationType {
    ROUTE_CONFLICT,
    SIGNAL_VIOLATION,
    TRACK_CIRCUIT_VIOLATION,
    POINT_MACHINE_VIOLATION,
    OVERLAP_VIOLATION,
    TIMING_VIOLATION,
    INTERLOCKING_VIOLATION,
    OPERATOR_VIOLATION,
    SYSTEM_INTEGRITY,
    EMERGENCY_PROTOCOL
};

struct SafetyViolation {
    QString id;
    QString routeId;
    ViolationType type;
    ComplianceLevel severity;
    QString description;
    QString affectedResource;
    QStringList affectedRoutes;
    QString operatorId;
    QDateTime detectedAt;
    QDateTime acknowledgedAt;
    QDateTime resolvedAt;
    QDateTime timestamp;
    QString resolution;
    QVariantMap metadata;
    bool isActive = true;
    
    // Utility methods
    QString violationTypeToString() const;
    QString complianceLevelToString() const;
    bool isResolved() const { return !resolvedAt.isNull(); }
    bool isAcknowledged() const { return !acknowledgedAt.isNull(); }
    qint64 durationMs() const {
        if (isResolved()) {
            return detectedAt.msecsTo(resolvedAt);
        }
        return detectedAt.msecsTo(QDateTime::currentDateTime());
    }
};

struct ComplianceReport {
    QString reportId;
    QDateTime generatedAt;
    QDateTime periodStart;
    QDateTime periodEnd;
    ComplianceLevel overallCompliance;
    
    // Statistics
    int totalViolations = 0;
    int activeViolations = 0;
    int resolvedViolations = 0;
    int criticalViolations = 0;
    double averageResolutionTimeMs = 0.0;
    double complianceScore = 100.0; // 0-100 percentage
    
    // Category breakdown
    QHash<ViolationType, int> violationsByType;
    QHash<QString, int> violationsByOperator;
    QHash<QString, int> violationsByResource;
    
    // Performance metrics
    QVariantMap performanceMetrics;
    QStringList recommendations;
};

class SafetyMonitorService : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isOperational READ isOperational NOTIFY operationalStateChanged)
    Q_PROPERTY(int activeViolations READ activeViolations NOTIFY violationCountChanged)
    Q_PROPERTY(ComplianceLevel currentComplianceLevel READ currentComplianceLevel NOTIFY complianceLevelChanged)
    Q_PROPERTY(double complianceScore READ complianceScore NOTIFY complianceScoreChanged)

public:
    explicit SafetyMonitorService(
        DatabaseManager* dbManager,
        TelemetryService* telemetryService = nullptr,
        QObject* parent = nullptr
    );
    ~SafetyMonitorService();

    // Properties
    bool isOperational() const { return m_isOperational; }
    int activeViolations() const;
    ComplianceLevel currentComplianceLevel() const { return m_currentComplianceLevel; }
    double complianceScore() const { return m_currentComplianceScore; }

    // === SAFETY MONITORING ===
    Q_INVOKABLE void startContinuousMonitoring();
    Q_INVOKABLE void stopContinuousMonitoring();
    Q_INVOKABLE bool performSafetyAudit();
    Q_INVOKABLE QVariantMap checkRouteCompliance(const QString& routeId);
    Q_INVOKABLE QVariantMap checkSystemCompliance();

    // === VIOLATION MANAGEMENT ===
    Q_INVOKABLE QString reportViolation(
        const QString& violationType,
        const QString& description,
        const QString& affectedResource,
        const QString& operatorId = "system",
        const QVariantMap& metadata = QVariantMap()
    );

    Q_INVOKABLE bool acknowledgeViolation(
        const QString& violationId,
        const QString& operatorId
    );

    Q_INVOKABLE bool resolveViolation(
        const QString& violationId,
        const QString& resolution,
        const QString& operatorId
    );

    Q_INVOKABLE QVariantMap getViolationDetails(const QString& violationId) const;
    Q_INVOKABLE QVariantList getActiveViolations() const;
    Q_INVOKABLE QVariantList getViolationHistory(int limitHours = 24) const;

    // === COMPLIANCE REPORTING ===
    Q_INVOKABLE QString generateComplianceReport(
        const QDateTime& periodStart,
        const QDateTime& periodEnd
    );

    Q_INVOKABLE QVariantMap getComplianceReport(const QString& reportId) const;
    Q_INVOKABLE QVariantList getComplianceReports(int limitDays = 30) const;
    Q_INVOKABLE QVariantMap getCurrentComplianceStatus() const;

    // === REAL-TIME MONITORING ===
    Q_INVOKABLE void monitorRouteOperation(const QString& routeId);
    Q_INVOKABLE void monitorResourceUsage(const QString& resourceType, const QString& resourceId);
    Q_INVOKABLE void monitorOperatorActions(const QString& operatorId);

    // === ALERTING AND NOTIFICATIONS ===
    Q_INVOKABLE bool setAlertThreshold(const QString& metricType, double threshold);
    Q_INVOKABLE QVariantMap getAlertConfiguration() const;
    Q_INVOKABLE QVariantList getPendingAlerts() const;

    // === ADDITIONAL METHODS FOR MAIN.CPP COMPATIBILITY ===
    Q_INVOKABLE void recordSafetyViolation(const QString& routeId, const QString& reason, const QString& severity);
    Q_INVOKABLE void recordEmergencyEvent(const QString& eventType, const QString& reason);
    Q_INVOKABLE void recordPerformanceWarning(const QString& warningType, const QVariantMap& details);

    // === INTEGRATION METHODS ===
    void recordRouteEvent(const QString& routeId, const QString& eventType, const QVariantMap& data);
    void recordOperatorAction(const QString& operatorId, const QString& action, const QVariantMap& data);
    void recordSystemEvent(const QString& eventType, const QVariantMap& data);

public slots:
    void initialize();
    void performPeriodicSafetyCheck();
    void onRouteStateChanged(const QString& routeId, const QString& newState);
    void onTrackCircuitOccupancyChanged(const QString& circuitId, bool isOccupied);
    void onSignalAspectChanged(const QString& signalId, const QString& aspect);
    void onPointMachinePositionChanged(const QString& machineId, const QString& position);
    void onEmergencyActivated(const QString& reason);
    void onSystemOverload();

signals:
    void operationalStateChanged();
    void violationCountChanged();
    void complianceLevelChanged();
    void complianceScoreChanged();
    
    void violationDetected(const QString& violationId, const QString& violationType, const QString& severity);
    void violationResolved(const QString& violationId, const QString& resolution);
    void criticalViolationDetected(const QString& violationId, const QString& description);
    
    void complianceThresholdBreached(const QString& metricType, double currentValue, double threshold);
    void complianceLevelDowngraded(const QString& previousLevel, const QString& newLevel);
    void safetyAuditCompleted(const QString& auditId, const QString& overallResult);
    void emergencyShutdownRequired(const QString& routeId, const QString& reason);

private:
    // Core monitoring functions
    void checkRouteConflicts();
    void checkSignalCompliance();
    void checkTrackCircuitCompliance();
    void checkPointMachineCompliance();
    void checkOverlapCompliance();
    void checkTimingCompliance();
    void checkInterlockingCompliance();
    void checkOperatorCompliance();
    void checkSystemIntegrity();

    // Violation detection algorithms
    QList<SafetyViolation> detectRouteConflicts();
    QList<SafetyViolation> detectSignalViolations();
    QList<SafetyViolation> detectTrackCircuitViolations();
    QList<SafetyViolation> detectPointMachineViolations();
    QList<SafetyViolation> detectOverlapViolations();
    QList<SafetyViolation> detectTimingViolations();
    QList<SafetyViolation> detectInterlockingViolations();
    QList<SafetyViolation> detectOperatorViolations();
    QList<SafetyViolation> detectSystemIntegrityViolations();

    // Analysis and scoring
    ComplianceLevel calculateOverallCompliance() const;
    double calculateComplianceScore() const;
    QStringList generateRecommendations() const;
    bool isViolationCritical(const SafetyViolation& violation) const;

    // Report generation
    ComplianceReport generateComplianceReportInternal(
        const QDateTime& periodStart,
        const QDateTime& periodEnd
    );
    
    bool saveComplianceReport(const ComplianceReport& report);
    QVariantMap complianceReportToVariantMap(const ComplianceReport& report) const;

    // Alerting
    void processAlerts();
    void sendAlert(const QString& alertType, const QVariantMap& alertData);
    bool shouldSendAlert(const QString& metricType, double value) const;

    // Database integration
    bool persistViolation(const SafetyViolation& violation);
    bool updateViolationInDatabase(const SafetyViolation& violation);
    bool loadActiveViolationsFromDatabase();
    bool loadComplianceConfiguration();

    // Performance monitoring
    void recordMonitoringMetrics(const QString& checkType, double durationMs, int violationsFound);
    void updatePerformanceStatistics();

    // Utility methods
    QString generateViolationId() const;
    QString generateReportId() const;
    ViolationType stringToViolationType(const QString& typeStr) const;
    QString violationTypeToString(ViolationType type) const;
    ComplianceLevel stringToComplianceLevel(const QString& levelStr) const;
    QString complianceLevelToString(ComplianceLevel level) const;
    QVariantMap violationToVariantMap(const SafetyViolation& violation) const;
    SafetyViolation variantMapToViolation(const QVariantMap& map) const;

    // Configuration methods
    void loadDefaultAlertThresholds();
    void applyAlertConfiguration(const QVariantMap& config);

private:
    // Service dependencies
    DatabaseManager* m_dbManager;
    TelemetryService* m_telemetryService;

    // Operational state
    bool m_isOperational = false;
    bool m_continuousMonitoring = false;
    ComplianceLevel m_currentComplianceLevel = ComplianceLevel::COMPLIANT;
    double m_currentComplianceScore = 100.0;

    // Violation storage
    QHash<QString, SafetyViolation> m_activeViolations; // violationId -> violation
    QQueue<SafetyViolation> m_recentViolations; // For performance tracking

    // Compliance reports
    QHash<QString, ComplianceReport> m_complianceReports; // reportId -> report

    // Monitoring timers
    QTimer* m_monitoringTimer;
    QTimer* m_alertTimer;

    // Alert configuration
    QHash<QString, double> m_alertThresholds; // metricType -> threshold
    QDateTime m_lastAlertSent;

    // Performance tracking
    QList<double> m_monitoringTimes;
    double m_averageMonitoringTime = 0.0;
    QDateTime m_lastPerformanceUpdate;

    // Configuration
    int m_monitoringIntervalMs = 5000;  // 5 seconds
    int m_alertCheckIntervalMs = 10000; // 10 seconds
    int m_maxActiveViolations = 100;
    int m_violationHistoryDays = 30;
    int m_reportRetentionDays = 90;

    // Performance thresholds
    static constexpr double TARGET_MONITORING_TIME_MS = 100.0;   // 100ms
    static constexpr double WARNING_MONITORING_TIME_MS = 500.0;  // 500ms
    static constexpr int PERFORMANCE_HISTORY_SIZE = 100;
    static constexpr int MAX_RECENT_VIOLATIONS = 50;
    static constexpr double CRITICAL_COMPLIANCE_THRESHOLD = 80.0; // Below 80% = critical
    static constexpr double WARNING_COMPLIANCE_THRESHOLD = 90.0;  // Below 90% = warning

    // Statistics
    mutable int m_totalChecks = 0;
    mutable int m_violationsDetected = 0;
    mutable int m_violationsResolved = 0;
    mutable int m_criticalViolations = 0;
    mutable int m_alertsSent = 0;
};

} // namespace RailFlux::Route
