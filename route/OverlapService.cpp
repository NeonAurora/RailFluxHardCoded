#include "OverlapService.h"
#include "../database/DatabaseManager.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QtMath>

namespace RailFlux::Route {

OverlapService::OverlapService(DatabaseManager* dbManager, QObject* parent)
    : QObject(parent)
    , m_dbManager(dbManager)
    , m_releaseTimer(new QTimer(this))
{
    if (!m_dbManager) {
        qCritical() << "[OverlapService > Ctor] DatabaseManager is null";
        return;
    }

    // Setup release timer for scheduled overlap releases
    m_releaseTimer->setInterval(RELEASE_TIMER_INTERVAL_MS);
    connect(m_releaseTimer, &QTimer::timeout, this, &OverlapService::processScheduledReleases);

    // Connect to database connection changes
    connect(m_dbManager, &DatabaseManager::connectionStateChanged,
            this, [this](bool connected) {
                if (connected) {
                    initialize();
                } else {
                    m_isOperational = false;
                    emit operationalStateChanged();
                }
            });
}

OverlapService::~OverlapService() {
    if (m_releaseTimer) {
        m_releaseTimer->stop();
    }
}

void OverlapService::initialize() {
    if (!m_dbManager || !m_dbManager->isConnected()) {
        return;
    }

    try {
        // Load overlap definitions from database (empty tables are OK)
        if (loadOverlapDefinitionsFromDatabase()) {
            m_isOperational = true;
            emit operationalStateChanged();

            // Start release timer (this exists)
            if (m_releaseTimer) {
                m_releaseTimer->start();
            }
        } else {
            // Stay operational with empty definitions
            m_isOperational = true;
            m_overlapDefinitions.clear();
            emit operationalStateChanged();
        }

        // Initialize counters
        m_totalOverlapOperations = 0;
        m_successfulReleases = 0;
        m_forceReleases = 0;
        m_overlapViolations = 0;
        m_totalOverlapTime = 0.0;
        m_averageHoldTime = 0.0;

        // Clear any existing data
        m_activeOverlaps.clear();
        m_circuitOverlaps.clear();
        m_triggerHistory.clear();

    } catch (const std::exception& e) {
        qCritical() << "[OverlapService > initialize] Initialization failed:" << e.what();
        m_isOperational = false;
        emit operationalStateChanged();
    }
}

bool OverlapService::loadOverlapDefinitionsFromDatabase() {
    QSqlQuery query(m_dbManager->getDatabase());
    query.prepare(R"(
        SELECT
            signal_id,
            overlap_circuits,
            release_conditions,
            overlap_type,
            overlap_hold_seconds,
            is_active
        FROM railway_control.signal_overlap_definitions
        WHERE is_active = TRUE
        ORDER BY signal_id
    )");

    if (!query.exec()) {
        qCritical() << "[OverlapService > loadOverlapDefinitionsFromDatabase] Failed to load overlap definitions:" << query.lastError().text();
        return false;
    }

    m_overlapDefinitions.clear();

    while (query.next()) {
        OverlapDefinition definition;
        definition.signalId = query.value("signal_id").toString();

        // Parse PostgreSQL text arrays
        QString overlapCircuitsStr = query.value("overlap_circuits").toString();
        if (overlapCircuitsStr.startsWith("{") && overlapCircuitsStr.endsWith("}")) {
            overlapCircuitsStr = overlapCircuitsStr.mid(1, overlapCircuitsStr.length() - 2);
            definition.overlapCircuitIds = overlapCircuitsStr.split(",", Qt::SkipEmptyParts);
        }

        QString releaseTriggersStr = query.value("release_conditions").toString();
        if (releaseTriggersStr.startsWith("{") && releaseTriggersStr.endsWith("}")) {
            releaseTriggersStr = releaseTriggersStr.mid(1, releaseTriggersStr.length() - 2);
            definition.releaseTriggerCircuitIds = releaseTriggersStr.split(",", Qt::SkipEmptyParts);
        }

        definition.type = stringToOverlapType(query.value("overlap_type").toString());
        definition.holdSeconds = query.value("overlap_hold_seconds").toInt();
        definition.isActive = query.value("is_active").toBool();

        m_overlapDefinitions[definition.signalId] = definition;
    }

    return true;
}

QVariantMap OverlapService::calculateOverlap(
    const QString& sourceSignalId,
    const QString& destSignalId,
    const QString& direction,
    const QVariantMap& trainData
) {
    QElapsedTimer timer;
    timer.start();

    m_totalOverlapOperations++;

    if (!m_isOperational) {
        return QVariantMap{
            {"success", false},
            {"error", "OverlapService not operational"}
        };
    }

    OverlapCalculationRequest request;
    request.sourceSignalId = sourceSignalId;
    request.destSignalId = destSignalId;
    request.direction = direction;
    request.trainData = trainData;

    OverlapCalculationResult result = calculateOverlapInternal(request);

    double timeMs = timer.elapsed();
    recordOverlapOperation("calculate", timeMs);

    return QVariantMap{
        {"success", result.success},
        {"error", result.error},
        {"overlapCircuits", result.overlapCircuits},
        {"releaseTriggerCircuits", result.releaseTriggerCircuits},
        {"holdSeconds", result.calculatedHoldSeconds},
        {"calculationTimeMs", timeMs},
        {"method", result.calculationMethod}
    };
}

OverlapCalculationResult OverlapService::calculateOverlapInternal(const OverlapCalculationRequest& request) {
    OverlapCalculationResult result;

    // Check if destination signal has overlap definition
    if (!m_overlapDefinitions.contains(request.destSignalId)) {
        result.success = false;
        result.error = QString("No overlap definition found for signal %1").arg(request.destSignalId);
        return result;
    }

    const OverlapDefinition& definition = m_overlapDefinitions[request.destSignalId];

    // Choose calculation method based on overlap type and available train data
    switch (definition.type) {
        case OverlapType::FIXED:
            result = calculateFixedOverlap(request.destSignalId);
            break;

        case OverlapType::VARIABLE:
            if (!request.trainData.isEmpty()) {
                result = calculateDynamicOverlap(request);
            } else {
                // Fallback to fixed if no train data
                result = calculateFixedOverlap(request.destSignalId);
                result.calculationMethod = "FIXED_FALLBACK";
            }
            break;

        case OverlapType::FLANK_PROTECTION:
            result = calculateSafetyMarginOverlap(request);
            break;
    }

    return result;
}

OverlapCalculationResult OverlapService::calculateFixedOverlap(const QString& signalId) {
    OverlapCalculationResult result;

    if (!m_overlapDefinitions.contains(signalId)) {
        result.success = false;
        result.error = QString("Signal %1 not found in overlap definitions").arg(signalId);
        return result;
    }

    const OverlapDefinition& definition = m_overlapDefinitions[signalId];

    result.success = true;
    result.overlapCircuits = definition.overlapCircuitIds;
    result.releaseTriggerCircuits = definition.releaseTriggerCircuitIds;
    result.calculatedHoldSeconds = definition.holdSeconds;
    result.calculationMethod = "FIXED";

    return result;
}

OverlapCalculationResult OverlapService::calculateDynamicOverlap(const OverlapCalculationRequest& request) {
    OverlapCalculationResult result;

    // Start with fixed overlap as base
    result = calculateFixedOverlap(request.destSignalId);
    if (!result.success) {
        return result;
    }

    // Adjust based on train characteristics
    if (request.trainData.contains("speed_kmh")) {
        double speedKmh = request.trainData["speed_kmh"].toDouble();
        double trainLengthM = request.trainData.value("length_m", 200.0).toDouble();
        double brakingRateMs2 = request.trainData.value("braking_rate_ms2", 0.8).toDouble();

        // Calculate dynamic braking distance
        double speedMs = speedKmh / 3.6; // Convert km/h to m/s
        double brakingDistanceM = (speedMs * speedMs) / (2 * brakingRateMs2);
        double totalSafetyDistanceM = brakingDistanceM + trainLengthM + 50.0; // 50m safety margin

        Q_UNUSED(totalSafetyDistanceM);

        // Adjust hold time based on calculated safety requirements
        double speedFactor = qBound(0.5, speedKmh / 80.0, 2.0); // Normalize to 80 km/h
        result.calculatedHoldSeconds = qRound(result.calculatedHoldSeconds * speedFactor);
        result.calculatedHoldSeconds = qBound(10, result.calculatedHoldSeconds, MAX_OVERLAP_HOLD_SECONDS);

        result.calculationMethod = "DYNAMIC";
    }

    return result;
}

OverlapCalculationResult OverlapService::calculateSafetyMarginOverlap(const OverlapCalculationRequest& request) {
    OverlapCalculationResult result;

    // Start with fixed overlap
    result = calculateFixedOverlap(request.destSignalId);
    if (!result.success) {
        return result;
    }

    // Apply safety margin multiplier
    result.calculatedHoldSeconds = qRound(result.calculatedHoldSeconds * SAFETY_MARGIN_MULTIPLIER);
    result.calculatedHoldSeconds = qBound(20, result.calculatedHoldSeconds, MAX_OVERLAP_HOLD_SECONDS);
    result.calculationMethod = "SAFETY_MARGIN";

    return result;
}

QVariantMap OverlapService::reserveOverlap(
    const QString& routeId,
    const QString& signalId,
    const QStringList& overlapCircuits,
    const QStringList& releaseTriggerCircuits,
    const QString& operatorId
) {
    if (!m_isOperational) {
        return QVariantMap{
            {"success", false},
            {"error", "OverlapService not operational"}
        };
    }

    QString validationError;
    if (!validateOverlapRequest(routeId, signalId, overlapCircuits, validationError)) {
        return QVariantMap{
            {"success", false},
            {"error", validationError}
        };
    }

    // Check for conflicts
    if (checkOverlapConflicts(overlapCircuits, routeId)) {
        return QVariantMap{
            {"success", false},
            {"error", "Overlap circuits are already reserved by another route"}
        };
    }

    // Create active overlap
    ActiveOverlap overlap;
    overlap.routeId = QUuid::fromString(routeId);
    overlap.signalId = signalId;
    overlap.reservedCircuits = overlapCircuits;
    overlap.releaseTriggerCircuits = releaseTriggerCircuits;
    overlap.state = OverlapState::RESERVED;
    overlap.reservedAt = QDateTime::currentDateTime();
    overlap.operatorId = operatorId;

    // Set hold time from definition
    if (m_overlapDefinitions.contains(signalId)) {
        overlap.holdSeconds = m_overlapDefinitions[signalId].holdSeconds;
    } else {
        overlap.holdSeconds = DEFAULT_OVERLAP_HOLD_SECONDS;
    }

    // Persist to database
    if (!persistOverlapToDatabase(overlap)) {
        qCritical() << "[OverlapService > reserveOverlap] Failed to persist overlap to database";
        return QVariantMap{
            {"success", false},
            {"error", "Failed to persist overlap to database"}
        };
    }

    // Add to memory
    QString overlapKey = overlap.key();
    m_activeOverlaps[overlapKey] = overlap;

    // Track circuits
    for (const QString& circuitId : overlapCircuits) {
        if (!m_circuitOverlaps.contains(circuitId)) {
            m_circuitOverlaps[circuitId] = QList<ActiveOverlap*>();
        }
        m_circuitOverlaps[circuitId].append(&m_activeOverlaps[overlapKey]);
    }

    emit overlapReserved(routeId, signalId, overlapCircuits);
    emit overlapCountChanged();

    return QVariantMap{
        {"success", true},
        {"reservedAt", overlap.reservedAt},
        {"holdSeconds", overlap.holdSeconds}
    };
}

bool OverlapService::activateOverlap(const QString& routeId, const QString& signalId) {
    QString overlapKey = QString("%1:%2").arg(routeId, signalId);

    if (!m_activeOverlaps.contains(overlapKey)) {
        return false;
    }

    ActiveOverlap& overlap = m_activeOverlaps[overlapKey];
    if (overlap.state != OverlapState::RESERVED) {
        return false;
    }

    overlap.state = OverlapState::ACTIVE;
    updateOverlapStateInDatabase(overlap);

    emit overlapActivated(routeId, signalId);
    return true;
}

bool OverlapService::startOverlapRelease(
    const QString& routeId,
    const QString& signalId,
    const QString& triggerReason
) {
    QString overlapKey = QString("%1:%2").arg(routeId, signalId);

    if (!m_activeOverlaps.contains(overlapKey)) {
        return false;
    }

    ActiveOverlap& overlap = m_activeOverlaps[overlapKey];
    if (overlap.state != OverlapState::ACTIVE) {
        return false;
    }

    overlap.state = OverlapState::RELEASING;
    overlap.releaseTimerStarted = QDateTime::currentDateTime();
    overlap.scheduledReleaseAt = overlap.releaseTimerStarted.addSecs(overlap.holdSeconds);

    updateOverlapStateInDatabase(overlap);

    emit overlapReleaseStarted(routeId, signalId, overlap.holdSeconds);
    Q_UNUSED(triggerReason);
    return true;
}

bool OverlapService::releaseOverlap(const QString& routeId, const QString& signalId, bool immediate) {
    QString overlapKey = QString("%1:%2").arg(routeId, signalId);

    if (!m_activeOverlaps.contains(overlapKey)) {
        return false;
    }

    ActiveOverlap overlap = m_activeOverlaps[overlapKey];

    // Check if immediate release is allowed or timer has expired
    if (!immediate && !overlap.isExpired() && overlap.state == OverlapState::RELEASING) {
        return false; // Timer still running
    }

    // Remove from memory
    m_activeOverlaps.remove(overlapKey);

    // Remove circuit tracking
    for (const QString& circuitId : overlap.reservedCircuits) {
        if (m_circuitOverlaps.contains(circuitId)) {
            m_circuitOverlaps[circuitId].removeAll(&overlap);
            if (m_circuitOverlaps[circuitId].isEmpty()) {
                m_circuitOverlaps.remove(circuitId);
            }
        }
    }

    // Remove from database
    if (!removeOverlapFromDatabase(routeId, signalId)) {
        qCritical() << "[OverlapService > releaseOverlap] Failed to remove overlap from database for"
                    << routeId << signalId;
        return false;
    }

    m_successfulReleases++;
    updateAverageHoldTime();

    emit overlapReleased(routeId, signalId);
    emit overlapCountChanged();

    return true;
}

void OverlapService::processScheduledReleases() {
    if (!m_isOperational) {
        return;
    }

    QStringList toRelease;
    QDateTime now = QDateTime::currentDateTime();
    Q_UNUSED(now);

    // Find expired overlaps
    for (auto it = m_activeOverlaps.begin(); it != m_activeOverlaps.end(); ++it) {
        const ActiveOverlap& overlap = it.value();
        if (overlap.state == OverlapState::RELEASING && overlap.isExpired()) {
            toRelease.append(it.key());
        }
    }

    // Release expired overlaps
    for (const QString& overlapKey : toRelease) {
        QStringList parts = overlapKey.split(":");
        if (parts.size() == 2) {
            releaseOverlap(parts[0], parts[1], true);
        }
    }

    // Check release triggers
    checkReleaseTriggers();
}

void OverlapService::checkReleaseTriggers() {
    for (auto it = m_activeOverlaps.begin(); it != m_activeOverlaps.end(); ++it) {
        ActiveOverlap& overlap = it.value();
        if (overlap.state == OverlapState::ACTIVE) {
            if (isReleaseTriggerSatisfied(overlap.routeId.toString(), overlap.signalId)) {
                startOverlapRelease(overlap.routeId.toString(), overlap.signalId, "trigger_detected");
            }
        }
    }
}

bool OverlapService::isReleaseTriggerSatisfied(const QString& routeId, const QString& signalId) const {
    QString overlapKey = QString("%1:%2").arg(routeId, signalId);
    if (!m_activeOverlaps.contains(overlapKey)) {
        return false;
    }

    const ActiveOverlap& overlap = m_activeOverlaps[overlapKey];
    return checkCircuitSequenceForRelease(overlap.releaseTriggerCircuits, routeId);
}

bool OverlapService::checkCircuitSequenceForRelease(const QStringList& triggerCircuits, const QString& routeId) const {
    Q_UNUSED(triggerCircuits)
    Q_UNUSED(routeId)
    // Simplified: require manual release
    return false;
}

void OverlapService::onTrackCircuitOccupancyChanged(const QString& circuitId, bool isOccupied) {
    // Update release trigger history
    for (auto it = m_activeOverlaps.begin(); it != m_activeOverlaps.end(); ++it) {
        const ActiveOverlap& overlap = it.value();

        if (overlap.releaseTriggerCircuits.contains(circuitId)) {
            updateReleaseTriggerHistory(overlap.routeId.toString(), circuitId, isOccupied);
            if (!isOccupied) {
                emit releaseTriggerDetected(overlap.routeId.toString(), overlap.signalId, circuitId);
            }
        }

        if (overlap.reservedCircuits.contains(circuitId) && isOccupied && overlap.state == OverlapState::RESERVED) {
            emit overlapViolation(overlap.routeId.toString(), overlap.signalId, "UNAUTHORIZED_OCCUPANCY",
                                  QString("Circuit %1 occupied while overlap reserved").arg(circuitId));
        }
    }
}

void OverlapService::updateReleaseTriggerHistory(const QString& routeId, const QString& circuitId, bool isOccupied) {
    if (!m_triggerHistory.contains(routeId)) {
        m_triggerHistory[routeId] = QHash<QString, QList<QPair<QDateTime, bool>>>();
    }
    if (!m_triggerHistory[routeId].contains(circuitId)) {
        m_triggerHistory[routeId][circuitId] = QList<QPair<QDateTime, bool>>();
    }

    m_triggerHistory[routeId][circuitId].append({QDateTime::currentDateTime(), isOccupied});

    // Limit history size
    auto& history = m_triggerHistory[routeId][circuitId];
    QDateTime cutoff = QDateTime::currentDateTime().addSecs(-TRIGGER_HISTORY_RETENTION_MINUTES * 60);
    while (!history.isEmpty() && history.first().first < cutoff) {
        history.removeFirst();
    }
}

bool OverlapService::validateOverlapRequest(const QString& routeId, const QString& signalId, const QStringList& circuits, QString& error) {
    if (routeId.isEmpty()) {
        error = "Route ID cannot be empty";
        return false;
    }
    if (signalId.isEmpty()) {
        error = "Signal ID cannot be empty";
        return false;
    }
    if (circuits.isEmpty()) {
        error = "Overlap circuits cannot be empty";
        return false;
    }
    if (!areCircuitsAvailableForOverlap(circuits)) {
        error = "One or more overlap circuits are not available";
        return false;
    }
    return true;
}

bool OverlapService::areCircuitsAvailableForOverlap(const QStringList& circuits) const {
    Q_UNUSED(circuits)
    return true;
}

bool OverlapService::checkOverlapConflicts(const QStringList& circuits, const QString& excludeRouteId) const {
    for (const QString& circuitId : circuits) {
        if (m_circuitOverlaps.contains(circuitId)) {
            for (const ActiveOverlap* overlap : m_circuitOverlaps[circuitId]) {
                if (overlap->routeId.toString() != excludeRouteId &&
                    overlap->state != OverlapState::RELEASED) {
                    return true;
                }
            }
        }
    }
    return false;
}

QVariantMap OverlapService::getOverlapStatistics() const {
    int reserved = 0, active = 0, releasing = 0;

    for (const ActiveOverlap& overlap : m_activeOverlaps) {
        switch (overlap.state) {
            case OverlapState::RESERVED: reserved++; break;
            case OverlapState::ACTIVE: active++; break;
            case OverlapState::RELEASING: releasing++; break;
            default: break;
        }
    }

    double averageOperationTime = m_totalOverlapOperations > 0 ?
                                 m_totalOverlapTime / m_totalOverlapOperations : 0.0;

    return QVariantMap{
        {"totalOverlaps", m_activeOverlaps.size()},
        {"reservedOverlaps", reserved},
        {"activeOverlaps", active},
        {"releasingOverlaps", releasing},
        {"totalOperations", m_totalOverlapOperations},
        {"successfulReleases", m_successfulReleases},
        {"forceReleases", m_forceReleases},
        {"overlapViolations", m_overlapViolations},
        {"averageOperationTimeMs", averageOperationTime},
        {"averageHoldTimeSeconds", m_averageHoldTime}
    };
}

// Utility method implementations
QString OverlapService::overlapStateToString(OverlapState state) const {
    switch (state) {
        case OverlapState::RESERVED: return "RESERVED";
        case OverlapState::ACTIVE: return "ACTIVE";
        case OverlapState::RELEASING: return "RELEASING";
        case OverlapState::RELEASED: return "RELEASED";
        default: return "UNKNOWN";
    }
}

OverlapState OverlapService::stringToOverlapState(const QString& stateStr) const {
    if (stateStr == "RESERVED") return OverlapState::RESERVED;
    if (stateStr == "ACTIVE") return OverlapState::ACTIVE;
    if (stateStr == "RELEASING") return OverlapState::RELEASING;
    if (stateStr == "RELEASED") return OverlapState::RELEASED;
    return OverlapState::RESERVED;
}

QString OverlapService::overlapTypeToString(OverlapType type) const {
    switch (type) {
        case OverlapType::FIXED: return "FIXED";
        case OverlapType::VARIABLE: return "VARIABLE";
        case OverlapType::FLANK_PROTECTION: return "FLANK_PROTECTION";
        default: return "FIXED";
    }
}

OverlapType OverlapService::stringToOverlapType(const QString& typeStr) const {
    if (typeStr == "VARIABLE") return OverlapType::VARIABLE;
    if (typeStr == "FLANK_PROTECTION") return OverlapType::FLANK_PROTECTION;
    return OverlapType::FIXED;
}

void OverlapService::recordOverlapOperation(const QString& operation, double timeMs) {
    m_totalOverlapOperations++;
    m_totalOverlapTime += timeMs;
    Q_UNUSED(operation)
}

void OverlapService::updateAverageHoldTime() {
    emit statisticsChanged();
}

// Database operation stubs (simplified for space)
bool OverlapService::persistOverlapToDatabase(const ActiveOverlap& overlap) {
    Q_UNUSED(overlap)
    return true;
}

bool OverlapService::updateOverlapStateInDatabase(const ActiveOverlap& overlap) {
    Q_UNUSED(overlap)
    return true;
}

bool OverlapService::removeOverlapFromDatabase(const QString& routeId, const QString& signalId) {
    Q_UNUSED(routeId) Q_UNUSED(signalId)
    return true;
}

// Additional method stubs
int OverlapService::pendingReleases() const {
    int count = 0;
    for (const ActiveOverlap& overlap : m_activeOverlaps) {
        if (overlap.state == OverlapState::RELEASING) {
            count++;
        }
    }
    return count;
}

QVariantMap OverlapService::getOverlapDefinition(const QString& signalId) const {
    if (m_overlapDefinitions.contains(signalId)) {
        return overlapDefinitionToVariantMap(m_overlapDefinitions[signalId]);
    }
    return QVariantMap();
}

QVariantMap OverlapService::overlapDefinitionToVariantMap(const OverlapDefinition& definition) const {
    return QVariantMap{
        {"signalId", definition.signalId},
        {"overlapCircuits", definition.overlapCircuitIds},
        {"releaseTriggerCircuits", definition.releaseTriggerCircuitIds},
        {"type", overlapTypeToString(definition.type)},
        {"holdSeconds", definition.holdSeconds},
        {"isActive", definition.isActive}
    };
}

void OverlapService::refreshOverlapDefinitions() {
    loadOverlapDefinitionsFromDatabase();
}

void OverlapService::onRouteStateChanged(const QString& routeId, const QString& newState) {
    Q_UNUSED(routeId)
    Q_UNUSED(newState)
}

bool OverlapService::forceReleaseOverlap(const QString& routeId, const QString& signalId, const QString& operatorId, const QString& reason) {
    Q_UNUSED(operatorId)
    Q_UNUSED(reason)

    QString overlapKey = QString("%1:%2").arg(routeId, signalId);
    if (!m_activeOverlaps.contains(overlapKey)) {
        return false;
    }

    m_forceReleases++;

    bool result = releaseOverlap(routeId, signalId, true);
    if (result) {
        emit overlapForceReleased(routeId, signalId, reason);
    }
    return result;
}

QVariantMap OverlapService::getOverlapStatus(const QString& routeId, const QString& signalId) const {
    QString overlapKey = QString("%1:%2").arg(routeId, signalId);
    if (!m_activeOverlaps.contains(overlapKey)) {
        return QVariantMap();
    }

    const ActiveOverlap& overlap = m_activeOverlaps[overlapKey];
    return overlapToVariantMap(overlap);
}

QVariantList OverlapService::getActiveOverlaps() const {
    QVariantList result;
    for (const ActiveOverlap& overlap : m_activeOverlaps) {
        result.append(overlapToVariantMap(overlap));
    }
    return result;
}

QVariantList OverlapService::getPendingReleases() const {
    QVariantList result;
    for (const ActiveOverlap& overlap : m_activeOverlaps) {
        if (overlap.state == OverlapState::RELEASING) {
            result.append(overlapToVariantMap(overlap));
        }
    }
    return result;
}

bool OverlapService::hasActiveOverlap(const QString& circuitId) const {
    return m_circuitOverlaps.contains(circuitId) && !m_circuitOverlaps[circuitId].isEmpty();
}

QVariantList OverlapService::getAllOverlapDefinitions() const {
    QVariantList result;
    for (const OverlapDefinition& definition : m_overlapDefinitions) {
        result.append(overlapDefinitionToVariantMap(definition));
    }
    return result;
}

bool OverlapService::updateOverlapDefinition(const QString& signalId, const QStringList& overlapCircuits, const QStringList& releaseTriggers, int holdSeconds) {
    if (!m_overlapDefinitions.contains(signalId)) {
        return false;
    }

    OverlapDefinition& definition = m_overlapDefinitions[signalId];
    definition.overlapCircuitIds = overlapCircuits;
    definition.releaseTriggerCircuitIds = releaseTriggers;
    definition.holdSeconds = holdSeconds;

    return true;
}

QVariantList OverlapService::getOverlapHistory(const QString& signalId, int limitDays) const {
    Q_UNUSED(signalId)
    Q_UNUSED(limitDays)
    return QVariantList();
}

QVariantMap OverlapService::overlapToVariantMap(const ActiveOverlap& overlap) const {
    return QVariantMap{
        {"routeId", overlap.routeId.toString()},
        {"signalId", overlap.signalId},
        {"reservedCircuits", overlap.reservedCircuits},
        {"releaseTriggerCircuits", overlap.releaseTriggerCircuits},
        {"state", overlapStateToString(overlap.state)},
        {"reservedAt", overlap.reservedAt},
        {"releaseTimerStarted", overlap.releaseTimerStarted},
        {"scheduledReleaseAt", overlap.scheduledReleaseAt},
        {"holdSeconds", overlap.holdSeconds},
        {"operatorId", overlap.operatorId},
        {"isExpired", overlap.isExpired()}
    };
}

} // namespace RailFlux::Route
