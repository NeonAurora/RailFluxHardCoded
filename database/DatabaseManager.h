#pragma once
#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlDriver>
#include <QTimer>
#include <QHash>
#include <QVariantMap>
#include <QVariantList>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <memory>
#include <QProcess>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QElapsedTimer>

class InterlockingService;

class DatabaseManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isConnected READ isConnected NOTIFY connectionStateChanged)

    // UPDATED: Data model properties for QML binding
    Q_PROPERTY(QVariantList trackSegments READ getTrackSegmentsList NOTIFY trackSegmentsChanged)
    Q_PROPERTY(QVariantList trackCircuits READ getTrackCircuitsList NOTIFY trackCircuitsChanged)
    Q_PROPERTY(QVariantList allSignals READ getAllSignalsList NOTIFY signalsChanged)
    Q_PROPERTY(QVariantList allPointMachines READ getAllPointMachinesList NOTIFY pointMachinesChanged)
    Q_PROPERTY(QVariantList textLabels READ getTextLabelsList NOTIFY textLabelsChanged)

    Q_PROPERTY(int currentPollingInterval READ getCurrentPollingInterval NOTIFY pollingIntervalChanged)
    Q_PROPERTY(QString pollingIntervalDisplay READ getPollingIntervalDisplay NOTIFY pollingIntervalChanged)

public:
    explicit DatabaseManager(QObject* parent = nullptr);
    ~DatabaseManager();

    void setInterlockingService(InterlockingService* service);
    QSqlDatabase getDatabase() const;
    QString getCurrentSignalAspect(const QString& signalId);

    // Connection management
    Q_INVOKABLE bool connectToDatabase();
    Q_INVOKABLE bool connectToSystemPostgreSQL();
    Q_INVOKABLE bool startPortableMode();
    Q_INVOKABLE bool isConnected() const;
    Q_INVOKABLE void cleanup();

    // Polling management
    Q_INVOKABLE void startPolling();
    Q_INVOKABLE void stopPolling();
    Q_INVOKABLE int getCurrentPollingInterval() const;
    Q_INVOKABLE QString getPollingIntervalDisplay() const;

    // Real-time notifications
    Q_INVOKABLE void enableRealTimeUpdates();

    // STREAMLINED: Track Segment Circuit operations (primary occupancy management)
    Q_INVOKABLE QVariantList getTrackCircuitsList();
    Q_INVOKABLE bool updateTrackCircuitOccupancy(const QString& trackCircuitId, bool isOccupied);
    Q_INVOKABLE bool getTrackCircuitOccupancy(const QString& trackCircuitId);
    Q_INVOKABLE QVariantMap getAllTrackCircuitStates();

    // STREAMLINED: Track Segment Segment operations (UI and physical layout)
    Q_INVOKABLE QVariantList getTrackSegmentsList();
    Q_INVOKABLE QVariantList getTrackSegmentsByCircuitId(const QString& trackCircuitId);
    Q_INVOKABLE QVariantMap getTrackSegmentById(const QString& trackSegmentId);
    Q_INVOKABLE bool updateTrackSegmentOccupancy(const QString& trackSegmentId, bool isOccupied);

    // STREAMLINED: Signal operations
    Q_INVOKABLE QVariantList getAllSignalsList();
    Q_INVOKABLE QVariantList getOuterSignalsList();
    Q_INVOKABLE QVariantList getHomeSignalsList();
    Q_INVOKABLE QVariantList getStarterSignalsList();
    Q_INVOKABLE QVariantList getAdvanceStarterSignalsList();
    Q_INVOKABLE QVariantMap getSignalById(const QString& signalId);
    Q_INVOKABLE bool updateSignalAspect(const QString& signalId, const QString& aspectType, const QString& newAspect);
    Q_INVOKABLE QVariantMap getAllSignalStates();
    Q_INVOKABLE QString getSignalState(int signalId);  // KEPT: Legacy for compatibility

    // STREAMLINED: Point Machine operations
    Q_INVOKABLE QVariantList getAllPointMachinesList();
    Q_INVOKABLE QVariantMap getPointMachineById(const QString& machineId);
    Q_INVOKABLE QVariantMap getAllPointMachineStates();
    Q_INVOKABLE QString getPointPosition(int machineId);  // KEPT: Legacy for compatibility

    // Text Labels
    Q_INVOKABLE QVariantList getTextLabelsList();
    Q_INVOKABLE QStringList getInterlockedSignals(const QString& signalId);

    // === NEW: TRIPLE-SOURCE PROTECTION SIGNAL QUERIES ===
    QStringList getProtectingSignalsFromInterlockingRules(const QString& circuitId);
    QStringList getProtectingSignalsFromTrackCircuits(const QString& circuitId);
    QStringList getProtectingSignalsFromTrackSegments(const QString& trackSegmentId);
    QString getCircuitIdByTrackSegmentId(const QString& trackSegmentId);
    QStringList getProtectedTrackCircuitsFromInterlockingRules(const QString& signalId);

    QVariantMap getTrackCircuitById(const QString& circuitId);

    // === INTERLOCKING HELPER METHODS ===
    QString getPairedMachine(const QString& machineId);
    QString getCurrentPointPosition(const QString& machineId);
    QVariantList getPointMachinesByTrackCircuit(const QString& trackCircuitId);

    // === ROUTE ASSIGNMENT METHODS ===
    Q_INVOKABLE bool insertRouteAssignment(
        const QString& routeId,
        const QString& sourceSignalId,
        const QString& destSignalId,
        const QString& direction,
        const QStringList& assignedCircuits,
        const QStringList& overlapCircuits,
        const QString& state,
        const QStringList& lockedPointMachines,
        int priority,
        const QString& operatorId
    );
    
    Q_INVOKABLE bool updateRouteState(
        const QString& routeId,
        const QString& newState,
        const QString& failureReason = QString()
        );
    Q_INVOKABLE bool updateRouteActivation(const QString& routeId);
    Q_INVOKABLE bool updateRouteRelease(const QString& routeId);
    Q_INVOKABLE bool updateRouteFailure(const QString& routeId, const QString& failureReason);
    Q_INVOKABLE bool updateRoutePerformanceMetrics(const QString& routeId, const QVariantMap& metrics);
    
    Q_INVOKABLE QVariantMap getRouteAssignment(const QString& routeId);
    Q_INVOKABLE QVariantList getActiveRoutes();
    Q_INVOKABLE QVariantList getRoutesByState(const QString& state);
    Q_INVOKABLE QVariantList getRoutesBySignal(const QString& signalId);
    
    // Route event logging
    Q_INVOKABLE bool insertRouteEvent(
        const QString& routeId,
        const QString& eventType,
        const QVariantMap& eventData,
        const QString& operatorId = QString(),
        const QString& sourceComponent = QString(),
        const QString& correlationId = QString(),
        double responseTimeMs = 0.0,
        bool safetyCritical = false
    );
    
    Q_INVOKABLE QVariantList getRouteEvents(const QString& routeId, int limitHours = 24);
    
    // Resource lock management
    Q_INVOKABLE bool insertResourceLock(
        const QString& resourceType,
        const QString& resourceId,
        const QString& routeId,
        const QString& lockType
    );
    
    Q_INVOKABLE bool releaseResourceLocks(const QString& routeId);
    Q_INVOKABLE QVariantList getResourceLocks(const QString& routeId);
    Q_INVOKABLE QVariantList getConflictingLocks(const QString& resourceId, const QString& resourceType);
    
    // Track circuit edges for pathfinding
    Q_INVOKABLE QVariantList getTrackCircuitEdges();
    Q_INVOKABLE QVariantList getOutgoingEdges(const QString& circuitId);
    Q_INVOKABLE QVariantList getIncomingEdges(const QString& circuitId);
    
    // Signal overlap definitions
    Q_INVOKABLE QVariantMap getSignalOverlapDefinition(const QString& signalId);
    Q_INVOKABLE QVariantList getAllSignalOverlapDefinitions();

    Q_INVOKABLE bool deleteRouteAssignment(const QString& routeId, bool forceDelete = false);

public slots:
    // Enhanced update method
    bool updatePointMachinePosition(const QString& machineId, const QString& newPosition);


signals:
    // Connection and system
    void connectionStateChanged(bool connected);
    void dataUpdated();
    void errorOccurred(const QString& error);
    void operationBlocked(const QString& entityId, const QString& reason);
    void pollingIntervalChanged(int newInterval);

    //  Consistent track segment signals
    void trackSegmentsChanged();
    void trackSegmentUpdated(const QString& trackSegmentId);

    // NEW: Track Segment circuit signals
    void trackCircuitsChanged();
    void trackCircuitUpdated(const QString& trackCircuitId);

    // Signal change signals
    void signalsChanged();
    void signalUpdated(const QString& signalId);
    void signalStateChanged(int signalId, const QString& newState);  // KEPT: Legacy

    // Point machine signals
    void pointMachinesChanged();
    void pointMachineUpdated(const QString& machineId);
    void pointMachineStateChanged(int machineId, const QString& newPosition);  // KEPT: Legacy

    // Track Segment circuit state (for legacy compatibility)
    void trackCircuitStateChanged(int circuitId, bool isOccupied);

    // Text labels
    void textLabelsChanged();

    void pairedMachinesUpdated(const QStringList& machineIds);
    void positionMismatchCorrected(const QString& machineId, const QString& pairedMachineId);

    // === ROUTE ASSIGNMENT SIGNALS ===
    void routeAssignmentInserted(const QString& routeId);
    void routeStateChanged(const QString& routeId, const QString& newState);
    void routeActivated(const QString& routeId);
    void routeReleased(const QString& routeId);
    void routeFailed(const QString& routeId, const QString& reason);
    void routeEventLogged(const QString& routeId, const QString& eventType);
    void resourceLockAcquired(const QString& routeId, const QString& resourceType, const QString& resourceId);
    void resourceLockReleased(const QString& routeId);
    void routeAssignmentsChanged();

    void routeDeleted(const QString& routeId);

private slots:
    void pollDatabase();
    void handleDatabaseNotification(const QString& name, const QVariant& payload);

private:
    // REMOVED: POLLING_INTERVAL_MS (as requested)
    static constexpr int POLLING_INTERVAL_MS = 50;
    static constexpr int POLLING_INTERVAL_FAST = 400000;     //  Real production values
    static constexpr int POLLING_INTERVAL_SLOW = 500000;   //  Real production values

    // Services
    InterlockingService* m_interlockingService = nullptr;

    // Database connection
    QSqlDatabase db;
    std::unique_ptr<QTimer> pollingTimer;
    bool connected;
    bool m_isConnected = false;
    QString m_connectionStatus = "Not Connected";

    // Real-time notifications
    bool m_notificationsEnabled = false;
    bool m_notificationsWorking = false;
    QDateTime m_lastNotificationReceived;
    QTimer* m_notificationHealthTimer = nullptr;

    // Portable PostgreSQL
    QProcess* m_postgresProcess = nullptr;
    QString m_appDirectory;
    QString m_postgresPath;
    QString m_dataPath;
    int m_portablePort = 5433;
    int m_systemPort = 5432;

    // State tracking for polling
    QHash<int, QString> lastSignalStates;
    QHash<int, bool> lastTrackSegmentStates;  // Now trackSegments circuit states
    QHash<int, QString> lastPointStates;

    // Private methods
    void detectAndEmitChanges();
    void checkNotificationHealth();
    void logError(const QString& operation, const QSqlError& error);

    // Database setup
    bool setupDatabase();

    // Portable PostgreSQL management
    bool startPortablePostgreSQL();
    bool stopPortablePostgreSQL();
    bool initializePortableDatabase();
    bool isPortableServerRunning();
    QString getApplicationDirectory();

    // Row conversion helpers
    QVariantMap convertSignalRowToVariant(const QSqlQuery& query);
    QVariantMap convertTrackSegmentRowToVariant(const QSqlQuery& query);
    QVariantMap convertPointMachineRowToVariant(const QSqlQuery& query);
    QVariantMap convertTrackCircuitRowToVariant(const QSqlQuery& query);

    // Current state helpers (for interlocking) - MOVED TO PUBLIC

    bool updateMainSignalAspect(const QString& signalId, const QString& newAspect);
    bool updateSubsidiarySignalAspect(const QString& signalId,
                                      const QString& aspectType,
                                      const QString& newAspect);

    // Helper functions
    QString getCurrentSubsidiaryAspect(const QString& signalId, const QString& aspectType);
    QString formatStringListForSQL(const QStringList& list) const;
};
