#pragma once

#include <QObject>
#include <QVariantMap>
#include <QVariantList>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <QDateTime>
#include <QDebug>
#include <memory>
#include <QSqlQuery>

// Forward declaration
class DatabaseManager;

namespace RailFlux::Route {

enum class ResourceType {
    TRACK_CIRCUIT,
    POINT_MACHINE,
    SIGNAL
};

enum class LockType {
    ROUTE,    // Only one route can use this resource
    SHARED,       // Multiple routes can share (e.g., for read-only operations)
    OVERLAP       // Special lock for overlap regions
};

struct ResourceLock {
    QString resourceType;
    QString resourceId;
    QUuid routeId;
    QString lockType;
    QDateTime lockedAt;
    QDateTime expiresAt;
    QString operatorId;
    QString lockReason;
    bool isActive = true;
    
    bool isExpired() const { 
        return expiresAt.isValid() && QDateTime::currentDateTime() > expiresAt; 
    }
    
    QString lockKey() const {
        return QString("%1:%2").arg(resourceType, resourceId);
    }
};

struct LockRequest {
    QString resourceType;
    QString resourceId;
    QUuid routeId;
    QString lockType;
    QString operatorId;
    QString reason;
    int timeoutMinutes = 30; // Default timeout
};

struct LockResult {
    bool success = false;
    QString error;
    QDateTime lockedAt;
    QDateTime expiresAt;
    QStringList conflictingLocks;
};

class ResourceLockService : public QObject {
    Q_OBJECT
    Q_PROPERTY(int activeLocks READ activeLocks NOTIFY lockCountChanged)
    Q_PROPERTY(int expiredLocks READ expiredLocks NOTIFY lockCountChanged)
    Q_PROPERTY(bool isOperational READ isOperational NOTIFY operationalStateChanged)

public:

    struct ResourceStatusResult {
        bool success = false;
        QString error;
        QStringList updatedResources;
        int affectedRows = 0;
    };

    explicit ResourceLockService(DatabaseManager* dbManager, QObject* parent = nullptr);
    ~ResourceLockService();

    // Properties
    int activeLocks() const { return m_activeLocks.size(); }
    int expiredLocks() const;
    bool isOperational() const { return m_isOperational; }

    // Main locking API
    Q_INVOKABLE QVariantMap lockResource(
        const QString& resourceType,
        const QString& resourceId,
        const QString& routeId,
        const QString& lockType = "ROUTE",
        const QString& operatorId = "system",
        const QString& reason = "",
        int timeoutMinutes = 30
    );

    Q_INVOKABLE QVariantMap lockMultipleResources(
        const QVariantList& lockRequests,
        const QString& routeId,
        const QString& operatorId = "system"
    );

    Q_INVOKABLE bool unlockResource(
        const QString& resourceType,
        const QString& resourceId,
        const QString& routeId
    );

    Q_INVOKABLE bool unlockAllResourcesForRoute(const QString& routeId);

    Q_INVOKABLE bool forceUnlockResource(
        const QString& resourceType,
        const QString& resourceId,
        const QString& operatorId,
        const QString& reason
    );

    // Lock status queries
    Q_INVOKABLE bool isResourceLocked(
        const QString& resourceType,
        const QString& resourceId
    ) const;

    Q_INVOKABLE QVariantMap getResourceLockStatus(
        const QString& resourceType,
        const QString& resourceId
    ) const;

    Q_INVOKABLE QVariantList getActiveLocksForRoute(const QString& routeId) const;
    Q_INVOKABLE QVariantList getAllActiveLocks() const;
    Q_INVOKABLE QVariantList getExpiredLocks() const;

    // Conflict detection
    Q_INVOKABLE QVariantMap checkLockConflicts(
        const QString& resourceType,
        const QString& resourceId,
        const QString& requestedLockType
    ) const;

    Q_INVOKABLE QVariantList checkMultipleResourceConflicts(
        const QVariantList& resourceRequests
    ) const;

    // Lock management
    Q_INVOKABLE bool renewLock(
        const QString& resourceType,
        const QString& resourceId,
        const QString& routeId,
        int additionalMinutes = 30
    );

    Q_INVOKABLE QVariantMap getLockStatistics() const;

public slots:
    void initialize();
    void refreshLocksFromDatabase();
    void cleanupExpiredLocks();
    void onResourceChanged(const QString& resourceType, const QString& resourceId);

signals:
    void lockCountChanged();
    void operationalStateChanged();
    void resourceLocked(const QString& resourceType, const QString& resourceId, const QString& routeId);
    void resourceUnlocked(const QString& resourceType, const QString& resourceId, const QString& routeId);
    void lockExpired(const QString& resourceType, const QString& resourceId, const QString& routeId);
    void lockConflictDetected(const QString& resourceType, const QString& resourceId, const QVariantMap& conflictDetails);
    void forceUnlockPerformed(const QString& resourceType, const QString& resourceId, const QString& operatorId, const QString& reason);

private:
    // Core locking logic
    LockResult lockResourceInternal(const LockRequest& request);
    bool unlockResourceInternal(const QString& resourceType, const QString& resourceId, const QUuid& routeId);
    
    // Conflict resolution
    bool isLockCompatible(const ResourceLock& existingLock, const LockRequest& newRequest) const;
    QStringList findConflictingLocks(const QString& resourceType, const QString& resourceId, const QString& lockType) const;
    
    // Database operations
    bool persistLockToDatabase(const ResourceLock& lock);
    bool removeLockFromDatabase(const ResourceLock& lock);
    bool loadLocksFromDatabase();
    
    // Lock validation
    bool validateLockRequest(const LockRequest& request, QString& error) const;
    bool isResourceValid(const QString& resourceType, const QString& resourceId) const;
    
    // Cleanup and maintenance
    void performMaintenanceCheck();
    void removeExpiredLocksFromMemory();
    
    // Utility methods
    QString lockTypeToString(LockType type) const;
    LockType stringToLockType(const QString& typeStr) const;
    QString resourceTypeToString(ResourceType type) const;
    ResourceType stringToResourceType(const QString& typeStr) const;
    QVariantMap lockToVariantMap(const ResourceLock& lock) const;
    ResourceLock variantMapToLock(const QVariantMap& map) const;
    bool updateIndividualResourceStatus(const ResourceLock& lock, bool lockStatus = true);
    bool updateTrackCircuitStatus(const QString& circuitId, bool isLocking, bool isOverlap = false);
    bool updatePointMachineStatus(const QString& machineId, bool isLocked);
    bool updatePointMachineStatusWithPairing(const QString& machineId, bool lockStatus,
                                             const QString& routeId,
                                             QSet<QString>* processedMachines = nullptr);
    bool updateSignalStatus(const QString& signalId, bool isLocked);
    bool updateTrackSegmentStatus(const QString& circuitId, bool isLocking, bool isOverlap = false);

private:
    DatabaseManager* m_dbManager;
    bool m_isOperational = false;

    // In-memory lock storage for fast access
    QHash<QString, QList<ResourceLock>> m_activeLocks; // lockKey -> locks
    QHash<QUuid, QStringList> m_routeLocks; // routeId -> resource keys
    
    // Maintenance timer
    QTimer* m_maintenanceTimer;
    
    // Configuration
    static constexpr int MAINTENANCE_INTERVAL_MS = 60000; // 1 minute
    static constexpr int DEFAULT_LOCK_TIMEOUT_MINUTES = 30;
    static constexpr int MAX_LOCK_DURATION_HOURS = 24;
    static constexpr int CLEANUP_BATCH_SIZE = 100;
    
    // Statistics
    mutable int m_totalLockRequests = 0;
    mutable int m_successfulLocks = 0;
    mutable int m_conflictDetections = 0;
    mutable int m_forceUnlocks = 0;
    mutable int m_expiredLocksCleanedUp = 0;
};

} // namespace RailFlux::Route
