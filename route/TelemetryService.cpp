#include "TelemetryService.h"
#include "../database/DatabaseManager.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUuid>
#include <QtMath>
#include <algorithm>

namespace RailFlux::Route {

TelemetryService::TelemetryService(DatabaseManager* dbManager, QObject* parent)
    : QObject(parent)
    , m_dbManager(dbManager)
    , m_collectionTimer(new QTimer(this))
    , m_thresholdTimer(new QTimer(this))
    , m_cleanupTimer(new QTimer(this))
{
    if (!m_dbManager) {
        qCritical() << "[TelemetryService > ctor] DatabaseManager is null";
        return;
    }

    // Setup timers
    m_collectionTimer->setInterval(COLLECTION_INTERVAL_MS);
    m_thresholdTimer->setInterval(THRESHOLD_CHECK_INTERVAL_MS);
    m_cleanupTimer->setInterval(CLEANUP_INTERVAL_MS);

    connect(m_collectionTimer, &QTimer::timeout, this, &TelemetryService::performPeriodicCollection);
    connect(m_thresholdTimer, &QTimer::timeout, this, &TelemetryService::checkThresholds);
    connect(m_cleanupTimer, &QTimer::timeout, this, &TelemetryService::cleanupOldMetrics);

    // Connect to database connection changes
    connect(m_dbManager, &DatabaseManager::connectionStateChanged,
            this, [this](bool connected) {
                if (connected) {
                    initialize();
                } else {
                    stopMonitoring();
                    m_isOperational = false;
                    emit operationalStateChanged();
                }
            });
}

TelemetryService::~TelemetryService() {
    stopMonitoring();
}

void TelemetryService::initialize() {
    if (!m_dbManager || !m_dbManager->isConnected()) {
        qWarning() << "[TelemetryService > initialize] Cannot initialize - database not connected";
        return;
    }

    try {
        // Load configuration from database
        if (loadConfigurationFromDatabase()) {
            // Set default thresholds if not configured
            if (m_performanceThresholds.isEmpty()) {
                m_performanceThresholds["route_assignment"] = 50.0;
                m_performanceThresholds["pathfinding"] = 100.0;
                m_performanceThresholds["overlap_calculation"] = 25.0;
                m_performanceThresholds["resource_locking"] = 20.0;
            }

            if (m_safetyViolationThresholds.isEmpty()) {
                m_safetyViolationThresholds["interlocking_violation"] = 2;
                m_safetyViolationThresholds["resource_conflict"] = 5;
                m_safetyViolationThresholds["overlap_violation"] = 3;
            }

            m_isOperational = true;
            startMonitoring();

            emit operationalStateChanged();
        } else {
            qCritical() << "[TelemetryService > initialize] Failed to load configuration";
        }

    } catch (const std::exception& e) {
        qCritical() << "[TelemetryService > initialize] Initialization failed:" << e.what();
        m_isOperational = false;
        emit operationalStateChanged();
    }
}

void TelemetryService::startMonitoring() {
    if (!m_isOperational) {
        return;
    }

    m_collectionTimer->start();
    m_thresholdTimer->start();
    m_cleanupTimer->start();

    // Record system startup
    recordSafetyEvent("system_startup", "INFO", "TelemetryService",
                      "Telemetry monitoring started", "system");
}

void TelemetryService::stopMonitoring() {
    m_collectionTimer->stop();
    m_thresholdTimer->stop();
    m_cleanupTimer->stop();
}

void TelemetryService::recordPerformanceMetric(
    const QString& operation,
    double responseTimeMs,
    bool success,
    const QString& context,
    const QVariantMap& metadata
) {
    if (!m_isOperational || !m_performanceMonitoringEnabled) {
        return;
    }

    PerformanceMetric metric;
    metric.operation = operation;
    metric.responseTimeMs = responseTimeMs;
    metric.timestamp = QDateTime::currentDateTime();
    metric.success = success;
    metric.context = context;
    metric.metadata = metadata;

    addPerformanceMetric(metric);
    m_totalMetricsRecorded++;

    // Check thresholds immediately for performance metrics
    if (m_performanceThresholds.contains(operation)) {
        double threshold = m_performanceThresholds[operation];
        if (responseTimeMs > threshold) {
            emit performanceThresholdExceeded(operation, responseTimeMs, threshold);

            // Create alert for significant threshold violations
            if (responseTimeMs > threshold * 2.0) {
                createAlert("WARNING",
                            QString("Performance Threshold Exceeded"),
                            QString("%1 took %2ms (threshold: %3ms)")
                                .arg(operation)
                                .arg(responseTimeMs, 0, 'f', 1)
                                .arg(threshold, 0, 'f', 1),
                            "TelemetryService",
                            QVariantMap{{"operation", operation}, {"responseTime", responseTimeMs}, {"threshold", threshold}});
            }
        }
    }

    // Update cached average response time
    calculateAverageResponseTime();
    emit metricsUpdated();
}

void TelemetryService::addPerformanceMetric(const PerformanceMetric& metric) {
    m_performanceMetrics.push_back(metric);

    // Maintain size limit
    if (m_performanceMetrics.size() > MAX_PERFORMANCE_METRICS) {
        m_performanceMetrics.pop_front();
    }
}

void TelemetryService::recordSafetyEvent(
    const QString& eventType,
    const QString& severity,
    const QString& entityId,
    const QString& description,
    const QString& operatorId,
    const QVariantMap& eventData
) {
    if (!m_isOperational) {
        return;
    }

    SafetyMetric metric;
    metric.eventType = eventType;
    metric.severity = stringToAlertLevel(severity);
    metric.entityId = entityId;
    metric.description = description;
    metric.timestamp = QDateTime::currentDateTime();
    metric.operatorId = operatorId;
    metric.eventData = eventData;

    addSafetyMetric(metric);
    m_totalMetricsRecorded++;

    // Create alert for WARNING and above safety events
    if (metric.severity >= AlertLevel::WARNING) {
        QString alertLevel = (metric.severity >= AlertLevel::CRITICAL) ? "CRITICAL" : "WARNING";
        createAlert(alertLevel,
                    QString("Safety Event: %1").arg(eventType),
                    QString("%1: %2").arg(entityId, description),
                    "SafetyMonitor",
                    QVariantMap{{"eventType", eventType}, {"entityId", entityId}, {"operatorId", operatorId}});
    }

    // Check safety violation thresholds
    if (eventType.contains("violation") && m_safetyViolationThresholds.contains(eventType)) {
        int violationCount = countSafetyViolations(eventType, 1); // Last hour
        int threshold = m_safetyViolationThresholds[eventType];

        if (violationCount >= threshold) {
            emit safetyViolationThresholdExceeded(eventType, violationCount, threshold);

            createAlert("CRITICAL",
                        QString("Safety Violation Threshold Exceeded"),
                        QString("%1 violations in last hour: %2 (threshold: %3)")
                            .arg(eventType)
                            .arg(violationCount)
                            .arg(threshold),
                        "TelemetryService",
                        QVariantMap{{"violationType", eventType}, {"count", violationCount}, {"threshold", threshold}});
        }
    }
}

void TelemetryService::addSafetyMetric(const SafetyMetric& metric) {
    m_safetyMetrics.push_back(metric);

    // Maintain size limit
    if (m_safetyMetrics.size() > MAX_SAFETY_METRICS) {
        m_safetyMetrics.pop_front();
    }
}

void TelemetryService::recordSafetyViolation(
    const QString& violationType,
    const QString& entityId,
    const QString& description,
    const QVariantMap& context
) {
    recordSafetyEvent(violationType, "WARNING", entityId, description, "system", context);
}

void TelemetryService::recordOperationalMetric(
    const QString& metricName,
    double value,
    const QString& unit,
    const QVariantMap& dimensions
) {
    if (!m_isOperational) {
        return;
    }

    OperationalMetric metric;
    metric.metricName = metricName;
    metric.value = value;
    metric.unit = unit;
    metric.timestamp = QDateTime::currentDateTime();
    metric.dimensions = dimensions;

    addOperationalMetric(metric);
    m_totalMetricsRecorded++;
}

void TelemetryService::addOperationalMetric(const OperationalMetric& metric) {
    m_operationalMetrics.push_back(metric);

    // Maintain size limit
    if (m_operationalMetrics.size() > MAX_OPERATIONAL_METRICS) {
        m_operationalMetrics.pop_front();
    }
}

void TelemetryService::updateResourceUtilization(
    const QString& resourceType,
    int totalResources,
    int usedResources
) {
    if (totalResources <= 0) {
        return;
    }

    double utilizationPercentage = (double)usedResources / totalResources * 100.0;

    recordOperationalMetric(
        QString("%1_utilization").arg(resourceType.toLower()),
        utilizationPercentage,
        "percentage",
        QVariantMap{{"resourceType", resourceType}, {"total", totalResources}, {"used", usedResources}}
    );

    // Alert on high utilization
    if (utilizationPercentage > 90.0) {
        createAlert("WARNING",
                    QString("High Resource Utilization"),
                    QString("%1 utilization: %2% (%3/%4)")
                        .arg(resourceType)
                        .arg(utilizationPercentage, 0, 'f', 1)
                        .arg(usedResources)
                        .arg(totalResources),
                    "ResourceMonitor",
                    QVariantMap{{"resourceType", resourceType}, {"utilization", utilizationPercentage}});
    }
}

void TelemetryService::recordSystemHealth(
    const QString& component,
    const QString& healthStatus,
    const QVariantMap& diagnostics
) {
    if (!m_isOperational) {
        return;
    }

    QString previousStatus = "unknown";
    if (m_systemHealthMetrics.contains(component)) {
        previousStatus = m_systemHealthMetrics[component].healthStatus;
    }

    SystemHealthMetric metric;
    metric.component = component;
    metric.healthStatus = healthStatus;
    metric.uptime = diagnostics.value("uptime", 100.0).toDouble();
    metric.lastCheck = QDateTime::currentDateTime();
    metric.diagnostics = diagnostics;

    updateSystemHealthMetric(metric);

    // Emit signal if health status changed
    if (previousStatus != healthStatus && previousStatus != "unknown") {
        emit systemHealthDegraded(component, previousStatus, healthStatus);

        if (healthStatus == "degraded" || healthStatus == "critical") {
            QString alertLevel = (healthStatus == "critical") ? "CRITICAL" : "WARNING";
            createAlert(alertLevel,
                        QString("System Health Alert"),
                        QString("%1 status changed from %2 to %3")
                            .arg(component, previousStatus, healthStatus),
                        "HealthMonitor",
                        QVariantMap{{"component", component}, {"previousStatus", previousStatus}, {"currentStatus", healthStatus}});
        }
    }

    // Recalculate overall system health score
    updateSystemHealthScore();
}

void TelemetryService::updateSystemHealthMetric(const SystemHealthMetric& metric) {
    m_systemHealthMetrics[metric.component] = metric;
}

double TelemetryService::calculateSystemHealthScore() {
    if (m_systemHealthMetrics.isEmpty()) {
        return 100.0;
    }

    double totalScore = 0.0;
    int componentCount = 0;

    for (const SystemHealthMetric& metric : m_systemHealthMetrics) {
        double componentScore = 100.0; // Default healthy score

        if (metric.healthStatus == "degraded") {
            componentScore = 75.0;
        } else if (metric.healthStatus == "critical") {
            componentScore = 25.0;
        } else if (metric.healthStatus == "failed") {
            componentScore = 0.0;
        }

        // Factor in uptime
        componentScore *= (metric.uptime / 100.0);

        totalScore += componentScore;
        componentCount++;
    }

    double newScore = totalScore / componentCount;

    // Update cached score and emit signal if changed significantly
    if (qAbs(newScore - m_systemHealthScore) > 5.0) {
        m_systemHealthScore = newScore;
        emit healthScoreChanged();

        // Alert on significant health degradation
        if (m_systemHealthScore < HEALTH_SCORE_CRITICAL_THRESHOLD) {
            createAlert("CRITICAL",
                        "System Health Critical",
                        QString("Overall system health score: %1%").arg(m_systemHealthScore, 0, 'f', 1),
                        "HealthMonitor");
        } else if (m_systemHealthScore < HEALTH_SCORE_DEGRADED_THRESHOLD) {
            createAlert("WARNING",
                        "System Health Degraded",
                        QString("Overall system health score: %1%").arg(m_systemHealthScore, 0, 'f', 1),
                        "HealthMonitor");
        }
    }

    m_lastHealthCalculation = QDateTime::currentDateTime();
    return m_systemHealthScore;
}

void TelemetryService::updateSystemHealthScore() {
    calculateSystemHealthScore();
}

QString TelemetryService::createAlert(
    const QString& level,
    const QString& title,
    const QString& message,
    const QString& source,
    const QVariantMap& metadata
) {
    AlertLevel alertLevel = stringToAlertLevel(level);
    Alert alert = createAlertInternal(alertLevel, title, message, source, metadata);

    processAlert(alert);

    return alert.alertId;
}

Alert TelemetryService::createAlertInternal(
    AlertLevel level,
    const QString& title,
    const QString& message,
    const QString& source,
    const QVariantMap& metadata
) {
    Alert alert;
    alert.alertId = generateAlertId();
    alert.level = level;
    alert.title = title;
    alert.message = message;
    alert.source = source;
    alert.createdAt = QDateTime::currentDateTime();
    alert.metadata = metadata;
    alert.isActive = true;

    return alert;
}

void TelemetryService::processAlert(const Alert& alert) {
    m_activeAlerts[alert.alertId] = alert;
    m_totalAlertsCreated++;

    emit alertCreated(alert.alertId, alertLevelToString(alert.level), alert.title);
    emit alertCountChanged();

    // Only critical and warning level logs
    if (alert.level >= AlertLevel::CRITICAL) {
        qCritical() << "[TelemetryService > processAlert] CRITICAL ALERT:" << alert.title << "-" << alert.message;
        emit criticalAlertCreated(alert.alertId, alert.title, alert.message);
    } else if (alert.level == AlertLevel::WARNING) {
        qWarning() << "[TelemetryService > processAlert] WARNING ALERT:" << alert.title << "-" << alert.message;
    }
}

QString TelemetryService::generateAlertId() const {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool TelemetryService::acknowledgeAlert(const QString& alertId, const QString& acknowledgedBy) {
    if (!m_activeAlerts.contains(alertId)) {
        return false;
    }

    Alert& alert = m_activeAlerts[alertId];
    alert.acknowledgedAt = QDateTime::currentDateTime();
    alert.acknowledgedBy = acknowledgedBy;
    alert.isActive = false;

    emit alertCountChanged();

    return true;
}

int TelemetryService::activeAlerts() const {
    int count = 0;
    for (const Alert& alert : m_activeAlerts) {
        if (alert.isActive) {
            count++;
        }
    }
    return count;
}

QVariantList TelemetryService::getActiveAlerts() const {
    QVariantList alerts;

    for (const Alert& alert : m_activeAlerts) {
        if (alert.isActive) {
            QVariantMap alertMap;
            alertMap["alertId"] = alert.alertId;
            alertMap["level"] = alertLevelToString(alert.level);
            alertMap["title"] = alert.title;
            alertMap["message"] = alert.message;
            alertMap["source"] = alert.source;
            alertMap["createdAt"] = alert.createdAt;
            alertMap["metadata"] = alert.metadata;
            alerts.append(alertMap);
        }
    }

    // Sort by severity and creation time
    std::sort(alerts.begin(), alerts.end(), [](const QVariant& a, const QVariant& b) {
        QVariantMap mapA = a.toMap();
        QVariantMap mapB = b.toMap();

        // Critical alerts first
        AlertLevel levelA = TelemetryService::stringToAlertLevel(mapA["level"].toString());
        AlertLevel levelB = TelemetryService::stringToAlertLevel(mapB["level"].toString());

        if (levelA != levelB) {
            return levelA > levelB; // Higher severity first
        }

        // Then by creation time (newest first)
        return mapA["createdAt"].toDateTime() > mapB["createdAt"].toDateTime();
    });

    return alerts;
}

double TelemetryService::calculateAverageResponseTime(const QString& operation, int timeWindowMinutes) const {
    QDateTime cutoff = QDateTime::currentDateTime().addSecs(-timeWindowMinutes * 60);

    double totalTime = 0.0;
    int count = 0;

    for (const PerformanceMetric& metric : m_performanceMetrics) {
        if (metric.timestamp < cutoff) {
            continue; // Outside time window
        }

        if (!operation.isEmpty() && metric.operation != operation) {
            continue; // Different operation
        }

        totalTime += metric.responseTimeMs;
        count++;
    }

    if (count == 0) {
        return 0.0;
    }

    double average = totalTime / count;

    // Update cached value if calculating overall average
    if (operation.isEmpty()) {
        const_cast<TelemetryService*>(this)->m_averageResponseTime = average;
    }

    return average;
}

int TelemetryService::countSafetyViolations(const QString& violationType, int timeWindowHours) const {
    QDateTime cutoff = QDateTime::currentDateTime().addSecs(-timeWindowHours * 3600);

    int count = 0;
    for (const SafetyMetric& metric : m_safetyMetrics) {
        if (metric.timestamp >= cutoff && metric.eventType == violationType) {
            count++;
        }
    }

    return count;
}

QVariantMap TelemetryService::getPerformanceStatistics(const QString& operation, int timeWindowMinutes) const {
    QDateTime cutoff = QDateTime::currentDateTime().addSecs(-timeWindowMinutes * 60);

    QList<double> responseTimes;
    int successCount = 0;
    int totalCount = 0;

    for (const PerformanceMetric& metric : m_performanceMetrics) {
        if (metric.timestamp < cutoff) {
            continue;
        }

        if (!operation.isEmpty() && metric.operation != operation) {
            continue;
        }

        responseTimes.append(metric.responseTimeMs);
        if (metric.success) {
            successCount++;
        }
        totalCount++;
    }

    if (responseTimes.isEmpty()) {
        return QVariantMap{
            {"operation", operation},
            {"timeWindowMinutes", timeWindowMinutes},
            {"count", 0},
            {"averageMs", 0.0},
            {"minMs", 0.0},
            {"maxMs", 0.0},
            {"successRate", 0.0}
        };
    }

    std::sort(responseTimes.begin(), responseTimes.end());

    double sum = std::accumulate(responseTimes.begin(), responseTimes.end(), 0.0);
    double average = sum / responseTimes.size();
    double min = responseTimes.first();
    double max = responseTimes.last();
    double p95 = responseTimes[qRound(responseTimes.size() * 0.95) - 1];
    double successRate = totalCount > 0 ? (double)successCount / totalCount * 100.0 : 0.0;

    return QVariantMap{
        {"operation", operation},
        {"timeWindowMinutes", timeWindowMinutes},
        {"count", totalCount},
        {"averageMs", average},
        {"minMs", min},
        {"maxMs", max},
        {"p95Ms", p95},
        {"successRate", successRate}
    };
}

QVariantMap TelemetryService::getLiveMetrics() const {
    return QVariantMap{
        {"timestamp", QDateTime::currentDateTime()},
        {"systemHealthScore", m_systemHealthScore},
        {"averageResponseTimeMs", m_averageResponseTime},
        {"activeAlerts", activeAlerts()},
        {"totalMetricsRecorded", m_totalMetricsRecorded},
        {"performanceMetricsCount", (int)m_performanceMetrics.size()},
        {"safetyMetricsCount", (int)m_safetyMetrics.size()},
        {"operationalMetricsCount", (int)m_operationalMetrics.size()},
        {"systemComponents", m_systemHealthMetrics.size()}
    };
}

void TelemetryService::performPeriodicCollection() {
    if (!m_isOperational) {
        return;
    }

    // Update system health metrics
    calculateSystemHealthScore();

    // Record operational metrics
    recordOperationalMetric("active_alerts", activeAlerts(), "count");
    recordOperationalMetric("metrics_recorded_total", m_totalMetricsRecorded, "count");
    recordOperationalMetric("system_health_score", m_systemHealthScore, "percentage");
}

void TelemetryService::checkThresholds() {
    if (!m_isOperational) {
        return;
    }

    checkPerformanceThresholds();
    checkSafetyThresholds();
    checkSystemHealthThresholds();
}

void TelemetryService::checkPerformanceThresholds() {
    // Check recent performance metrics for threshold violations
    QDateTime cutoff = QDateTime::currentDateTime().addSecs(-60); // Last minute

    for (const PerformanceMetric& metric : m_performanceMetrics) {
        if (metric.timestamp < cutoff) {
            continue;
        }

        if (m_performanceThresholds.contains(metric.operation)) {
            double threshold = m_performanceThresholds[metric.operation];
            if (metric.responseTimeMs > threshold) {
                // Threshold violation already handled in recordPerformanceMetric
            }
        }
    }
}

void TelemetryService::checkSafetyThresholds() {
    // Check safety violation rates
    for (auto it = m_safetyViolationThresholds.begin(); it != m_safetyViolationThresholds.end(); ++it) {
        int count = countSafetyViolations(it.key(), 1); // Last hour
        if (count >= it.value()) {
            // Threshold violation already handled in recordSafetyEvent
        }
    }
}

void TelemetryService::checkSystemHealthThresholds() {
    // Check if any component health has degraded
    for (const SystemHealthMetric& metric : m_systemHealthMetrics) {
        if (metric.healthStatus == "critical" || metric.healthStatus == "failed") {
            // Component health issues already handled in recordSystemHealth
        }
    }
}

void TelemetryService::cleanupOldMetrics() {
    cleanupPerformanceMetrics();
    cleanupSafetyMetrics();
    cleanupOperationalMetrics();
    cleanupOldAlerts();
}

void TelemetryService::cleanupPerformanceMetrics() {
    QDateTime cutoff = QDateTime::currentDateTime().addSecs(-METRIC_RETENTION_HOURS * 3600);

    while (!m_performanceMetrics.empty() && m_performanceMetrics.front().timestamp < cutoff) {
        m_performanceMetrics.pop_front();
    }
}

void TelemetryService::cleanupSafetyMetrics() {
    QDateTime cutoff = QDateTime::currentDateTime().addSecs(-METRIC_RETENTION_HOURS * 3600);

    while (!m_safetyMetrics.empty() && m_safetyMetrics.front().timestamp < cutoff) {
        m_safetyMetrics.pop_front();
    }
}

void TelemetryService::cleanupOperationalMetrics() {
    QDateTime cutoff = QDateTime::currentDateTime().addSecs(-METRIC_RETENTION_HOURS * 3600);

    while (!m_operationalMetrics.empty() && m_operationalMetrics.front().timestamp < cutoff) {
        m_operationalMetrics.pop_front();
    }
}

void TelemetryService::cleanupOldAlerts() {
    QDateTime cutoff = QDateTime::currentDateTime().addDays(-ALERT_RETENTION_DAYS);

    QStringList toRemove;
    for (auto it = m_activeAlerts.begin(); it != m_activeAlerts.end(); ++it) {
        if (!it.value().isActive && it.value().createdAt < cutoff) {
            toRemove.append(it.key());
        }
    }

    for (const QString& alertId : toRemove) {
        m_activeAlerts.remove(alertId);
    }
}

void TelemetryService::recordRouteEvent(const QString& routeId, const QString& eventType, const QVariantMap& eventData) {
    recordSafetyEvent(
        eventType,
        "INFO",
        routeId,
        QString("Route event: %1").arg(eventType),
        "system",
        eventData
    );
}

// Utility method implementations
AlertLevel TelemetryService::stringToAlertLevel(const QString& levelStr) {
    if (levelStr.toUpper() == "CRITICAL") return AlertLevel::CRITICAL;
    if (levelStr.toUpper() == "WARNING") return AlertLevel::WARNING;
    if (levelStr.toUpper() == "EMERGENCY") return AlertLevel::EMERGENCY;
    return AlertLevel::INFO;
}

QString TelemetryService::alertLevelToString(AlertLevel level) const {
    switch (level) {
        case AlertLevel::EMERGENCY: return "EMERGENCY";
        case AlertLevel::CRITICAL: return "CRITICAL";
        case AlertLevel::WARNING: return "WARNING";
        case AlertLevel::INFO: return "INFO";
        default: return "INFO";
    }
}

void TelemetryService::setPerformanceMonitoringEnabled(bool enabled) {
    if (m_performanceMonitoringEnabled != enabled) {
        m_performanceMonitoringEnabled = enabled;
        emit configurationChanged();
    }
}

// Stub implementations for database operations
bool TelemetryService::loadConfigurationFromDatabase() {
    // Placeholder - would load thresholds and settings from database
    return true;
}

bool TelemetryService::saveConfigurationToDatabase() {
    // Placeholder - would save current configuration
    return true;
}

bool TelemetryService::persistMetricsToDatabase() {
    // Placeholder - would batch persist metrics to database
    return true;
}

// Additional stub implementations
void TelemetryService::setPerformanceThreshold(const QString& operation, double thresholdMs) {
    m_performanceThresholds[operation] = thresholdMs;
    emit configurationChanged();
}

void TelemetryService::setSafetyViolationThreshold(const QString& violationType, int maxViolationsPerHour) {
    m_safetyViolationThresholds[violationType] = maxViolationsPerHour;
    emit configurationChanged();
}

void TelemetryService::recordBatchPerformanceMetrics(const QVariantList& metrics) {
    for (const QVariant& metricVar : metrics) {
        QVariantMap metricMap = metricVar.toMap();
        QString operation = metricMap["operation"].toString();
        double responseTime = metricMap["responseTime"].toDouble();
        bool success = metricMap["success"].toBool();
        QString operatorId = metricMap.value("operatorId", "system").toString();

        recordPerformanceMetric(operation, responseTime, success, operatorId);
    }
}

QVariantList TelemetryService::getSafetyEvents(int limitHours, const QString& severity) const {
    QVariantList result;
    QDateTime cutoff = QDateTime::currentDateTime().addSecs(-limitHours * 3600);

    for (const SafetyMetric& metric : m_safetyMetrics) {
        if (metric.timestamp >= cutoff) {
            if (severity.isEmpty() || alertLevelToString(metric.severity) == severity) {
                QVariantMap eventMap;
                eventMap["timestamp"] = metric.timestamp;
                eventMap["eventType"] = metric.eventType;
                eventMap["severity"] = alertLevelToString(metric.severity);
                eventMap["resourceId"] = metric.resourceId;
                eventMap["description"] = metric.description;
                eventMap["operatorId"] = metric.operatorId;
                eventMap["metadata"] = metric.metadata;
                result.append(eventMap);
            }
        }
    }

    return result;
}

QVariantMap TelemetryService::getOperationalMetrics(int limitHours) const {
    QVariantMap result;
    QVariantList metrics;
    QDateTime cutoff = QDateTime::currentDateTime().addSecs(-limitHours * 3600);

    for (const OperationalMetric& metric : m_operationalMetrics) {
        if (metric.timestamp >= cutoff) {
            QVariantMap metricMap;
            metricMap["timestamp"] = metric.timestamp;
            metricMap["metricName"] = metric.metricName;
            metricMap["value"] = metric.value;
            metricMap["unit"] = metric.unit;
            metricMap["dimensions"] = metric.dimensions;
            metrics.append(metricMap);
        }
    }

    result["metrics"] = metrics;
    result["timeWindow"] = limitHours;
    result["totalCount"] = metrics.size();

    return result;
}

QVariantMap TelemetryService::getSystemHealthStatus() const {
    QVariantMap result;
    result["systemHealthScore"] = m_systemHealthScore;
    result["isOperational"] = m_isOperational;
    result["activeAlerts"] = activeAlerts();
    result["averageResponseTime"] = m_averageResponseTime;
    result["totalMetricsRecorded"] = m_totalMetricsRecorded;
    result["lastUpdate"] = QDateTime::currentDateTime();

    // Convert system health metrics to QVariantMap
    QVariantMap componentStatuses;
    for (auto it = m_systemHealthMetrics.constBegin(); it != m_systemHealthMetrics.constEnd(); ++it) {
        QVariantMap componentStatus;
        componentStatus["component"] = it.value().component;
        componentStatus["healthStatus"] = it.value().healthStatus;
        componentStatus["uptime"] = it.value().uptime;
        componentStatus["lastCheck"] = it.value().lastCheck;
        componentStatus["diagnostics"] = it.value().diagnostics;
        componentStatuses[it.key()] = componentStatus;
    }
    result["componentStatuses"] = componentStatuses;

    return result;
}

QVariantList TelemetryService::getAlertHistory(int limitHours) const {
    QVariantList result;
    QDateTime cutoff = QDateTime::currentDateTime().addSecs(-limitHours * 3600);

    for (const Alert& alert : m_activeAlerts) {
        if (alert.createdAt >= cutoff) {
            QVariantMap alertMap;
            alertMap["alertId"] = alert.alertId;
            alertMap["level"] = alertLevelToString(alert.level);
            alertMap["title"] = alert.title;
            alertMap["message"] = alert.message;
            alertMap["source"] = alert.source;
            alertMap["createdAt"] = alert.createdAt;
            alertMap["acknowledgedAt"] = alert.acknowledgedAt;
            alertMap["acknowledgedBy"] = alert.acknowledgedBy;
            alertMap["isActive"] = alert.isActive;
            alertMap["metadata"] = alert.metadata;
            result.append(alertMap);
        }
    }

    return result;
}

QVariantList TelemetryService::getPerformanceTrends(const QString& operation, int intervalMinutes, int periodHours) const {
    QVariantList result;
    QDateTime cutoff = QDateTime::currentDateTime().addSecs(-periodHours * 3600);
    QDateTime intervalStart = cutoff;

    while (intervalStart < QDateTime::currentDateTime()) {
        QDateTime intervalEnd = intervalStart.addSecs(intervalMinutes * 60);

        QList<double> responseTimes;
        int successCount = 0;
        int totalCount = 0;

        for (const PerformanceMetric& metric : m_performanceMetrics) {
            if (metric.timestamp >= intervalStart && metric.timestamp < intervalEnd) {
                if (operation.isEmpty() || metric.operation == operation) {
                    responseTimes.append(metric.responseTimeMs);
                    totalCount++;
                    if (metric.success) {
                        successCount++;
                    }
                }
            }
        }

        if (totalCount > 0) {
            double average = std::accumulate(responseTimes.begin(), responseTimes.end(), 0.0) / responseTimes.size();
            double successRate = (double)successCount / totalCount * 100.0;

            QVariantMap intervalData;
            intervalData["intervalStart"] = intervalStart;
            intervalData["intervalEnd"] = intervalEnd;
            intervalData["operation"] = operation;
            intervalData["count"] = totalCount;
            intervalData["averageResponseTime"] = average;
            intervalData["successRate"] = successRate;
            result.append(intervalData);
        }

        intervalStart = intervalEnd;
    }

    return result;
}

QVariantMap TelemetryService::generatePerformanceReport(const QDateTime& startTime, const QDateTime& endTime) const {
    QVariantMap report;

    // Calculate metrics for the specified period
    QList<double> responseTimes;
    int totalOperations = 0;
    int successfulOperations = 0;
    QHash<QString, int> operationCounts;

    for (const PerformanceMetric& metric : m_performanceMetrics) {
        if (metric.timestamp >= startTime && metric.timestamp <= endTime) {
            responseTimes.append(metric.responseTimeMs);
            totalOperations++;
            if (metric.success) {
                successfulOperations++;
            }
            operationCounts[metric.operation]++;
        }
    }

    if (!responseTimes.isEmpty()) {
        std::sort(responseTimes.begin(), responseTimes.end());
        double average = std::accumulate(responseTimes.begin(), responseTimes.end(), 0.0) / responseTimes.size();

        report["reportPeriod"] = QVariantMap{
            {"startTime", startTime},
            {"endTime", endTime}
        };
        report["totalOperations"] = totalOperations;
        report["successfulOperations"] = successfulOperations;
        report["successRate"] = totalOperations > 0 ? (double)successfulOperations / totalOperations * 100.0 : 0.0;
        report["averageResponseTime"] = average;
        report["minResponseTime"] = responseTimes.first();
        report["maxResponseTime"] = responseTimes.last();
        report["p95ResponseTime"] = responseTimes[qRound(responseTimes.size() * 0.95) - 1];

        QVariantMap operationBreakdown;
        for (auto it = operationCounts.begin(); it != operationCounts.end(); ++it) {
            operationBreakdown[it.key()] = it.value();
        }
        report["operationBreakdown"] = operationBreakdown;
    }

    return report;
}

QVariantMap TelemetryService::generateSafetyReport(const QDateTime& startTime, const QDateTime& endTime) const {
    QVariantMap report;

    QHash<QString, int> eventTypeCounts;
    QHash<QString, int> severityCounts;
    int totalEvents = 0;

    for (const SafetyMetric& metric : m_safetyMetrics) {
        if (metric.timestamp >= startTime && metric.timestamp <= endTime) {
            eventTypeCounts[metric.eventType]++;
            severityCounts[alertLevelToString(metric.severity)]++;
            totalEvents++;
        }
    }

    QVariantMap reportPeriod;
    reportPeriod["startTime"] = startTime;
    reportPeriod["endTime"] = endTime;
    report["reportPeriod"] = reportPeriod;
    report["totalSafetyEvents"] = totalEvents;

    QVariantMap eventBreakdown;
    for (auto it = eventTypeCounts.begin(); it != eventTypeCounts.end(); ++it) {
        eventBreakdown[it.key()] = it.value();
    }
    report["eventTypeBreakdown"] = eventBreakdown;

    QVariantMap severityBreakdown;
    for (auto it = severityCounts.begin(); it != severityCounts.end(); ++it) {
        severityBreakdown[it.key()] = it.value();
    }
    report["severityBreakdown"] = severityBreakdown;

    return report;
}

} // namespace RailFlux::Route
