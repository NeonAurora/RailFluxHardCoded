#pragma once

#include <QObject>
#include <QVariantMap>
#include <QVariantList>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QTimer>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDebug>
#include <QJsonObject>
#include <QJsonDocument>
#include <deque>
#include <memory>

// Forward declarations
class DatabaseManager;

namespace RailFlux::Route {

enum class MetricType {
    PERFORMANCE,
    SAFETY,
    OPERATIONAL,
    SYSTEM
};

enum class AlertLevel {
    INFO,
    WARNING,
    CRITICAL,
    EMERGENCY
};

struct PerformanceMetric {
    QString operation;
    double responseTimeMs;
    QDateTime timestamp;
    bool success;
    QString context;
    QVariantMap metadata;
};

struct SafetyMetric {
    QString eventType;
    AlertLevel severity;
    QString entityId;
    QString resourceId;
    QString description;
    QDateTime timestamp;
    QString operatorId;
    QVariantMap eventData;
    QVariantMap metadata;
};

struct OperationalMetric {
    QString metricName;
    double value;
    QString unit;
    QDateTime timestamp;
    QVariantMap dimensions;
};

struct SystemHealthMetric {
    QString component;
    QString healthStatus;
    double uptime;
    QDateTime lastCheck;
    QVariantMap diagnostics;
};

struct Alert {
    QString alertId;
    AlertLevel level;
    QString title;
    QString message;
    QString source;              // Component that generated the alert
    QDateTime createdAt;
    QDateTime acknowledgedAt;
    QString acknowledgedBy;
    bool isActive = true;
    QVariantMap metadata;
};

class TelemetryService : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isOperational READ isOperational NOTIFY operationalStateChanged)
    Q_PROPERTY(int activeAlerts READ activeAlerts NOTIFY alertCountChanged)
    Q_PROPERTY(double averageResponseTimeMs READ averageResponseTimeMs NOTIFY metricsUpdated)
    Q_PROPERTY(double systemHealthScore READ systemHealthScore NOTIFY healthScoreChanged)
    Q_PROPERTY(bool performanceMonitoringEnabled READ performanceMonitoringEnabled WRITE setPerformanceMonitoringEnabled NOTIFY configurationChanged)

public:
    explicit TelemetryService(DatabaseManager* dbManager, QObject* parent = nullptr);
    ~TelemetryService();

    // Properties
    bool isOperational() const { return m_isOperational; }
    int activeAlerts() const;
    double averageResponseTimeMs() const { return m_averageResponseTime; }
    double systemHealthScore() const { return m_systemHealthScore; }
    bool performanceMonitoringEnabled() const { return m_performanceMonitoringEnabled; }
    void setPerformanceMonitoringEnabled(bool enabled);

    // Performance metrics recording
    Q_INVOKABLE void recordPerformanceMetric(
        const QString& operation,
        double responseTimeMs,
        bool success = true,
        const QString& context = QString(),
        const QVariantMap& metadata = QVariantMap()
    );

    Q_INVOKABLE void recordBatchPerformanceMetrics(const QVariantList& metrics);

    // Safety metrics recording
    Q_INVOKABLE void recordSafetyEvent(
        const QString& eventType,
        const QString& severity,
        const QString& entityId,
        const QString& description,
        const QString& operatorId = "system",
        const QVariantMap& eventData = QVariantMap()
    );

    Q_INVOKABLE void recordSafetyViolation(
        const QString& violationType,
        const QString& entityId,
        const QString& description,
        const QVariantMap& context = QVariantMap()
    );

    // Operational metrics recording
    Q_INVOKABLE void recordOperationalMetric(
        const QString& metricName,
        double value,
        const QString& unit = "count",
        const QVariantMap& dimensions = QVariantMap()
    );

    Q_INVOKABLE void updateResourceUtilization(
        const QString& resourceType,
        int totalResources,
        int usedResources
    );

    // Route event recording
    Q_INVOKABLE void recordRouteEvent(
        const QString& routeId,
        const QString& eventType,
        const QVariantMap& eventData = QVariantMap()
    );

    // System health monitoring
    Q_INVOKABLE void recordSystemHealth(
        const QString& component,
        const QString& healthStatus,
        const QVariantMap& diagnostics = QVariantMap()
    );

    Q_INVOKABLE double calculateSystemHealthScore();

    // Metrics retrieval and analysis
    Q_INVOKABLE QVariantMap getPerformanceStatistics(
        const QString& operation = QString(),
        int timeWindowMinutes = 60
    ) const;

    Q_INVOKABLE QVariantList getSafetyEvents(
        int timeWindowHours = 24,
        const QString& minSeverity = "INFO"
    ) const;

    Q_INVOKABLE QVariantMap getOperationalMetrics(
        int timeWindowMinutes = 60
    ) const;

    Q_INVOKABLE QVariantMap getSystemHealthStatus() const;

    // Alert management
    Q_INVOKABLE QString createAlert(
        const QString& level,
        const QString& title,
        const QString& message,
        const QString& source,
        const QVariantMap& metadata = QVariantMap()
    );

    Q_INVOKABLE bool acknowledgeAlert(
        const QString& alertId,
        const QString& acknowledgedBy
    );

    Q_INVOKABLE QVariantList getActiveAlerts() const;
    Q_INVOKABLE QVariantList getAlertHistory(int limitHours = 24) const;

    // Real-time monitoring
    Q_INVOKABLE QVariantMap getLiveMetrics() const;
    Q_INVOKABLE QVariantList getPerformanceTrends(
        const QString& operation,
        int timeWindowMinutes = 60,
        int intervalMinutes = 5
    ) const;

    // Threshold monitoring
    Q_INVOKABLE void setPerformanceThreshold(
        const QString& operation,
        double thresholdMs
    );

    Q_INVOKABLE void setSafetyViolationThreshold(
        const QString& violationType,
        int maxViolationsPerHour
    );

    // Export and reporting
    Q_INVOKABLE QVariantMap generatePerformanceReport(
        const QDateTime& startTime,
        const QDateTime& endTime
    ) const;

    Q_INVOKABLE QVariantMap generateSafetyReport(
        const QDateTime& startTime,
        const QDateTime& endTime
    ) const;

public slots:
    void initialize();
    void startMonitoring();
    void stopMonitoring();
    void performPeriodicCollection();
    void checkThresholds();
    void cleanupOldMetrics();

signals:
    void operationalStateChanged();
    void alertCountChanged();
    void metricsUpdated();
    void healthScoreChanged();
    void configurationChanged();
    
    void performanceThresholdExceeded(const QString& operation, double responseTimeMs, double thresholdMs);
    void safetyViolationThresholdExceeded(const QString& violationType, int violationCount, int threshold);
    void systemHealthDegraded(const QString& component, const QString& previousStatus, const QString& currentStatus);
    void alertCreated(const QString& alertId, const QString& level, const QString& title);
    void criticalAlertCreated(const QString& alertId, const QString& title, const QString& message);

private:
    // Metric storage management
    void addPerformanceMetric(const PerformanceMetric& metric);
    void addSafetyMetric(const SafetyMetric& metric);
    void addOperationalMetric(const OperationalMetric& metric);
    void updateSystemHealthMetric(const SystemHealthMetric& metric);

    // Database operations
    bool persistMetricsToDatabase();
    bool loadConfigurationFromDatabase();
    bool saveConfigurationToDatabase();

    // Analysis and calculations
    double calculateAverageResponseTime(const QString& operation = QString(), int timeWindowMinutes = 60) const;
    int countSafetyViolations(const QString& violationType, int timeWindowHours = 1) const;
    QVariantMap calculateResourceUtilizationStats() const;
    void updateSystemHealthScore();

    // Threshold checking
    void checkPerformanceThresholds();
    void checkSafetyThresholds();
    void checkSystemHealthThresholds();

    // Alert management
    QString generateAlertId() const;
    Alert createAlertInternal(AlertLevel level, const QString& title, const QString& message, const QString& source, const QVariantMap& metadata);
    void processAlert(const Alert& alert);

    // Utility methods
    static AlertLevel stringToAlertLevel(const QString& levelStr);
    QString alertLevelToString(AlertLevel level) const;
    MetricType stringToMetricType(const QString& typeStr) const;
    QString metricTypeToString(MetricType type) const;
    QVariantMap performanceMetricToVariantMap(const PerformanceMetric& metric) const;
    QVariantMap safetyMetricToVariantMap(const SafetyMetric& metric) const;

    // Data cleanup
    void cleanupPerformanceMetrics();
    void cleanupSafetyMetrics();
    void cleanupOperationalMetrics();
    void cleanupOldAlerts();

private:
    DatabaseManager* m_dbManager;
    bool m_isOperational = false;
    bool m_performanceMonitoringEnabled = true;

    // Metric storage (in-memory with size limits for performance)
    std::deque<PerformanceMetric> m_performanceMetrics;
    std::deque<SafetyMetric> m_safetyMetrics;
    std::deque<OperationalMetric> m_operationalMetrics;
    QHash<QString, SystemHealthMetric> m_systemHealthMetrics; // component -> health

    // Active alerts
    QHash<QString, Alert> m_activeAlerts; // alertId -> alert

    // Performance thresholds
    QHash<QString, double> m_performanceThresholds; // operation -> threshold_ms
    QHash<QString, int> m_safetyViolationThresholds; // violation_type -> max_per_hour

    // Timers
    QTimer* m_collectionTimer;
    QTimer* m_thresholdTimer;
    QTimer* m_cleanupTimer;

    // Cached calculations
    double m_averageResponseTime = 0.0;
    double m_systemHealthScore = 100.0;
    QDateTime m_lastHealthCalculation;

    // Configuration
    static constexpr int COLLECTION_INTERVAL_MS = 30000;      // 30 seconds
    static constexpr int THRESHOLD_CHECK_INTERVAL_MS = 5000;  // 5 seconds
    static constexpr int CLEANUP_INTERVAL_MS = 300000;       // 5 minutes
    static constexpr int MAX_PERFORMANCE_METRICS = 10000;
    static constexpr int MAX_SAFETY_METRICS = 5000;
    static constexpr int MAX_OPERATIONAL_METRICS = 5000;
    static constexpr int METRIC_RETENTION_HOURS = 24;
    static constexpr int ALERT_RETENTION_DAYS = 30;
    static constexpr double DEFAULT_PERFORMANCE_THRESHOLD_MS = 100.0;
    static constexpr int DEFAULT_SAFETY_VIOLATION_THRESHOLD = 5;
    static constexpr double HEALTH_SCORE_DEGRADED_THRESHOLD = 80.0;
    static constexpr double HEALTH_SCORE_CRITICAL_THRESHOLD = 60.0;

    // Statistics tracking
    mutable int m_totalMetricsRecorded = 0;
    mutable int m_totalAlertsCreated = 0;
    mutable int m_thresholdViolations = 0;
};

} // namespace RailFlux::Route
