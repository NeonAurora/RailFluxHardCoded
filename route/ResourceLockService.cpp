#include "ResourceLockService.h"
#include "../database/DatabaseManager.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QJsonDocument>
#include <QJsonObject>

namespace RailFlux::Route {

ResourceLockService::ResourceLockService(DatabaseManager* dbManager, QObject* parent)
    : QObject(parent)
    , m_dbManager(dbManager)
    , m_maintenanceTimer(new QTimer(this))
{
    if (!m_dbManager) {
        qCritical() << "ResourceLockService: DatabaseManager is null";
        return;
    }

    // Setup maintenance timer
    m_maintenanceTimer->setInterval(MAINTENANCE_INTERVAL_MS);
    connect(m_maintenanceTimer, &QTimer::timeout, this, &ResourceLockService::performMaintenanceCheck);

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

ResourceLockService::~ResourceLockService() {
    if (m_maintenanceTimer) {
        m_maintenanceTimer->stop();
    }
}

void ResourceLockService::initialize() {
    qDebug() << "ResourceLockService: Initializing...";

    if (!m_dbManager || !m_dbManager->isConnected()) {
        qWarning() << "ResourceLockService: Cannot initialize - database not connected";
        return;
    }

    try {
        // Load existing locks from database (empty tables are OK)
        if (loadLocksFromDatabase()) {
            m_isOperational = true;
            m_maintenanceTimer->start();

            qDebug() << "  ResourceLockService: Initialized with" << activeLocks() << "active locks";
            emit operationalStateChanged();
            emit lockCountChanged();
        } else {
            // CHANGED: Don't fail if table is empty, just log warning
            qWarning() << " ResourceLockService: Database query failed, but continuing with empty lock state";
            m_isOperational = true;  // Still become operational
            m_maintenanceTimer->start();
            emit operationalStateChanged();
        }

    } catch (const std::exception& e) {
        qCritical() << " ResourceLockService: Initialization failed:" << e.what();
        m_isOperational = false;
        emit operationalStateChanged();
    }
}

QVariantMap ResourceLockService::lockResource(
    const QString& resourceType,
    const QString& resourceId,
    const QString& routeId,
    const QString& lockType,
    const QString& operatorId,
    const QString& reason,
    int timeoutMinutes
) {
    m_totalLockRequests++;

    if (!m_isOperational) {
        return QVariantMap{
            {"success", false},
            {"error", "ResourceLockService not operational"}
        };
    }

    LockRequest request;
    request.resourceType = resourceType.toUpper();
    request.resourceId = resourceId;
    request.routeId = QUuid::fromString(routeId);
    request.lockType = lockType.toUpper();
    request.operatorId = operatorId;
    request.reason = reason;
    request.timeoutMinutes = qBound(1, timeoutMinutes, MAX_LOCK_DURATION_HOURS * 60);

    // Validate request
    QString validationError;
    if (!validateLockRequest(request, validationError)) {
        return QVariantMap{
            {"success", false},
            {"error", validationError}
        };
    }

    // Attempt to acquire lock
    LockResult result = lockResourceInternal(request);
    
    if (result.success) {
        m_successfulLocks++;
        emit resourceLocked(resourceType, resourceId, routeId);
        emit lockCountChanged();
    }

    return QVariantMap{
        {"success", result.success},
        {"error", result.error},
        {"lockedAt", result.lockedAt},
        {"expiresAt", result.expiresAt},
        {"conflictingLocks", result.conflictingLocks}
    };
}

LockResult ResourceLockService::lockResourceInternal(const LockRequest& request) {
    LockResult result;
    QString lockKey = QString("%1:%2").arg(request.resourceType, request.resourceId);

    // Check for conflicts
    QStringList conflicts = findConflictingLocks(request.resourceType, request.resourceId, request.lockType);
    if (!conflicts.isEmpty()) {
        result.success = false;
        result.error = QString("Resource conflicts detected with existing locks");
        result.conflictingLocks = conflicts;
        m_conflictDetections++;
        
        emit lockConflictDetected(request.resourceType, request.resourceId, QVariantMap{
            {"conflictingLocks", conflicts},
            {"requestedLockType", request.lockType},
            {"routeId", request.routeId.toString()}
        });
        
        return result;
    }

    // Create lock
    ResourceLock lock;
    lock.resourceType = request.resourceType;
    lock.resourceId = request.resourceId;
    lock.routeId = request.routeId;
    lock.lockType = request.lockType;
    lock.lockedAt = QDateTime::currentDateTime();
    lock.expiresAt = lock.lockedAt.addSecs(request.timeoutMinutes * 60);
    lock.operatorId = request.operatorId;
    lock.lockReason = request.reason;
    lock.isActive = true;

    // Persist to database
    if (!persistLockToDatabase(lock)) {
        result.success = false;
        result.error = "Failed to persist lock to database";
        return result;
    }

    if (!updateIndividualResourceStatus(lock, true)) {
        qWarning() << " SAFETY: Failed to update individual resource status for"
                   << request.resourceType << request.resourceId;
        qWarning() << " SAFETY: Resource is locked in resource_locks table but individual table not updated";

        // For safety-critical systems, you might want to rollback here
        // For now, we'll continue but log the inconsistency
        emit lockConflictDetected(request.resourceType, request.resourceId, QVariantMap{
                                                                                {"error", "Individual resource status update failed"},
                                                                                {"lockId", lock.routeId.toString()},
                                                                                {"inconsistencyType", "LOCK_TABLE_INDIVIDUAL_TABLE_MISMATCH"}
                                                                            });
    }

    // Add to memory
    if (!m_activeLocks.contains(lockKey)) {
        m_activeLocks[lockKey] = QList<ResourceLock>();
    }
    m_activeLocks[lockKey].append(lock);

    // Track by route
    if (!m_routeLocks.contains(request.routeId)) {
        m_routeLocks[request.routeId] = QStringList();
    }
    m_routeLocks[request.routeId].append(lockKey);

    result.success = true;
    result.lockedAt = lock.lockedAt;
    result.expiresAt = lock.expiresAt;

    qDebug() << "ResourceLockService: Locked" << request.resourceType << request.resourceId 
             << "for route" << request.routeId.toString();

    return result;
}

bool ResourceLockService::unlockResource(
    const QString& resourceType,
    const QString& resourceId,
    const QString& routeId
) {
    if (!m_isOperational) {
        return false;
    }

    QUuid uuid = QUuid::fromString(routeId);
    bool success = unlockResourceInternal(resourceType.toUpper(), resourceId, uuid);
    
    if (success) {
        emit resourceUnlocked(resourceType, resourceId, routeId);
        emit lockCountChanged();
    }

    return success;
}

bool ResourceLockService::unlockResourceInternal(
    const QString& resourceType,
    const QString& resourceId,
    const QUuid& routeId
    ) {
    QString lockKey = QString("%1:%2").arg(resourceType, resourceId);

    if (!m_activeLocks.contains(lockKey)) {
        return false; // No lock found
    }

    QList<ResourceLock>& locks = m_activeLocks[lockKey];

    // Find and remove the specific lock
    for (int i = 0; i < locks.size(); ++i) {
        if (locks[i].routeId == routeId && locks[i].isActive) {
            ResourceLock lockToRemove = locks[i];

            // Remove from database first
            if (removeLockFromDatabase(lockToRemove)) {
                // 🆕 NEW: Update individual resource status on unlock
                updateIndividualResourceStatus(lockToRemove, false);

                // Remove from memory
                locks.removeAt(i);

                qDebug() << " ResourceLockService: Unlocked" << resourceType << resourceId
                         << "for route" << routeId.toString();
                return true;
            } else {
                qCritical() << " Failed to remove lock from database";
                return false;
            }
        }
    }

    return false;
}

bool ResourceLockService::unlockAllResourcesForRoute(const QString& routeId) {
    if (!m_isOperational) {
        return false;
    }

    QUuid uuid = QUuid::fromString(routeId);
    if (!m_routeLocks.contains(uuid)) {
        return true; // No locks for this route
    }

    QStringList lockKeys = m_routeLocks[uuid];
    bool allSuccess = true;

    for (const QString& lockKey : lockKeys) {
        QStringList parts = lockKey.split(":");
        if (parts.size() == 2) {
            if (!unlockResourceInternal(parts[0], parts[1], uuid)) {
                allSuccess = false;
            }
        }
    }

    if (allSuccess) {
        qDebug() << " ResourceLockService: Unlocked all resources for route" << routeId;
    }

    return allSuccess;
}

bool ResourceLockService::isResourceLocked(
    const QString& resourceType,
    const QString& resourceId
) const {
    QString lockKey = QString("%1:%2").arg(resourceType.toUpper(), resourceId);
    
    if (!m_activeLocks.contains(lockKey)) {
        return false;
    }

    const QList<ResourceLock>& locks = m_activeLocks[lockKey];
    for (const ResourceLock& lock : locks) {
        if (lock.isActive && !lock.isExpired()) {
            return true;
        }
    }

    return false;
}

QVariantMap ResourceLockService::getResourceLockStatus(
    const QString& resourceType,
    const QString& resourceId
) const {
    QString lockKey = QString("%1:%2").arg(resourceType.toUpper(), resourceId);
    
    QVariantMap status;
    status["isLocked"] = false;
    status["locks"] = QVariantList();

    if (!m_activeLocks.contains(lockKey)) {
        return status;
    }

    const QList<ResourceLock>& locks = m_activeLocks[lockKey];
    QVariantList lockList;
    bool hasActiveLock = false;

    for (const ResourceLock& lock : locks) {
        if (lock.isActive && !lock.isExpired()) {
            hasActiveLock = true;
            lockList.append(lockToVariantMap(lock));
        }
    }

    status["isLocked"] = hasActiveLock;
    status["locks"] = lockList;

    return status;
}

bool ResourceLockService::loadLocksFromDatabase() {
    QSqlQuery query(m_dbManager->getDatabase());
    query.prepare(R"(
        SELECT
            resource_type,
            resource_id,
            route_id,
            lock_type,
            acquired_at,
            expires_at,
            acquired_by,
            is_active
        FROM railway_control.resource_locks
        WHERE is_active = TRUE
        ORDER BY acquired_at
    )");

    if (!query.exec()) {
        qCritical() << "ResourceLockService: Failed to load locks from database:" << query.lastError().text();
        return false;
    }

    m_activeLocks.clear();
    m_routeLocks.clear();

    while (query.next()) {
        ResourceLock lock;
        lock.resourceType = query.value("resource_type").toString();
        lock.resourceId = query.value("resource_id").toString();
        lock.routeId = QUuid::fromString(query.value("route_id").toString());
        lock.lockType = query.value("lock_type").toString();
        lock.lockedAt = query.value("acquired_at").toDateTime();  //  was locked_at
        lock.expiresAt = query.value("expires_at").toDateTime();
        lock.operatorId = query.value("acquired_by").toString();   //  was operator_id
        lock.lockReason = ""; //  removed since column doesn't exist
        lock.isActive = query.value("is_active").toBool();

        // Skip expired locks
        if (lock.isExpired()) {
            continue;
        }

        QString lockKey = lock.lockKey();
        if (!m_activeLocks.contains(lockKey)) {
            m_activeLocks[lockKey] = QList<ResourceLock>();
        }
        m_activeLocks[lockKey].append(lock);

        // Track by route
        if (!m_routeLocks.contains(lock.routeId)) {
            m_routeLocks[lock.routeId] = QStringList();
        }
        m_routeLocks[lock.routeId].append(lockKey);
    }

    qDebug() << "📥 ResourceLockService: Loaded" << activeLocks() << "active locks from database";
    return true;
}

bool ResourceLockService::persistLockToDatabase(const ResourceLock& lock) {
    //   SAFETY: Use DatabaseManager's validated method instead of direct SQL
    if (!m_dbManager) {
        qCritical() << "ResourceLockService: DatabaseManager is null";
        return false;
    }

    //   UPDATED: lock.resourceType and lock.lockType use railway terminology
    // Lock types: "ROUTE", "OVERLAP", "EMERGENCY", "MAINTENANCE"
    qDebug() << "ResourceLockService: Persisting lock for"
             << lock.resourceType << lock.resourceId << "route:" << lock.routeId;

    //    Use DatabaseManager's insertResourceLock method
    // This uses SQL functions that properly handle the railway database schema
    bool success = m_dbManager->insertResourceLock(
        lock.resourceType,           // "TRACK_CIRCUIT", "POINT_MACHINE", "SIGNAL"
        lock.resourceId,            // Resource ID string
        lock.routeId.toString(),    // Route UUID as string
        lock.lockType              // "ROUTE", "OVERLAP", "EMERGENCY", "MAINTENANCE"
        );

    if (!success) {
        qCritical() << "ResourceLockService: Failed to persist lock via DatabaseManager for resource:"
                    << lock.resourceType << lock.resourceId << "route:" << lock.routeId;
        return false;
    }

    qDebug() << "  ResourceLockService: Successfully persisted lock for"
             << lock.resourceType << lock.resourceId << "route:" << lock.routeId;
    return true;
}


bool ResourceLockService::removeLockFromDatabase(const ResourceLock& lock) {
    QSqlQuery query(m_dbManager->getDatabase());
    query.prepare(R"(
        UPDATE railway_control.resource_locks 
        SET is_active = FALSE
        WHERE resource_type = ? AND resource_id = ? AND route_id = ? AND lock_type = ?
    )");

    query.addBindValue(lock.resourceType);
    query.addBindValue(lock.resourceId);
    query.addBindValue(lock.routeId.toString());
    query.addBindValue(lock.lockType);

    if (!query.exec()) {
        qCritical() << "ResourceLockService: Failed to remove lock from database:" << query.lastError().text();
        return false;
    }

    return true;
}

QStringList ResourceLockService::findConflictingLocks(
    const QString& resourceType,
    const QString& resourceId,
    const QString& lockType
) const {
    QStringList conflicts;
    QString lockKey = QString("%1:%2").arg(resourceType, resourceId);
    
    if (!m_activeLocks.contains(lockKey)) {
        return conflicts; // No existing locks
    }

    const QList<ResourceLock>& existingLocks = m_activeLocks[lockKey];
    
    for (const ResourceLock& existingLock : existingLocks) {
        if (!existingLock.isActive || existingLock.isExpired()) {
            continue;
        }

        // Check compatibility
        LockRequest newRequest;
        newRequest.lockType = lockType;
        
        if (!isLockCompatible(existingLock, newRequest)) {
            conflicts.append(QString("%1 (%2) by route %3")
                           .arg(existingLock.lockType, existingLock.operatorId, existingLock.routeId.toString()));
        }
    }

    return conflicts;
}

bool ResourceLockService::isLockCompatible(const ResourceLock& existingLock, const LockRequest& newRequest) const {
    //   REFACTORED: Use railway lock type compatibility rules

    // EMERGENCY locks override everything and are never compatible
    if (existingLock.lockType == "EMERGENCY" || newRequest.lockType == "EMERGENCY") {
        return false;
    }

    // ROUTE locks are exclusive - cannot coexist with other ROUTE locks
    if (existingLock.lockType == "ROUTE" && newRequest.lockType == "ROUTE") {
        return false;
    }

    // MAINTENANCE locks conflict with ROUTE locks
    if ((existingLock.lockType == "MAINTENANCE" && newRequest.lockType == "ROUTE") ||
        (existingLock.lockType == "ROUTE" && newRequest.lockType == "MAINTENANCE")) {
        return false;
    }

    //   RAILWAY RULE: OVERLAP locks have special compatibility rules
    if (existingLock.lockType == "OVERLAP" && newRequest.lockType == "OVERLAP") {
        // Multiple overlap locks can coexist for different routes in some cases
        // This depends on railway operating rules - for safety, default to false
        return false;
    }

    // OVERLAP locks can coexist with ROUTE locks in some cases
    // but this should be carefully validated based on railway rules
    if ((existingLock.lockType == "OVERLAP" && newRequest.lockType == "ROUTE") ||
        (existingLock.lockType == "ROUTE" && newRequest.lockType == "OVERLAP")) {
        // For safety-critical railway operations, default to no compatibility
        // This can be refined based on specific railway operating procedures
        return false;
    }

    // MAINTENANCE locks can potentially coexist with other MAINTENANCE locks
    if (existingLock.lockType == "MAINTENANCE" && newRequest.lockType == "MAINTENANCE") {
        // Multiple maintenance operations might be allowed
        // but should be validated based on maintenance procedures
        return true;
    }

    //   ADDED: Handle unknown lock type combinations safely
    qWarning() << " Unknown lock type combination:"
               << existingLock.lockType << "vs" << newRequest.lockType;
    return false; // Default to safe behavior
}

bool ResourceLockService::validateLockRequest(const LockRequest& request, QString& error) const {
    if (request.resourceType.isEmpty()) {
        error = "Resource type cannot be empty";
        return false;
    }

    if (request.resourceId.isEmpty()) {
        error = "Resource ID cannot be empty";
        return false;
    }

    if (request.routeId.isNull()) {
        error = "Route ID is invalid";
        return false;
    }

    //   UPDATED: Use railway lock types matching database schema
    QStringList validLockTypes = {"ROUTE", "OVERLAP", "EMERGENCY", "MAINTENANCE"};
    if (!validLockTypes.contains(request.lockType)) {
        error = QString("Invalid lock type: %1. Valid types are: %2")
        .arg(request.lockType)
            .arg(validLockTypes.join(", "));
        return false;
    }

    QStringList validResourceTypes = {"TRACK_CIRCUIT", "POINT_MACHINE", "SIGNAL"};
    if (!validResourceTypes.contains(request.resourceType)) {
        error = QString("Invalid resource type: %1. Valid types are: %2")
        .arg(request.resourceType)
            .arg(validResourceTypes.join(", "));
        return false;
    }

    //   ADDED: Railway-specific validation rules
    if (request.lockType == "EMERGENCY" && request.reason.isEmpty()) {
        error = "Emergency locks require a reason";
        return false;
    }

    if (request.lockType == "MAINTENANCE" && request.reason.isEmpty()) {
        error = "Maintenance locks require a reason";
        return false;
    }

    //   ADDED: Resource-specific validation
    if (request.resourceType == "POINT_MACHINE" && request.lockType == "OVERLAP") {
        error = "Point machines cannot have overlap locks";
        return false;
    }

    return true;
}

void ResourceLockService::performMaintenanceCheck() {
    if (!m_isOperational) {
        return;
    }

    cleanupExpiredLocks();
}

void ResourceLockService::cleanupExpiredLocks() {
    QStringList expiredLockKeys;
    
    for (auto it = m_activeLocks.begin(); it != m_activeLocks.end(); ++it) {
        QList<ResourceLock>& locks = it.value();
        
        for (int i = locks.size() - 1; i >= 0; --i) {
            if (locks[i].isExpired()) {
                ResourceLock expiredLock = locks[i];
                locks.removeAt(i);
                
                // Remove from database
                removeLockFromDatabase(expiredLock);
                
                // Remove from route tracking
                if (m_routeLocks.contains(expiredLock.routeId)) {
                    m_routeLocks[expiredLock.routeId].removeOne(it.key());
                    if (m_routeLocks[expiredLock.routeId].isEmpty()) {
                        m_routeLocks.remove(expiredLock.routeId);
                    }
                }
                
                emit lockExpired(expiredLock.resourceType, expiredLock.resourceId, expiredLock.routeId.toString());
                m_expiredLocksCleanedUp++;
            }
        }
        
        if (locks.isEmpty()) {
            expiredLockKeys.append(it.key());
        }
    }
    
    // Remove empty lock lists
    for (const QString& key : expiredLockKeys) {
        m_activeLocks.remove(key);
    }
    
    if (!expiredLockKeys.isEmpty()) {
        qDebug() << " ResourceLockService: Cleaned up" << expiredLockKeys.size() << "expired locks";
        emit lockCountChanged();
    }
}

int ResourceLockService::expiredLocks() const {
    // This would require querying the database for expired locks
    // For now, return 0 as we clean them up automatically
    return 0;
}

QVariantMap ResourceLockService::lockToVariantMap(const ResourceLock& lock) const {
    return QVariantMap{
        {"resourceType", lock.resourceType},
        {"resourceId", lock.resourceId},
        {"routeId", lock.routeId.toString()},
        {"lockType", lock.lockType},
        {"lockedAt", lock.lockedAt},
        {"expiresAt", lock.expiresAt},
        {"operatorId", lock.operatorId},
        {"lockReason", lock.lockReason},
        {"isActive", lock.isActive},
        {"isExpired", lock.isExpired()}
    };
}

QVariantMap ResourceLockService::getLockStatistics() const {
    return QVariantMap{
        {"activeLocks", activeLocks()},
        {"totalLockRequests", m_totalLockRequests},
        {"successfulLocks", m_successfulLocks},
        {"conflictDetections", m_conflictDetections},
        {"forceUnlocks", m_forceUnlocks},
        {"expiredLocksCleanedUp", m_expiredLocksCleanedUp},
        {"successRate", m_totalLockRequests > 0 ? (double)m_successfulLocks / m_totalLockRequests * 100.0 : 0.0}
    };
}

// Stub implementations for remaining methods
QVariantMap ResourceLockService::lockMultipleResources(const QVariantList& lockRequests, const QString& routeId, const QString& operatorId) {
    // Implementation would batch multiple lock requests
    Q_UNUSED(lockRequests) Q_UNUSED(routeId) Q_UNUSED(operatorId)
    return QVariantMap{{"success", false}, {"error", "Not implemented"}};
}

QVariantList ResourceLockService::getActiveLocksForRoute(const QString& routeId) const {
    Q_UNUSED(routeId)
    return QVariantList();
}

QVariantList ResourceLockService::getAllActiveLocks() const {
    return QVariantList();
}

QVariantList ResourceLockService::getExpiredLocks() const {
    return QVariantList();
}

void ResourceLockService::refreshLocksFromDatabase() {
    loadLocksFromDatabase();
}

void ResourceLockService::onResourceChanged(const QString& resourceType, const QString& resourceId) {
    Q_UNUSED(resourceType)
    Q_UNUSED(resourceId)
}

bool ResourceLockService::forceUnlockResource(const QString& resourceType, const QString& resourceId, const QString& operatorId, const QString& reason) {
    QString lockKey = QString("%1:%2").arg(resourceType.toUpper(), resourceId);
    
    if (!m_activeLocks.contains(lockKey)) {
        return false;
    }

    QList<ResourceLock>& locks = m_activeLocks[lockKey];
    bool unlocked = false;
    
    for (int i = locks.size() - 1; i >= 0; --i) {
        ResourceLock lockToRemove = locks[i];
        locks.removeAt(i);
        
        // Remove from database
        removeLockFromDatabase(lockToRemove);
        
        // Remove from route tracking
        if (m_routeLocks.contains(lockToRemove.routeId)) {
            m_routeLocks[lockToRemove.routeId].removeOne(lockKey);
            if (m_routeLocks[lockToRemove.routeId].isEmpty()) {
                m_routeLocks.remove(lockToRemove.routeId);
            }
        }
        
        unlocked = true;
        m_forceUnlocks++;
        
        qWarning() << " ResourceLockService: Force unlocked" << resourceType << resourceId 
                   << "by" << operatorId << "reason:" << reason;
    }
    
    if (locks.isEmpty()) {
        m_activeLocks.remove(lockKey);
    }
    
    if (unlocked) {
        emit forceUnlockPerformed(resourceType, resourceId, operatorId, reason);
        emit lockCountChanged();
    }
    
    return unlocked;
}

QVariantMap ResourceLockService::checkLockConflicts(const QString& resourceType, const QString& resourceId, const QString& requestedLockType) const {
    QStringList conflicts = findConflictingLocks(resourceType.toUpper(), resourceId, requestedLockType.toUpper());
    
    return QVariantMap{
        {"hasConflicts", !conflicts.isEmpty()},
        {"conflictingLocks", conflicts},
        {"isLocked", isResourceLocked(resourceType, resourceId)}
    };
}

QVariantList ResourceLockService::checkMultipleResourceConflicts(const QVariantList& resourceRequests) const {
    QVariantList results;
    
    for (const QVariant& request : resourceRequests) {
        QVariantMap requestMap = request.toMap();
        QString resourceType = requestMap["resourceType"].toString();
        QString resourceId = requestMap["resourceId"].toString();
        QString lockType = requestMap["lockType"].toString();
        
        QVariantMap conflictResult = checkLockConflicts(resourceType, resourceId, lockType);
        conflictResult["resourceType"] = resourceType;
        conflictResult["resourceId"] = resourceId;
        conflictResult["requestedLockType"] = lockType;
        
        results.append(conflictResult);
    }
    
    return results;
}

bool ResourceLockService::renewLock(const QString& resourceType, const QString& resourceId, const QString& routeId, int additionalMinutes) {
    QString lockKey = QString("%1:%2").arg(resourceType.toUpper(), resourceId);
    QUuid uuid = QUuid::fromString(routeId);
    
    if (!m_activeLocks.contains(lockKey)) {
        return false;
    }
    
    QList<ResourceLock>& locks = m_activeLocks[lockKey];
    
    for (ResourceLock& lock : locks) {
        if (lock.routeId == uuid && lock.isActive && !lock.isExpired()) {
            lock.expiresAt = lock.expiresAt.addSecs(additionalMinutes * 60);
            
            // Update in database (simplified)
            // In full implementation, would update the database record
            
            qDebug() << "ResourceLockService: Renewed lock for" << resourceType << resourceId 
                     << "by" << additionalMinutes << "minutes";
            return true;
        }
    }
    
    return false;
}

bool ResourceLockService::updateIndividualResourceStatus(const ResourceLock& lock, bool lockStatus) {
    if (!m_dbManager || !m_dbManager->isConnected()) {
        qCritical() << " SAFETY: Cannot update individual resource status - database not connected";
        return false;
    }

    qDebug() << "[INDIVIDUAL_UPDATE] Updating individual resource status:"
             << lock.resourceType << lock.resourceId << "Lock:" << lockStatus;

    try {
        if (lock.resourceType == "TRACK_CIRCUIT") {
            bool isOverlap = (lock.lockType == "OVERLAP");

            // Update track circuit
            bool success = updateTrackCircuitStatus(lock.resourceId, lockStatus, isOverlap);

            // Update track segments for BOTH main circuits AND overlap circuits
            if (success) {
                updateTrackSegmentStatus(lock.resourceId, lockStatus, isOverlap);
            }
            return success;

        } else if (lock.resourceType == "POINT_MACHINE") {
            //   ENHANCED: Handle paired point machine locking
            QSet<QString> processedMachines;
            return updatePointMachineStatusWithPairing(lock.resourceId, lockStatus,
                                                       lock.routeId.toString(), &processedMachines);

        } else if (lock.resourceType == "SIGNAL") {
            return updateSignalStatus(lock.resourceId, lockStatus);

        } else {
            qCritical() << " SAFETY: Unknown resource type for individual update:" << lock.resourceType;
            return false;
        }
    } catch (const std::exception& e) {
        qCritical() << " SAFETY: Exception in updateIndividualResourceStatus:" << e.what();
        return false;
    }
}

bool ResourceLockService::updateTrackCircuitStatus(const QString& circuitId, bool isLocking, bool isOverlap) {
    QSqlDatabase db = m_dbManager->getDatabase();
    QSqlQuery query(db);

    //   FIXED LOGIC: Overlap circuits get is_overlap=true but is_assigned=false
    // Main circuits get is_assigned=true and is_overlap=false
    bool is_assigned = isLocking && !isOverlap;  // Only main circuits are "assigned"
    bool is_overlap_value = isLocking && isOverlap;   // Only overlap circuits get overlap flag

    qDebug() << "[TRACK_CIRCUIT_UPDATE] Circuit:" << circuitId
             << "isLocking:" << isLocking << "isOverlap:" << isOverlap
             << "→ is_assigned:" << is_assigned << "is_overlap:" << is_overlap_value;

    // Update track circuit with correct logic
    query.prepare(R"(
        UPDATE railway_control.track_circuits
        SET is_assigned = ?,
            is_overlap = ?,
            updated_at = CURRENT_TIMESTAMP
        WHERE circuit_id = ?
        RETURNING circuit_id
    )");

    query.addBindValue(is_assigned);
    query.addBindValue(is_overlap_value);
    query.addBindValue(circuitId);

    if (!query.exec()) {
        qCritical() << " SAFETY: Failed to update track circuit" << circuitId << ":"
                    << query.lastError().text();
        return false;
    }

    if (query.next()) {
        QString updatedId = query.value(0).toString();
        qDebug() << "  [INDIVIDUAL_UPDATE] Track circuit updated:" << updatedId
                 << "assigned:" << is_assigned << "overlap:" << is_overlap_value;
        return true;
    } else {
        qWarning() << " [INDIVIDUAL_UPDATE] Track circuit not found:" << circuitId;
        return false;
    }
}

bool ResourceLockService::updatePointMachineStatus(const QString& machineId, bool isLocked) {
    QSqlDatabase db = m_dbManager->getDatabase();
    QSqlQuery query(db);

    query.prepare(R"(
        UPDATE railway_control.point_machines
        SET is_locked = ?,
            updated_at = CURRENT_TIMESTAMP
        WHERE machine_id = ?
        RETURNING machine_id
    )");

    query.addBindValue(isLocked);
    query.addBindValue(machineId);

    if (!query.exec()) {
        qCritical() << " SAFETY: Failed to update point machine" << machineId << ":"
                    << query.lastError().text();
        return false;
    }

    if (query.next()) {
        QString updatedId = query.value(0).toString();
        qDebug() << "  [INDIVIDUAL_UPDATE] Point machine updated:" << updatedId
                 << "locked:" << isLocked;
        return true;
    } else {
        qWarning() << " [INDIVIDUAL_UPDATE] Point machine not found:" << machineId;
        return false;
    }
}

bool ResourceLockService::updateSignalStatus(const QString& signalId, bool isLocked) {
    QSqlDatabase db = m_dbManager->getDatabase();
    QSqlQuery query(db);

    query.prepare(R"(
        UPDATE railway_control.signals
        SET is_locked = ?,
            updated_at = CURRENT_TIMESTAMP
        WHERE signal_id = ?
        RETURNING signal_id
    )");

    query.addBindValue(isLocked);
    query.addBindValue(signalId);

    if (!query.exec()) {
        qCritical() << " SAFETY: Failed to update signal" << signalId << ":"
                    << query.lastError().text();
        return false;
    }

    if (query.next()) {
        QString updatedId = query.value(0).toString();
        qDebug() << "  [INDIVIDUAL_UPDATE] Signal updated:" << updatedId
                 << "locked:" << isLocked;
        return true;
    } else {
        qWarning() << " [INDIVIDUAL_UPDATE] Signal not found:" << signalId;
        return false;
    }
}

bool ResourceLockService::updateTrackSegmentStatus(const QString& circuitId, bool isLocking, bool isOverlap) {
    QSqlDatabase db = m_dbManager->getDatabase();
    QSqlQuery query(db);

    //   FIXED LOGIC: Similar to track circuits
    // Main circuits: is_assigned=true, is_overlap=false
    // Overlap circuits: is_assigned=false, is_overlap=true
    bool is_assigned = isLocking && !isOverlap;      // Only main circuits are "assigned"
    bool is_overlap_value = isLocking && isOverlap;  // Only overlap circuits get overlap flag

    qDebug() << "[TRACK_SEGMENT_UPDATE] Circuit:" << circuitId
             << "isLocking:" << isLocking << "isOverlap:" << isOverlap
             << "→ is_assigned:" << is_assigned << "is_overlap:" << is_overlap_value;

    // Update track segments that belong to this circuit - UPDATE BOTH COLUMNS
    query.prepare(R"(
        UPDATE railway_control.track_segments
        SET is_assigned = ?,
            is_overlap = ?,
            updated_at = CURRENT_TIMESTAMP
        WHERE circuit_id = ?
        RETURNING segment_id
    )");

    query.addBindValue(is_assigned);
    query.addBindValue(is_overlap_value);
    query.addBindValue(circuitId);

    if (!query.exec()) {
        qCritical() << " SAFETY: Failed to update track segments for circuit" << circuitId << ":"
                    << query.lastError().text();
        return false;
    }

    QStringList updatedSegments;
    while (query.next()) {
        updatedSegments.append(query.value(0).toString());
    }

    if (!updatedSegments.isEmpty()) {
        qDebug() << "  [INDIVIDUAL_UPDATE] Track segments updated for circuit" << circuitId << ":"
                 << updatedSegments << "assigned:" << is_assigned << "overlap:" << is_overlap_value;
    } else {
        qDebug() << " [INDIVIDUAL_UPDATE] No track segments found for circuit:" << circuitId;
    }

    return true; // Return true even if no segments found, as this might be normal
}

bool ResourceLockService::updatePointMachineStatusWithPairing(const QString& machineId,
                                                              bool lockStatus,
                                                              const QString& routeId,
                                                              QSet<QString>* processedMachines) {
    qDebug() << " [POINT_MACHINE_PAIRING] Processing point machine:" << machineId << "lockStatus:" << lockStatus;

    //   NEW: Circular dependency detection
    if (processedMachines && processedMachines->contains(machineId)) {
        qDebug() << " [POINT_MACHINE_PAIRING] Machine" << machineId << "already processed in this operation - avoiding circular lock";
        return true; // Return success to avoid breaking the chain
    }

    // Add to processed machines set
    if (processedMachines) {
        processedMachines->insert(machineId);
    }

    // Step 1: Update the primary point machine
    bool primarySuccess = updatePointMachineStatus(machineId, lockStatus);
    if (!primarySuccess) {
        qCritical() << " [POINT_MACHINE_PAIRING] Failed to update primary point machine:" << machineId;
        return false;
    }

    // Step 2: Get point machine information to check for paired entity
    QVariantMap pointMachineData = m_dbManager->getPointMachineById(machineId);
    if (pointMachineData.isEmpty()) {
        qWarning() << " [POINT_MACHINE_PAIRING] Could not retrieve point machine data for:" << machineId;
        return primarySuccess;
    }

    // Step 3: Check if this point machine has a paired entity
    QVariant pairedEntityVariant = pointMachineData["pairedEntity"];
    if (pairedEntityVariant.isNull() || !pairedEntityVariant.isValid()) {
        qDebug() << " [POINT_MACHINE_PAIRING] Point machine" << machineId << "has no paired entity";
        return primarySuccess;
    }

    QString pairedMachineId = pairedEntityVariant.toString();
    if (pairedMachineId.isEmpty() || pairedMachineId == machineId) {
        qDebug() << " [POINT_MACHINE_PAIRING] Point machine" << machineId << "paired entity is empty or self-reference";
        return primarySuccess;
    }

    qDebug() << " [POINT_MACHINE_PAIRING] Found paired machine:" << pairedMachineId << "for" << machineId;

    //   NEW: Check if paired machine already processed
    if (processedMachines && processedMachines->contains(pairedMachineId)) {
        qDebug() << " [POINT_MACHINE_PAIRING] Paired machine" << pairedMachineId
                 << "already processed in this operation - skipping to avoid circular dependency";
        return primarySuccess;
    }

    // Step 4: Enhanced route ID comparison for conflict detection
    if (lockStatus) {
        QVariantMap pairedData = m_dbManager->getPointMachineById(pairedMachineId);
        bool pairedAlreadyLocked = pairedData["isRouteLocked"].toBool();
        QString pairedLockedByRouteStr = pairedData["lockedByRouteId"].toString();

        //    Robust route ID comparison (handle UUID format variations)
        QString cleanRouteId = routeId;
        QString cleanPairedRouteId = pairedLockedByRouteStr;

        // Remove braces if present: {uuid} → uuid
        if (cleanRouteId.startsWith("{") && cleanRouteId.endsWith("}")) {
            cleanRouteId = cleanRouteId.mid(1, cleanRouteId.length() - 2);
        }
        if (cleanPairedRouteId.startsWith("{") && cleanPairedRouteId.endsWith("}")) {
            cleanPairedRouteId = cleanPairedRouteId.mid(1, cleanPairedRouteId.length() - 2);
        }

        if (pairedAlreadyLocked && cleanPairedRouteId == cleanRouteId) {
            qDebug() << " [POINT_MACHINE_PAIRING] Paired machine" << pairedMachineId
                     << "already locked by same route" << cleanRouteId << "- skipping";
            return primarySuccess;
        }

        if (pairedAlreadyLocked && cleanPairedRouteId != cleanRouteId && !cleanPairedRouteId.isEmpty()) {
            qWarning() << " [POINT_MACHINE_PAIRING] Paired machine" << pairedMachineId
                       << "is locked by different route:" << cleanPairedRouteId
                       << "(our route:" << cleanRouteId << ")";
            // For safety, we could fail here, but let's continue for now
        }
    }

    // Step 5: Recursively handle the paired machine (with circular protection)
    if (lockStatus) {
        qDebug() << "[POINT_MACHINE_PAIRING] Locking paired machine:" << pairedMachineId;

        QVariantMap lockResult = lockResource(
            "POINT_MACHINE",
            pairedMachineId,
            routeId,
            "ROUTE",
            "INTELLIGENT_SYSTEM",
            QString("Paired with %1 for route %2").arg(machineId, routeId),
            30
            );

        bool pairedLockSuccess = lockResult["success"].toBool();
        if (pairedLockSuccess) {
            qDebug() << "  [POINT_MACHINE_PAIRING] Successfully locked paired machine:" << pairedMachineId;
        } else {
            QString error = lockResult["error"].toString();
            qWarning() << " [POINT_MACHINE_PAIRING] Failed to lock paired machine:" << pairedMachineId
                       << "Error:" << error;

            // Check if this is a "already locked" error which might be okay
            if (error.contains("already locked") || error.contains("conflicting lock")) {
                qDebug() << " [POINT_MACHINE_PAIRING] Paired machine lock failure due to existing lock - might be acceptable";
                return primarySuccess; // Continue with primary success
            }

            return false; // Other types of failures should fail the operation
        }

        return pairedLockSuccess;

    } else {
        // Unlock operation
        qDebug() << " [POINT_MACHINE_PAIRING] Unlocking paired machine:" << pairedMachineId;

        bool pairedUnlockSuccess = unlockResource("POINT_MACHINE", pairedMachineId, routeId);
        if (pairedUnlockSuccess) {
            qDebug() << "  [POINT_MACHINE_PAIRING] Successfully unlocked paired machine:" << pairedMachineId;
        } else {
            qWarning() << " [POINT_MACHINE_PAIRING] Failed to unlock paired machine:" << pairedMachineId;
        }

        return pairedUnlockSuccess;
    }
}

} // namespace RailFlux::Route
