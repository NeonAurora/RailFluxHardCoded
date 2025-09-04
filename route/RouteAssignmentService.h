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
        DatabaseManager* dbManager
        );

    // Properties
    bool isOperational() const { return m_isOperational; }
    bool emergencyMode() const { return m_emergencyMode; }

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

public slots:
    void initialize();

signals:
    void operationalStateChanged();
    void routeAssigned(const QString& routeId, const QString& sourceSignal, const QString& destSignal, const QStringList& path);
    void routeFailed(const QString& requestId, const QString& reason);

private:
    // === CLEARANCE CHECK STRUCTURES ===
    struct ClearanceCheckResult {
        bool isCleared = true;
        QString blockReason;
        QStringList conflicts;
        QList<DestinationCandidate::RequiredPMAction> requiredPMActions;
    };

    QList<DestinationCandidate> performDestinationScan(
        const QString& sourceSignalId,
        const QString& direction
        );
    QVariantMap formatScanResults(const QList<DestinationCandidate>& candidates);

    // Utility methods
    QString generateRequestId() const;

    void initializeHardcodedRoutes();
    HardcodedRoute findHardcodedRoute(const QString& sourceId, const QString& destId);
    bool applyHardcodedRoute(const QString& routeId, const HardcodedRoute& route, const QString& operatorId);



private:
    // Service dependencies (composed services)
    DatabaseManager* m_dbManager = nullptr;

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

    // Timers
    QTimer* m_maintenanceTimer;
};

} // namespace RailFlux::Route
