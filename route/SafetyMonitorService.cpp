#include "SafetyMonitorService.h"
#include "../database/DatabaseManager.h"
#include "TelemetryService.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <QtMath>
#include <algorithm>

namespace RailFlux::Route {

SafetyMonitorService::SafetyMonitorService(
    DatabaseManager* dbManager,
    TelemetryService* telemetryService,
    QObject* parent
    )
    : QObject(parent)
    , m_dbManager(dbManager)
    , m_telemetryService(telemetryService)
    , m_monitoringTimer(new QTimer(this))
    , m_alertTimer(new QTimer(this))
{
    // Setup monitoring timer
    m_monitoringTimer->setInterval(m_monitoringIntervalMs);
    connect(m_monitoringTimer, &QTimer::timeout, this, &SafetyMonitorService::performPeriodicSafetyCheck);

    // Setup alert timer
    m_alertTimer->setInterval(m_alertCheckIntervalMs);
    connect(m_alertTimer, &QTimer::timeout, this, &SafetyMonitorService::processAlerts);
}

SafetyMonitorService::~SafetyMonitorService() {
    if (m_monitoringTimer) {
        m_monitoringTimer->stop();
    }
    if (m_alertTimer) {
        m_alertTimer->stop();
    }
}

void SafetyMonitorService::initialize() {
    if (!m_dbManager) {
        qCritical() << "[SafetyMonitorService > initialize] DatabaseManager not set";
        return;
    }

    try {
        if (loadComplianceConfiguration()) {
            if (loadActiveViolationsFromDatabase()) {
                loadDefaultAlertThresholds();

                m_currentComplianceLevel = calculateOverallCompliance();
                m_currentComplianceScore = calculateComplianceScore();

                m_isOperational = true;

                emit operationalStateChanged();
                emit complianceLevelChanged();
                emit complianceScoreChanged();

                if (m_telemetryService) {
                    m_telemetryService->recordSafetyEvent(
                        "safety_monitor_initialized",
                        "INFO",
                        "SafetyMonitorService",
                        QString("Compliance monitoring initialized - Level: %1, Score: %2%")
                            .arg(complianceLevelToString(m_currentComplianceLevel))
                            .arg(m_currentComplianceScore, 0, 'f', 1),
                        "system"
                        );
                }
            } else {
                qCritical() << "[SafetyMonitorService > initialize] Failed to load active violations";
            }
        } else {
            qCritical() << "[SafetyMonitorService > initialize] Failed to load configuration";
        }

    } catch (const std::exception& e) {
        qCritical() << "[SafetyMonitorService > initialize] Initialization failed:" << e.what();
        m_isOperational = false;
        emit operationalStateChanged();
    }
}

int SafetyMonitorService::activeViolations() const {
    return m_activeViolations.size();
}

void SafetyMonitorService::startContinuousMonitoring() {
    if (!m_isOperational) {
        qWarning() << "[SafetyMonitorService > startContinuousMonitoring] Cannot start monitoring - service not operational";
        return;
    }

    if (m_continuousMonitoring) {
        return;
    }

    m_continuousMonitoring = true;
    m_monitoringTimer->start();
    m_alertTimer->start();

    if (m_telemetryService) {
        m_telemetryService->recordSafetyEvent(
            "continuous_monitoring_started",
            "INFO",
            "SafetyMonitorService",
            "Continuous safety monitoring activated",
            "system"
            );
    }
}

void SafetyMonitorService::stopContinuousMonitoring() {
    if (!m_continuousMonitoring) {
        return;
    }

    m_continuousMonitoring = false;
    m_monitoringTimer->stop();
    m_alertTimer->stop();

    if (m_telemetryService) {
        m_telemetryService->recordSafetyEvent(
            "continuous_monitoring_stopped",
            "INFO",
            "SafetyMonitorService",
            "Continuous safety monitoring deactivated",
            "system"
            );
    }
}

bool SafetyMonitorService::performSafetyAudit() {
    if (!m_isOperational) {
        return false;
    }

    QElapsedTimer auditTimer;
    auditTimer.start();

    int initialViolations = m_activeViolations.size();
    int newViolations = 0;

    try {
        // Perform all safety checks
        checkRouteConflicts();
        checkSignalCompliance();
        checkTrackCircuitCompliance();
        checkPointMachineCompliance();
        checkOverlapCompliance();
        checkTimingCompliance();
        checkInterlockingCompliance();
        checkOperatorCompliance();
        checkSystemIntegrity();

        newViolations = m_activeViolations.size() - initialViolations;

        // Update compliance metrics
        ComplianceLevel previousLevel = m_currentComplianceLevel;
        m_currentComplianceLevel = calculateOverallCompliance();
        m_currentComplianceScore = calculateComplianceScore();

        double auditTime = auditTimer.elapsed();
        recordMonitoringMetrics("comprehensive_audit", auditTime, newViolations);

        if (newViolations > 0 && previousLevel != m_currentComplianceLevel) {
            emit complianceLevelChanged();
            emit complianceLevelDowngraded(
                complianceLevelToString(previousLevel),
                complianceLevelToString(m_currentComplianceLevel)
                );
        }

        emit complianceScoreChanged();

        if (m_telemetryService) {
            m_telemetryService->recordSafetyEvent(
                "safety_audit_completed",
                newViolations > 0 ? "WARNING" : "INFO",
                "SafetyMonitorService",
                QString("Audit completed - %1 new violations, compliance: %2%")
                    .arg(newViolations)
                    .arg(m_currentComplianceScore, 0, 'f', 1),
                "system"
                );
        }

        QString auditId = generateReportId();
        emit safetyAuditCompleted(auditId, m_currentComplianceLevel == ComplianceLevel::COMPLIANT ? "PASS" : "FAIL");

        return true;

    } catch (const std::exception& e) {
        qCritical() << "[SafetyMonitorService > performSafetyAudit] Safety audit failed:" << e.what();
        return false;
    }
}

void SafetyMonitorService::performPeriodicSafetyCheck() {
    if (!m_isOperational) {
        return;
    }

    QElapsedTimer checkTimer;
    checkTimer.start();

    m_totalChecks++;

    // Perform lightweight periodic checks
    checkRouteConflicts();
    checkSignalCompliance();
    checkTrackCircuitCompliance();

    double checkTime = checkTimer.elapsed();
    recordMonitoringMetrics("periodic_check", checkTime, 0);

    // Update performance statistics
    updatePerformanceStatistics();
}

QString SafetyMonitorService::reportViolation(
    const QString& violationType,
    const QString& description,
    const QString& affectedResource,
    const QString& operatorId,
    const QVariantMap& metadata
    ) {
    if (!m_isOperational) {
        return QString();
    }

    // Create violation
    SafetyViolation violation;
    violation.id = generateViolationId();
    violation.type = stringToViolationType(violationType);
    violation.description = description;
    violation.affectedResource = affectedResource;
    violation.operatorId = operatorId;
    violation.detectedAt = QDateTime::currentDateTime();
    violation.metadata = metadata;
    violation.isActive = true;

    // Determine severity based on type and context
    violation.severity = isViolationCritical(violation) ?
                             ComplianceLevel::SAFETY_CRITICAL : ComplianceLevel::MAJOR_DEVIATION;

    // Store violation
    m_activeViolations[violation.id] = violation;
    m_recentViolations.enqueue(violation);

    if (m_recentViolations.size() > MAX_RECENT_VIOLATIONS) {
        m_recentViolations.dequeue();
    }

    // Persist to database
    persistViolation(violation);

    // Update compliance metrics
    ComplianceLevel previousLevel = m_currentComplianceLevel;
    m_currentComplianceLevel = calculateOverallCompliance();
    m_currentComplianceScore = calculateComplianceScore();

    m_violationsDetected++;

    qWarning() << "[SafetyMonitorService > reportViolation] id:" << violation.id
               << "| type:" << violationType
               << "| severity:" << complianceLevelToString(violation.severity)
               << "| resource:" << affectedResource;

    // Emit signals
    emit violationDetected(violation.id, violationType, complianceLevelToString(violation.severity));
    emit violationCountChanged();

    if (violation.severity == ComplianceLevel::SAFETY_CRITICAL) {
        emit criticalViolationDetected(violation.id, description);
    }

    if (previousLevel != m_currentComplianceLevel) {
        emit complianceLevelChanged();
        emit complianceLevelDowngraded(
            complianceLevelToString(previousLevel),
            complianceLevelToString(m_currentComplianceLevel)
            );
    }

    emit complianceScoreChanged();

    // Record in telemetry
    if (m_telemetryService) {
        m_telemetryService->recordSafetyEvent(
            "safety_violation_reported",
            violation.severity == ComplianceLevel::SAFETY_CRITICAL ? "CRITICAL" : "WARNING",
            violation.affectedResource,
            QString("Violation: %1 - %2").arg(violationType, description),
            operatorId
            );
    }

    return violation.id;
}

bool SafetyMonitorService::acknowledgeViolation(const QString& violationId, const QString& operatorId) {
    if (!m_activeViolations.contains(violationId)) {
        qWarning() << "[SafetyMonitorService > acknowledgeViolation] Unknown violation:" << violationId;
        return false;
    }

    SafetyViolation& violation = m_activeViolations[violationId];

    if (!violation.acknowledgedAt.isNull()) {
        return true;
    }

    violation.acknowledgedAt = QDateTime::currentDateTime();
    updateViolationInDatabase(violation);

    if (m_telemetryService) {
        m_telemetryService->recordSafetyEvent(
            "safety_violation_acknowledged",
            "INFO",
            violation.affectedResource,
            QString("Violation %1 acknowledged").arg(violationId),
            operatorId
            );
    }

    return true;
}

bool SafetyMonitorService::resolveViolation(
    const QString& violationId,
    const QString& resolution,
    const QString& operatorId
    ) {
    if (!m_activeViolations.contains(violationId)) {
        qWarning() << "[SafetyMonitorService > resolveViolation] Unknown violation:" << violationId;
        return false;
    }

    SafetyViolation& violation = m_activeViolations[violationId];

    if (!violation.resolvedAt.isNull()) {
        return true;
    }

    violation.resolvedAt = QDateTime::currentDateTime();
    violation.resolution = resolution;
    violation.isActive = false;

    updateViolationInDatabase(violation);

    // Remove from active violations
    m_activeViolations.remove(violationId);

    // Update compliance metrics
    ComplianceLevel previousLevel = m_currentComplianceLevel;
    m_currentComplianceLevel = calculateOverallCompliance();
    m_currentComplianceScore = calculateComplianceScore();

    m_violationsResolved++;

    // Emit signals
    emit violationResolved(violationId, resolution);
    emit violationCountChanged();

    if (previousLevel != m_currentComplianceLevel) {
        emit complianceLevelChanged();
    }

    emit complianceScoreChanged();

    // Record in telemetry
    if (m_telemetryService) {
        m_telemetryService->recordSafetyEvent(
            "safety_violation_resolved",
            "INFO",
            violation.affectedResource,
            QString("Violation %1 resolved: %2").arg(violationId, resolution),
            operatorId
            );
    }

    return true;
}

QVariantMap SafetyMonitorService::checkRouteCompliance(const QString& routeId) {
    QVariantMap result;

    if (!m_isOperational) {
        result["success"] = false;
        result["error"] = "Service not operational";
        return result;
    }

    QElapsedTimer checkTimer;
    checkTimer.start();

    QList<SafetyViolation> routeViolations;

    // Check for route-specific violations
    for (const SafetyViolation& violation : m_activeViolations.values()) {
        if (violation.affectedRoutes.contains(routeId)) {
            routeViolations.append(violation);
        }
    }

    double checkTime = checkTimer.elapsed();

    // Determine compliance level for this route
    ComplianceLevel routeCompliance = ComplianceLevel::COMPLIANT;
    if (!routeViolations.isEmpty()) {
        for (const SafetyViolation& violation : routeViolations) {
            if (violation.severity > routeCompliance) {
                routeCompliance = violation.severity;
            }
        }
    }

    result["success"] = true;
    result["routeId"] = routeId;
    result["complianceLevel"] = complianceLevelToString(routeCompliance);
    result["violationCount"] = routeViolations.size();
    result["checkTimeMs"] = checkTime;

    QVariantList violationsList;
    for (const SafetyViolation& violation : routeViolations) {
        violationsList.append(violationToVariantMap(violation));
    }
    result["violations"] = violationsList;

    return result;
}

QVariantMap SafetyMonitorService::checkSystemCompliance() {
    QVariantMap result;

    if (!m_isOperational) {
        result["success"] = false;
        result["error"] = "Service not operational";
        return result;
    }

    QElapsedTimer checkTimer;
    checkTimer.start();

    // Perform quick system-wide compliance check
    int criticalViolations = 0;
    int majorViolations = 0;
    int minorViolations = 0;

    for (const SafetyViolation& violation : m_activeViolations.values()) {
        switch (violation.severity) {
        case ComplianceLevel::SAFETY_CRITICAL:
        case ComplianceLevel::NON_COMPLIANT:
            criticalViolations++;
            break;
        case ComplianceLevel::MAJOR_DEVIATION:
            majorViolations++;
            break;
        case ComplianceLevel::MINOR_DEVIATION:
            minorViolations++;
            break;
        default:
            break;
        }
    }

    double checkTime = checkTimer.elapsed();

    result["success"] = true;
    result["overallComplianceLevel"] = complianceLevelToString(m_currentComplianceLevel);
    result["complianceScore"] = m_currentComplianceScore;
    result["totalViolations"] = m_activeViolations.size();
    result["criticalViolations"] = criticalViolations;
    result["majorViolations"] = majorViolations;
    result["minorViolations"] = minorViolations;
    result["checkTimeMs"] = checkTime;
    result["lastAuditTime"] = QDateTime::currentDateTime().toString(Qt::ISODate);

    QStringList recommendations = generateRecommendations();
    result["recommendations"] = QVariantList(recommendations.begin(), recommendations.end());

    return result;
}

QString SafetyMonitorService::generateComplianceReport(
    const QDateTime& periodStart,
    const QDateTime& periodEnd
    ) {
    if (!m_isOperational) {
        return QString();
    }

    ComplianceReport report = generateComplianceReportInternal(periodStart, periodEnd);

    if (saveComplianceReport(report)) {
        return report.reportId;
    }

    return QString();
}

QVariantMap SafetyMonitorService::getCurrentComplianceStatus() const {
    QVariantMap status;

    status["isOperational"] = m_isOperational;
    status["complianceLevel"] = complianceLevelToString(m_currentComplianceLevel);
    status["complianceScore"] = m_currentComplianceScore;
    status["activeViolations"] = m_activeViolations.size();
    status["continuousMonitoring"] = m_continuousMonitoring;
    status["lastUpdateTime"] = QDateTime::currentDateTime().toString(Qt::ISODate);

    // Statistics
    status["totalChecks"] = m_totalChecks;
    status["violationsDetected"] = m_violationsDetected;
    status["violationsResolved"] = m_violationsResolved;
    status["criticalViolations"] = m_criticalViolations;
    status["alertsSent"] = m_alertsSent;

    // Performance
    status["averageMonitoringTimeMs"] = m_averageMonitoringTime;

    return status;
}

// Core monitoring functions
void SafetyMonitorService::checkRouteConflicts() {
    QList<SafetyViolation> violations = detectRouteConflicts();

    for (const SafetyViolation& violation : violations) {
        if (!m_activeViolations.contains(violation.id)) {
            SafetyViolation newViolation = violation;
            newViolation.id = generateViolationId();
            newViolation.detectedAt = QDateTime::currentDateTime();

            m_activeViolations[newViolation.id] = newViolation;
            persistViolation(newViolation);
        }
    }
}

void SafetyMonitorService::checkSignalCompliance() {
    QList<SafetyViolation> violations = detectSignalViolations();

    for (const SafetyViolation& violation : violations) {
        if (!m_activeViolations.contains(violation.id)) {
            SafetyViolation newViolation = violation;
            newViolation.id = generateViolationId();
            newViolation.detectedAt = QDateTime::currentDateTime();

            m_activeViolations[newViolation.id] = newViolation;
            persistViolation(newViolation);
        }
    }
}

void SafetyMonitorService::checkTrackCircuitCompliance() {
    // Implementation for track circuit compliance checking
}

void SafetyMonitorService::checkPointMachineCompliance() {
    // Implementation for point machine compliance checking
}

void SafetyMonitorService::checkOverlapCompliance() {
    // Implementation for overlap compliance checking
}

void SafetyMonitorService::checkTimingCompliance() {
    // Implementation for timing compliance checking
}

void SafetyMonitorService::checkInterlockingCompliance() {
    // Implementation for interlocking compliance checking
}

void SafetyMonitorService::checkOperatorCompliance() {
    // Implementation for operator compliance checking
}

void SafetyMonitorService::checkSystemIntegrity() {
    // Implementation for system integrity checking
}

// Violation detection algorithms (simplified implementations)
QList<SafetyViolation> SafetyMonitorService::detectRouteConflicts() {
    QList<SafetyViolation> violations;
    // Implementation for detecting route conflicts
    return violations;
}

QList<SafetyViolation> SafetyMonitorService::detectSignalViolations() {
    QList<SafetyViolation> violations;
    // Implementation for detecting signal violations
    return violations;
}

QList<SafetyViolation> SafetyMonitorService::detectTrackCircuitViolations() {
    QList<SafetyViolation> violations;
    return violations;
}

QList<SafetyViolation> SafetyMonitorService::detectPointMachineViolations() {
    QList<SafetyViolation> violations;
    return violations;
}

QList<SafetyViolation> SafetyMonitorService::detectOverlapViolations() {
    QList<SafetyViolation> violations;
    return violations;
}

QList<SafetyViolation> SafetyMonitorService::detectTimingViolations() {
    QList<SafetyViolation> violations;
    return violations;
}

QList<SafetyViolation> SafetyMonitorService::detectInterlockingViolations() {
    QList<SafetyViolation> violations;
    return violations;
}

QList<SafetyViolation> SafetyMonitorService::detectOperatorViolations() {
    QList<SafetyViolation> violations;
    return violations;
}

QList<SafetyViolation> SafetyMonitorService::detectSystemIntegrityViolations() {
    QList<SafetyViolation> violations;
    return violations;
}

// Analysis and scoring
ComplianceLevel SafetyMonitorService::calculateOverallCompliance() const {
    if (m_activeViolations.isEmpty()) {
        return ComplianceLevel::COMPLIANT;
    }

    ComplianceLevel mostSevere = ComplianceLevel::COMPLIANT;
    for (const SafetyViolation& violation : m_activeViolations.values()) {
        if (violation.severity > mostSevere) {
            mostSevere = violation.severity;
        }
    }

    return mostSevere;
}

double SafetyMonitorService::calculateComplianceScore() const {
    if (m_activeViolations.isEmpty()) {
        return 100.0;
    }

    double score = 100.0;

    for (const SafetyViolation& violation : m_activeViolations.values()) {
        switch (violation.severity) {
        case ComplianceLevel::SAFETY_CRITICAL:
        case ComplianceLevel::NON_COMPLIANT:
            score -= 20.0;
            break;
        case ComplianceLevel::MAJOR_DEVIATION:
            score -= 10.0;
            break;
        case ComplianceLevel::MINOR_DEVIATION:
            score -= 2.0;
            break;
        default:
            break;
        }
    }

    return qMax(0.0, score);
}

QStringList SafetyMonitorService::generateRecommendations() const {
    QStringList recommendations;

    if (m_activeViolations.isEmpty()) {
        recommendations.append("System is fully compliant - maintain current safety protocols");
        return recommendations;
    }

    int criticalCount = 0;
    int majorCount = 0;

    for (const SafetyViolation& violation : m_activeViolations.values()) {
        if (violation.severity >= ComplianceLevel::SAFETY_CRITICAL) {
            criticalCount++;
        } else if (violation.severity == ComplianceLevel::MAJOR_DEVIATION) {
            majorCount++;
        }
    }

    if (criticalCount > 0) {
        recommendations.append(QString("URGENT: Address %1 critical safety violations immediately").arg(criticalCount));
    }

    if (majorCount > 0) {
        recommendations.append(QString("Review and resolve %1 major compliance deviations").arg(majorCount));
    }

    if (m_activeViolations.size() > 10) {
        recommendations.append("High violation count - consider comprehensive safety review");
    }

    return recommendations;
}

bool SafetyMonitorService::isViolationCritical(const SafetyViolation& violation) const {
    return violation.type == ViolationType::ROUTE_CONFLICT ||
           violation.type == ViolationType::SIGNAL_VIOLATION ||
           violation.type == ViolationType::INTERLOCKING_VIOLATION ||
           violation.type == ViolationType::EMERGENCY_PROTOCOL;
}

// Utility methods
QString SafetyMonitorService::generateViolationId() const {
    return QString("VIO_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces).left(8).toUpper());
}

QString SafetyMonitorService::generateReportId() const {
    return QString("RPT_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces).left(8).toUpper());
}

ViolationType SafetyMonitorService::stringToViolationType(const QString& typeStr) const {
    if (typeStr == "ROUTE_CONFLICT") return ViolationType::ROUTE_CONFLICT;
    if (typeStr == "SIGNAL_VIOLATION") return ViolationType::SIGNAL_VIOLATION;
    if (typeStr == "TRACK_CIRCUIT_VIOLATION") return ViolationType::TRACK_CIRCUIT_VIOLATION;
    if (typeStr == "POINT_MACHINE_VIOLATION") return ViolationType::POINT_MACHINE_VIOLATION;
    if (typeStr == "OVERLAP_VIOLATION") return ViolationType::OVERLAP_VIOLATION;
    if (typeStr == "TIMING_VIOLATION") return ViolationType::TIMING_VIOLATION;
    if (typeStr == "INTERLOCKING_VIOLATION") return ViolationType::INTERLOCKING_VIOLATION;
    if (typeStr == "OPERATOR_VIOLATION") return ViolationType::OPERATOR_VIOLATION;
    if (typeStr == "SYSTEM_INTEGRITY") return ViolationType::SYSTEM_INTEGRITY;
    if (typeStr == "EMERGENCY_PROTOCOL") return ViolationType::EMERGENCY_PROTOCOL;
    return ViolationType::SYSTEM_INTEGRITY; // Default
}

QString SafetyMonitorService::violationTypeToString(ViolationType type) const {
    switch (type) {
    case ViolationType::ROUTE_CONFLICT: return "ROUTE_CONFLICT";
    case ViolationType::SIGNAL_VIOLATION: return "SIGNAL_VIOLATION";
    case ViolationType::TRACK_CIRCUIT_VIOLATION: return "TRACK_CIRCUIT_VIOLATION";
    case ViolationType::POINT_MACHINE_VIOLATION: return "POINT_MACHINE_VIOLATION";
    case ViolationType::OVERLAP_VIOLATION: return "OVERLAP_VIOLATION";
    case ViolationType::TIMING_VIOLATION: return "TIMING_VIOLATION";
    case ViolationType::INTERLOCKING_VIOLATION: return "INTERLOCKING_VIOLATION";
    case ViolationType::OPERATOR_VIOLATION: return "OPERATOR_VIOLATION";
    case ViolationType::SYSTEM_INTEGRITY: return "SYSTEM_INTEGRITY";
    case ViolationType::EMERGENCY_PROTOCOL: return "EMERGENCY_PROTOCOL";
    }
    return "UNKNOWN";
}

ComplianceLevel SafetyMonitorService::stringToComplianceLevel(const QString& levelStr) const {
    if (levelStr == "COMPLIANT") return ComplianceLevel::COMPLIANT;
    if (levelStr == "MINOR_DEVIATION") return ComplianceLevel::MINOR_DEVIATION;
    if (levelStr == "MAJOR_DEVIATION") return ComplianceLevel::MAJOR_DEVIATION;
    if (levelStr == "SAFETY_CRITICAL") return ComplianceLevel::SAFETY_CRITICAL;
    if (levelStr == "NON_COMPLIANT") return ComplianceLevel::NON_COMPLIANT;
    return ComplianceLevel::NON_COMPLIANT; // Default to most restrictive
}

QString SafetyMonitorService::complianceLevelToString(ComplianceLevel level) const {
    switch (level) {
    case ComplianceLevel::COMPLIANT: return "COMPLIANT";
    case ComplianceLevel::MINOR_DEVIATION: return "MINOR_DEVIATION";
    case ComplianceLevel::MAJOR_DEVIATION: return "MAJOR_DEVIATION";
    case ComplianceLevel::SAFETY_CRITICAL: return "SAFETY_CRITICAL";
    case ComplianceLevel::NON_COMPLIANT: return "NON_COMPLIANT";
    }
    return "UNKNOWN";
}

QVariantMap SafetyMonitorService::violationToVariantMap(const SafetyViolation& violation) const {
    QVariantMap map;
    map["id"] = violation.id;
    map["type"] = violationTypeToString(violation.type);
    map["severity"] = complianceLevelToString(violation.severity);
    map["description"] = violation.description;
    map["affectedResource"] = violation.affectedResource;
    map["affectedRoutes"] = QVariantList(violation.affectedRoutes.begin(), violation.affectedRoutes.end());
    map["operatorId"] = violation.operatorId;
    map["detectedAt"] = violation.detectedAt.toString(Qt::ISODate);
    map["acknowledgedAt"] = violation.acknowledgedAt.toString(Qt::ISODate);
    map["resolvedAt"] = violation.resolvedAt.toString(Qt::ISODate);
    map["resolution"] = violation.resolution;
    map["metadata"] = violation.metadata;
    map["isActive"] = violation.isActive;
    map["durationMs"] = violation.durationMs();
    return map;
}

void SafetyMonitorService::recordMonitoringMetrics(const QString& checkType, double durationMs, int violationsFound) {
    Q_UNUSED(checkType)
    Q_UNUSED(violationsFound)

    m_monitoringTimes.append(durationMs);
    if (m_monitoringTimes.size() > PERFORMANCE_HISTORY_SIZE) {
        m_monitoringTimes.removeFirst();
    }
}

void SafetyMonitorService::updatePerformanceStatistics() {
    if (m_monitoringTimes.isEmpty()) {
        return;
    }

    double total = std::accumulate(m_monitoringTimes.begin(), m_monitoringTimes.end(), 0.0);
    m_averageMonitoringTime = total / m_monitoringTimes.size();

    m_lastPerformanceUpdate = QDateTime::currentDateTime();
}

void SafetyMonitorService::loadDefaultAlertThresholds() {
    m_alertThresholds["compliance_score"] = WARNING_COMPLIANCE_THRESHOLD;
    m_alertThresholds["active_violations"] = 5.0;
    m_alertThresholds["critical_violations"] = 1.0;
    m_alertThresholds["monitoring_time"] = WARNING_MONITORING_TIME_MS;
}

void SafetyMonitorService::processAlerts() {
    if (!m_isOperational) {
        return;
    }

    // Check compliance score threshold
    if (shouldSendAlert("compliance_score", m_currentComplianceScore)) {
        QVariantMap alertData;
        alertData["metric"] = "compliance_score";
        alertData["currentValue"] = m_currentComplianceScore;
        alertData["threshold"] = m_alertThresholds["compliance_score"];
        sendAlert("compliance_threshold_breached", alertData);
    }

    // Check active violations threshold
    if (shouldSendAlert("active_violations", m_activeViolations.size())) {
        QVariantMap alertData;
        alertData["metric"] = "active_violations";
        alertData["currentValue"] = m_activeViolations.size();
        alertData["threshold"] = m_alertThresholds["active_violations"];
        sendAlert("violation_count_threshold_breached", alertData);
    }
}

bool SafetyMonitorService::shouldSendAlert(const QString& metricType, double value) const {
    if (!m_alertThresholds.contains(metricType)) {
        return false;
    }

    double threshold = m_alertThresholds[metricType];

    if (metricType == "compliance_score") {
        return value < threshold;
    } else {
        return value > threshold;
    }
}

void SafetyMonitorService::sendAlert(const QString& alertType, const QVariantMap& alertData) {
    m_alertsSent++;
    m_lastAlertSent = QDateTime::currentDateTime();

    qWarning() << "[SafetyMonitorService > sendAlert] type:" << alertType
               << "| metric:" << alertData.value("metric").toString()
               << "| value:" << alertData.value("currentValue").toDouble()
               << "| threshold:" << alertData.value("threshold").toDouble();

    emit complianceThresholdBreached(
        alertData["metric"].toString(),
        alertData["currentValue"].toDouble(),
        alertData["threshold"].toDouble()
        );

    if (m_telemetryService) {
        m_telemetryService->recordSafetyEvent(
            QString("alert_%1").arg(alertType),
            "WARNING",
            "SafetyMonitorService",
            QString("Alert: %1").arg(alertType),
            "system"
            );
    }
}

// Database integration stubs
bool SafetyMonitorService::loadComplianceConfiguration() {
    return true; // Placeholder
}

bool SafetyMonitorService::persistViolation(const SafetyViolation& violation) {
    Q_UNUSED(violation)
    return true; // Placeholder
}

bool SafetyMonitorService::updateViolationInDatabase(const SafetyViolation& violation) {
    Q_UNUSED(violation)
    return true; // Placeholder
}

bool SafetyMonitorService::loadActiveViolationsFromDatabase() {
    return true; // Placeholder
}

ComplianceReport SafetyMonitorService::generateComplianceReportInternal(
    const QDateTime& periodStart,
    const QDateTime& periodEnd
    ) {
    ComplianceReport report;
    report.reportId = generateReportId();
    report.generatedAt = QDateTime::currentDateTime();
    report.periodStart = periodStart;
    report.periodEnd = periodEnd;
    report.overallCompliance = m_currentComplianceLevel;
    report.complianceScore = m_currentComplianceScore;

    report.totalViolations = m_activeViolations.size();
    report.activeViolations = m_activeViolations.size();

    return report;
}

bool SafetyMonitorService::saveComplianceReport(const ComplianceReport& report) {
    m_complianceReports[report.reportId] = report;
    return true; // Placeholder
}

// Event handler stubs
void SafetyMonitorService::onRouteStateChanged(const QString& routeId, const QString& newState) {
    Q_UNUSED(routeId) Q_UNUSED(newState)
    // Handle route state changes for compliance monitoring
}

void SafetyMonitorService::onTrackCircuitOccupancyChanged(const QString& circuitId, bool isOccupied) {
    Q_UNUSED(circuitId) Q_UNUSED(isOccupied)
    // Handle track circuit changes for compliance monitoring
}

void SafetyMonitorService::onSignalAspectChanged(const QString& signalId, const QString& aspect) {
    Q_UNUSED(signalId) Q_UNUSED(aspect)
    // Handle signal aspect changes for compliance monitoring
}

void SafetyMonitorService::onPointMachinePositionChanged(const QString& machineId, const QString& position) {
    Q_UNUSED(machineId) Q_UNUSED(position)
    // Handle point machine position changes for compliance monitoring
}

void SafetyMonitorService::onEmergencyActivated(const QString& reason) {
    Q_UNUSED(reason)
    // Handle emergency activation for compliance monitoring
}

void SafetyMonitorService::onSystemOverload() {
    // Handle system overload for compliance monitoring
    reportViolation(
        "SYSTEM_INTEGRITY",
        "System overload detected",
        "system",
        "system",
        QVariantMap{{"timestamp", QDateTime::currentDateTime().toString(Qt::ISODate)}}
        );
}

// Additional methods for main.cpp compatibility
void SafetyMonitorService::recordSafetyViolation(const QString& routeId, const QString& reason, const QString& severity) {
    reportViolation(
        "GENERAL_SAFETY_VIOLATION",
        reason,
        "system",
        routeId,
        QVariantMap{{"severity", severity}}
        );
}

void SafetyMonitorService::recordEmergencyEvent(const QString& eventType, const QString& reason) {
    reportViolation(
        "EMERGENCY_EVENT",
        QString("%1: %2").arg(eventType, reason),
        "system",
        "system",
        QVariantMap{{"eventType", eventType}}
        );
}

void SafetyMonitorService::recordPerformanceWarning(const QString& warningType, const QVariantMap& details) {
    reportViolation(
        "PERFORMANCE_WARNING",
        QString("Performance warning: %1").arg(warningType),
        "system",
        "system",
        details
        );
}

QVariantMap SafetyMonitorService::getViolationDetails(const QString& violationId) const {
    if (!m_activeViolations.contains(violationId)) {
        return QVariantMap();
    }

    return violationToVariantMap(m_activeViolations[violationId]);
}

QVariantList SafetyMonitorService::getActiveViolations() const {
    QVariantList result;

    for (const SafetyViolation& violation : m_activeViolations.values()) {
        result.append(violationToVariantMap(violation));
    }

    return result;
}

QVariantList SafetyMonitorService::getViolationHistory(int limitHours) const {
    Q_UNUSED(limitHours)
    // Would query database for violation history
    return QVariantList();
}

QVariantMap SafetyMonitorService::getComplianceReport(const QString& reportId) const {
    if (!m_complianceReports.contains(reportId)) {
        return QVariantMap();
    }

    const ComplianceReport& report = m_complianceReports[reportId];

    QVariantMap reportMap;
    reportMap["reportId"] = report.reportId;
    reportMap["generatedAt"] = report.generatedAt;
    reportMap["periodStart"] = report.periodStart;
    reportMap["periodEnd"] = report.periodEnd;
    reportMap["overallCompliance"] = complianceLevelToString(report.overallCompliance);
    reportMap["complianceScore"] = report.complianceScore;
    reportMap["totalViolations"] = report.totalViolations;
    reportMap["activeViolations"] = report.activeViolations;
    reportMap["resolvedViolations"] = report.resolvedViolations;
    reportMap["criticalViolations"] = report.criticalViolations;
    reportMap["recommendations"] = report.recommendations;

    return reportMap;
}

QVariantList SafetyMonitorService::getComplianceReports(int limitDays) const {
    Q_UNUSED(limitDays)

    QVariantList result;
    QDateTime cutoff = QDateTime::currentDateTime().addDays(-limitDays);

    for (const ComplianceReport& report : m_complianceReports.values()) {
        if (report.generatedAt >= cutoff) {
            result.append(getComplianceReport(report.reportId));
        }
    }

    return result;
}

void SafetyMonitorService::monitorRouteOperation(const QString& routeId) {
    Q_UNUSED(routeId)
    // Monitor specific route operation for compliance
}

void SafetyMonitorService::monitorResourceUsage(const QString& resourceType, const QString& resourceId) {
    Q_UNUSED(resourceType)
    Q_UNUSED(resourceId)
    // Monitor resource usage for compliance
}

void SafetyMonitorService::monitorOperatorActions(const QString& operatorId) {
    Q_UNUSED(operatorId)
    // Monitor operator actions for compliance
}

bool SafetyMonitorService::setAlertThreshold(const QString& metricType, double threshold) {
    if (threshold <= 0) {
        return false;
    }
    m_alertThresholds[metricType] = threshold;
    return true;
}

QVariantMap SafetyMonitorService::getAlertConfiguration() const {
    QVariantMap config;

    for (auto it = m_alertThresholds.begin(); it != m_alertThresholds.end(); ++it) {
        config[it.key()] = it.value();
    }

    return config;
}

QVariantList SafetyMonitorService::getPendingAlerts() const {
    QVariantList alerts;

    if (m_currentComplianceScore < m_alertThresholds.value("compliance_score", WARNING_COMPLIANCE_THRESHOLD)) {
        QVariantMap alert;
        alert["type"] = "compliance_score_low";
        alert["metric"] = "compliance_score";
        alert["currentValue"] = m_currentComplianceScore;
        alert["threshold"] = m_alertThresholds.value("compliance_score");
        alert["severity"] = "WARNING";
        alerts.append(alert);
    }

    if (m_activeViolations.size() > m_alertThresholds.value("active_violations", 5.0)) {
        QVariantMap alert;
        alert["type"] = "high_violation_count";
        alert["metric"] = "active_violations";
        alert["currentValue"] = m_activeViolations.size();
        alert["threshold"] = m_alertThresholds.value("active_violations");
        alert["severity"] = "WARNING";
        alerts.append(alert);
    }

    return alerts;
}

} // namespace RailFlux::Route
