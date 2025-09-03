#pragma once

#include <QObject>
#include <QVariantMap>
#include <QVariantList>
#include <QString>
#include <QStringList>
#include <QUuid>
#include <QDateTime>
#include <QTimer>
#include <QQueue>
#include <QElapsedTimer>
#include <QDebug>
#include <QSqlQuery>
#include <memory>
#include <QThread>
#include <optional>

// Forward declarations
class DatabaseManager;

namespace RailFlux::Route {

// Forward declarations of Layer 2 and Layer 3 services
class GraphService;
class ResourceLockService;
class OverlapService;
class TelemetryService;
class VitalRouteController;

// Forward declaration - RouteState defined in VitalRouteController.h
enum class RouteState;
enum class Direction;

struct RouteRequest {
    QUuid requestId;
    QString sourceSignalId;
    QString destSignalId;
    QString direction;
    QString requestedBy;
    QString priority;
    QDateTime requestedAt;
    QVariantMap trainData;
    QVariantMap metadata;
    QString reason;

    QString key() const { return requestId.toString(); }
};

struct ProcessingResult {
    bool success = false;
    QString error;
    QString routeId;
    QStringList path;
    QStringList overlapCircuits;
    double totalTimeMs = 0.0;
    QVariantMap performanceBreakdown;
    QVariantMap validationResults;

    QVariantMap signalAspects;
    QVariantMap pointMachines;
    QString overlapReservationId;
};

// Add to RouteAssignmentService.h after existing structures

struct HardcodedRoute {
    QString sourceSignalId;
    QString destSignalId;
    QStringList path;                    // Hardcoded circuit path
    QStringList overlapCircuits;         // Hardcoded overlap
    QVariantMap signalAspects;          // Hardcoded signal settings
    QVariantMap pointMachineSettings;   // Hardcoded PM positions
    QString reachability;               // "SUCCESS" or "BLOCKED"
    QString blockedReason;              // If blocked
    double simulatedProcessingTime;     // For realistic timing
};

struct HardcodedRouteDatabase {
    QList<HardcodedRoute> routes;
    QMap<QString, QList<HardcodedRoute>> routesBySource; // Indexed by source signal
};

class RouteAssignmentService : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isOperational READ isOperational NOTIFY operationalStateChanged)
    Q_PROPERTY(int activeRoutes READ activeRoutes NOTIFY routeCountChanged)
    Q_PROPERTY(int pendingRequests READ pendingRequests NOTIFY requestQueueChanged)
    Q_PROPERTY(double averageProcessingTimeMs READ averageProcessingTimeMs NOTIFY performanceChanged)
    Q_PROPERTY(bool emergencyMode READ emergencyMode NOTIFY emergencyModeChanged)

public:
    explicit RouteAssignmentService(QObject* parent = nullptr);
    ~RouteAssignmentService();

    //
    struct DestinationCandidate {
        QString destSignalId;
        QString displayName;
        QString direction;
        QString reachability; // REACHABLE_CLEAR, REACHABLE_REQUIRES_PM, BLOCKED
        QString blockedReason; // OCCUPIED, RESERVED, LOCKED_PM, etc.

        struct PathSummary {
            int hopCount = -1;           // -1 indicates no valid path, 0+ indicates valid path
            QStringList circuitsPreview; // First few + last circuit
            double estimatedWeight = -1.0; // -1 indicates no valid path, 0+ indicates valid weight
        } pathSummary;

        struct RequiredPMAction {
            QString machineId;
            QString currentPosition;
            QString targetPosition;
        };
        QList<RequiredPMAction> requiredPMActions;

        QStringList conflicts;
        QVariantMap telemetry;
    };

    // Service composition - must be called after construction
    void setServices(
        DatabaseManager* dbManager,
        GraphService* graphService,
        ResourceLockService* resourceLockService,
        OverlapService* overlapService,
        TelemetryService* telemetryService,
        VitalRouteController* vitalController
        );

    // Properties
    bool isOperational() const { return m_isOperational; }
    int activeRoutes() const;
    int pendingRequests() const { return m_requestQueue.size(); }
    double averageProcessingTimeMs() const { return m_averageProcessingTime; }
    bool emergencyMode() const { return m_emergencyMode; }

    // === ROUTE SCANNING API ===
    Q_INVOKABLE QVariantMap scanDestinationSignals(
        const QString& sourceSignalId,
        const QString& direction = "AUTO", // AUTO, UP, DOWN
        bool includeBlocked = true
        );

    // === MAIN API ===
    Q_INVOKABLE QString requestRoute(
        const QString& sourceSignalId,
        const QString& destSignalId,
        const QString& direction = "UP",
        const QString& requestedBy = "operator",
        const QVariantMap& trainData = QVariantMap(),
        const QString& priority = "NORMAL"
        );

    Q_INVOKABLE bool cancelRoute(
        const QString& routeId,
        const QString& reason = "operator_cancel"
        );

    Q_INVOKABLE bool activateRoute(
        const QString& routeId
        );

    Q_INVOKABLE bool releaseRoute(
        const QString& routeId,
        const QString& reason = "normal_release"
        );

    // === EMERGENCY OPERATIONS ===
    Q_INVOKABLE bool emergencyReleaseRoute(
        const QString& routeId,
        const QString& reason
        );

    Q_INVOKABLE bool emergencyReleaseAllRoutes(
        const QString& reason
        );

    Q_INVOKABLE void activateEmergencyMode(
        const QString& reason
        );

    Q_INVOKABLE void deactivateEmergencyMode();

    // === ROUTE STATUS AND MONITORING ===
    Q_INVOKABLE QVariantMap getRouteStatus(const QString& routeId) const;
    Q_INVOKABLE QVariantList getActiveRoutes() const;
    Q_INVOKABLE QVariantList getPendingRequests() const;
    Q_INVOKABLE QVariantMap getSystemStatus() const;

    // === CONFIGURATION ===
    Q_INVOKABLE bool setMaxConcurrentRoutes(int maxRoutes);
    Q_INVOKABLE int getMaxConcurrentRoutes() const { return m_maxConcurrentRoutes; }
    Q_INVOKABLE bool setProcessingTimeout(int timeoutMs);
    Q_INVOKABLE int getProcessingTimeout() const { return m_processingTimeoutMs; }

    // === STATISTICS AND REPORTING ===
    Q_INVOKABLE QVariantMap getPerformanceStatistics() const;
    Q_INVOKABLE QVariantMap getOperationalStatistics() const;
    Q_INVOKABLE QVariantList getRouteHistory(int limitHours = 24) const;

public slots:
    void initialize();
    void processRequestQueue();
    void onTrackCircuitOccupancyChanged(const QString& circuitId, bool isOccupied);
    void onRouteStateChanged(const QString& routeId, const QString& newState);
    void onSystemOverload();
    void performMaintenanceCheck();

signals:
    void operationalStateChanged();
    void routeCountChanged();
    void requestQueueChanged();
    void performanceChanged();
    void emergencyModeChanged();

    void routeRequested(const QString& requestId, const QString& sourceSignal, const QString& destSignal);
    void routeAssigned(const QString& routeId, const QString& sourceSignal, const QString& destSignal, const QStringList& path);
    void routeActivated(const QString& routeId);
    void routeReleased(const QString& routeId, const QString& reason);
    void routeFailed(const QString& requestId, const QString& reason);

    void emergencyActivated(const QString& reason);
    void emergencyDeactivated();
    void systemOverloaded(int pendingRequests, int maxConcurrent);
    void performanceWarning(const QString& metric, double value, double threshold);

private:
    // === CLEARANCE CHECK STRUCTURES ===
    struct ClearanceCheckResult {
        bool isCleared = true;
        QString blockReason;
        QStringList conflicts;
        QList<DestinationCandidate::RequiredPMAction> requiredPMActions;
    };

    // === SCAN IMPLEMENTATION ===
    QList<DestinationCandidate> performDestinationScan(
        const QString& sourceSignalId,
        const QString& direction
        );

    QStringList getEligibleDestinationSignals(
        const QString& sourceSignalId,
        const QString& direction
        );

    DestinationCandidate evaluateDestinationReachability(
        const QString& sourceSignalId,
        const QString& destSignalId,
        const QString& direction
        );

    QString determineSignalDirection(const QString& signalId);
    bool isValidSignalProgression(const QString& sourceType, const QString& destType);
    QVariantMap formatScanResults(const QList<DestinationCandidate>& candidates);

    // === CLEARANCE CHECKING ===
    ClearanceCheckResult checkPathClearance(const QStringList& path);
    bool isCircuitReserved(const QString& circuitId);
    bool isPointMachineSettable(const QString& machineId);
    QList<DestinationCandidate::RequiredPMAction> getRequiredPointMachineActions(const QStringList& path);

    // Main route processing pipeline
    ProcessingResult processRouteRequest(const RouteRequest& request);

    // Processing pipeline stages
    ProcessingResult validateRequest(const RouteRequest& request);
    ProcessingResult performPathfinding(const RouteRequest& request);
    ProcessingResult calculateOverlap(const RouteRequest& request, const QStringList& path);
    ProcessingResult reserveResources(const RouteRequest& request, const QStringList& path, const QStringList& overlap);
    ProcessingResult finalizeRoute(const RouteRequest& request, const QStringList& path, const QStringList& overlap);

    // Queue management
    void addToQueue(const RouteRequest& request);
    RouteRequest dequeueRequest();
    void prioritizeQueue();
    bool shouldProcessRequest() const;

    // Route state management
    void updateRouteState(const QString& routeId, RouteState newState);
    void handleRouteActivation(const QString& routeId);
    void handleRouteRelease(const QString& routeId, const QString& reason);

    // Emergency handling
    void processEmergencyRelease(const QString& routeId, const QString& reason);
    void processEmergencyReleaseAll(const QString& reason);
    void enterDegradedMode();
    void exitDegradedMode();

    // Performance monitoring
    void recordProcessingTime(const QString& stage, double timeMs);
    void updateAverageProcessingTime();
    void checkPerformanceThresholds();

    // System monitoring
    void checkSystemHealth();
    void updateOperationalStatus();
    bool areServicesHealthy() const;

    // Configuration management
    bool loadConfiguration();
    bool saveConfiguration();
    void applyDegradedModeSettings();
    void restoreNormalModeSettings();

    // Integration with Layer 2 services
    QVariantMap findOptimalPath(const QString& sourceSignal, const QString& destSignal, const QString& direction);
    QVariantMap calculateRouteOverlap(const QString& destSignal, const QStringList& path, const QVariantMap& trainData);
    bool reserveRouteResources(const QString& routeId, const QStringList& circuits, const QStringList& pointMachines);
    bool releaseRouteResources(const QString& routeId);

    // Database integration
    bool persistRouteRequest(const RouteRequest& request);
    bool persistRouteAssignment(const QString& routeId, const RouteRequest& request, const QStringList& path);
    bool updateRouteInDatabase(const QString& routeId, RouteState state, const QVariantMap& data = QVariantMap());

    // Reactive updates from database
    void handleTrackOccupancyChange(const QString& circuitId, bool isOccupied);
    void handlePointMachinePositionChange(const QString& machineId, const QString& position);
    QList<DestinationCandidate::RequiredPMAction> analyzeRequiredPMMovements(const QStringList& path, const QString& direction, const QVariantMap& currentPMStates);

    // Utility methods
    QString generateRequestId() const;
    QString routeStateToString(RouteState state) const;
    RouteState stringToRouteState(const QString& stateStr) const;
    QVariantMap requestToVariantMap(const RouteRequest& request) const;
    QVariantMap resultToVariantMap(const ProcessingResult& result) const;
    int calculateRequestPriority(const RouteRequest& request) const;

    // Validation helpers
    bool isValidSignalId(const QString& signalId) const;
    bool isValidDirection(const QString& direction) const;
    bool isValidPriority(const QString& priority) const;
    bool canAcceptNewRequests() const;
    QString resolveSignalToCircuit(const QString& signalId, bool isSource);

    int convertPriorityToInt(const QString& priorityStr) const;

    void initializeHardcodedRoutes();
    HardcodedRoute findHardcodedRoute(const QString& sourceId, const QString& destId);
    bool applyHardcodedRoute(const QString& routeId, const HardcodedRoute& route, const QString& operatorId);



private:
    // Service dependencies (composed services)
    DatabaseManager* m_dbManager = nullptr;
    std::unique_ptr<GraphService> m_graphService;
    std::unique_ptr<ResourceLockService> m_resourceLockService;
    std::unique_ptr<OverlapService> m_overlapService;
    std::unique_ptr<TelemetryService> m_telemetryService;
    std::unique_ptr<VitalRouteController> m_vitalController;

    // Operational state
    bool m_isOperational = false;
    bool m_emergencyMode = false;
    bool m_degradedMode = false;
    // === HARDCODED ROUTE DATA ===
    HardcodedRouteDatabase m_hardcodedRoutes;

    // Request processing
    QQueue<RouteRequest> m_requestQueue;
    QHash<QString, RouteRequest> m_processingRequests; // requestId -> request
    QTimer* m_processingTimer;

    // Configuration
    int m_maxConcurrentRoutes = 10;
    int m_degradedMaxRoutes = 5;
    int m_processingTimeoutMs = 5000;  // 5 seconds
    int m_queueProcessingIntervalMs = 100;
    int m_maintenanceIntervalMs = 30000; // 30 seconds

    // Performance monitoring
    QList<double> m_processingTimes;
    double m_averageProcessingTime = 0.0;
    QDateTime m_lastPerformanceUpdate;
    QHash<QString, QList<double>> m_stagePerformance; // stage -> times
    qint64 m_serviceStartTime;

    // Statistics
    mutable int m_totalRequests = 0;
    mutable int m_successfulRoutes = 0;
    mutable int m_failedRoutes = 0;
    mutable int m_emergencyReleases = 0;
    mutable int m_timeouts = 0;

    // Performance thresholds
    static constexpr double TARGET_PROCESSING_TIME_MS = 1000.0;  // 1 second
    static constexpr double WARNING_PROCESSING_TIME_MS = 2000.0; // 2 seconds
    static constexpr int MAX_QUEUE_SIZE = 50;
    static constexpr int PERFORMANCE_HISTORY_SIZE = 100;
    static constexpr int OVERLOAD_THRESHOLD = 20; // Pending requests
    static constexpr double PATHFINDING_TIMEOUT_MS = 500.0;
    static constexpr double OVERLAP_CALCULATION_TIMEOUT_MS = 200.0;
    static constexpr double RESOURCE_RESERVATION_TIMEOUT_MS = 300.0;

    // Timers
    QTimer* m_maintenanceTimer;
};

} // namespace RailFlux::Route
