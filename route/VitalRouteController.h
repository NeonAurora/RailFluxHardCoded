#pragma once

#include <QObject>
#include <QVariantMap>
#include <QVariantList>
#include <QString>
#include <QStringList>
#include <QUuid>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDebug>
#include <QTimer>
#include <memory>
#include <chrono>
#include "../interlocking/AspectPropagationService.h"

// Forward declarations
#include "SafetyMonitorService.h"
class DatabaseManager;
class InterlockingService;

// Forward declaration for AspectPropagationService
namespace RailFlux::Interlocking {
    class AspectPropagationService;
}

namespace RailFlux::Route {

// Forward declarations of Layer 2 services
class ResourceLockService;
class TelemetryService;

enum class SafetyLevel {
    VITAL_SAFE,
    SAFE,
    CAUTION,
    WARNING,
    DANGER
};

enum class RouteState {
    REQUESTED,
    VALIDATING,
    RESERVED,
    ACTIVE,
    PARTIALLY_RELEASED,
    RELEASED,
    FAILED,
    EMERGENCY_RELEASED,
    DEGRADED
};

struct ValidationResult {
    bool isAllowed = false;
    SafetyLevel safetyLevel = SafetyLevel::DANGER;
    QString reason;
    QString details;
    QStringList conflictingResources;
    QStringList alternativeSolutions;
    QVariantMap performanceMetrics;
    QVariantMap interlockingResults;
    std::chrono::milliseconds responseTime{0};

    // Convenience methods
    bool isVitalSafe() const { return isAllowed && safetyLevel == SafetyLevel::VITAL_SAFE; }
    bool isSafe() const { return isAllowed && safetyLevel >= SafetyLevel::SAFE; }

    static ValidationResult allowed(const QString& reason = "Validation passed") {
        ValidationResult result;
        result.isAllowed = true;
        result.safetyLevel = SafetyLevel::VITAL_SAFE;
        result.reason = reason;
        return result;
    }

    static ValidationResult blocked(const QString& reason, SafetyLevel level = SafetyLevel::DANGER) {
        ValidationResult result;
        result.isAllowed = false;
        result.safetyLevel = level;
        result.reason = reason;
        return result;
    }
};

// SafetyViolation struct moved to SafetyMonitorService.h to avoid redefinition

struct RouteAssignment {
    QUuid id;
    QString routeName;
    QString sourceSignalId;
    QString destSignalId;
    QString direction;
    QStringList assignedCircuits;
    QStringList overlapCircuits;
    QStringList lockedPointMachines;
    RouteState state = RouteState::REQUESTED;
    int priority = 100;
    QString operatorId;
    QDateTime createdAt;
    QDateTime activatedAt;
    QDateTime releasedAt;
    QDateTime overlapReleaseDueAt;
    QString failureReason;
    QVariantMap performanceMetrics;

    // Utility methods
    QString key() const { return id.toString(); }
    bool isActive() const {
        return state == RouteState::RESERVED ||
               state == RouteState::ACTIVE ||
               state == RouteState::PARTIALLY_RELEASED;
    }
};

class VitalRouteController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isOperational READ isOperational NOTIFY operationalStateChanged)
    Q_PROPERTY(bool safetySystemHealthy READ safetySystemHealthy NOTIFY safetyStatusChanged)
    Q_PROPERTY(int activeRoutes READ activeRoutes NOTIFY routeCountChanged)
    Q_PROPERTY(double averageValidationTimeMs READ averageValidationTimeMs NOTIFY performanceChanged)

public:
    explicit VitalRouteController(
        DatabaseManager* dbManager,
        InterlockingService* interlockingService,
        ResourceLockService* resourceLockService,
        TelemetryService* telemetryService,
        QObject* parent = nullptr
        );
    
    // === ASPECT PROPAGATION INTEGRATION ===
    void setAspectPropagationService(RailFlux::Interlocking::AspectPropagationService* aspectService);
    ValidationResult validateAgainstInterlockingWithIntelligentAspects(const RouteAssignment& route);
    bool hasIntelligentAspectPropagation() const { return m_aspectPropagationService != nullptr; }
    ~VitalRouteController();

    // Properties
    bool isOperational() const { return m_isOperational; }
    bool safetySystemHealthy() const { return m_safetySystemHealthy; }
    int activeRoutes() const;
    double averageValidationTimeMs() const { return m_averageValidationTimeMs; }  //    Use correct member

    // === SAFETY-CRITICAL OPERATIONS ===
    Q_INVOKABLE QVariantMap reserveRouteResources(const QVariantMap& routeData);
    Q_INVOKABLE QVariantMap releaseRouteResources(const QString& routeId);
    Q_INVOKABLE QVariantMap emergencyRelease(const QString& routeId, const QString& reason);
    Q_INVOKABLE QVariantMap emergencyReleaseAll(const QString& reason);
    
    // === INTELLIGENT ASPECT ESTABLISHMENT ===
    QVariantMap establishRouteWithIntelligentAspects(
        const QString& sourceSignalId,
        const QString& destinationSignalId,
        const QStringList& routePath,        //  ADD
        const QStringList& overlapPath,      //  ADD
        const QVariantMap& pointMachinePositions = {}
        );
    
    Q_INVOKABLE QVariantMap executeCoordinatedAspectChanges(
        const QVariantMap& signalAspects,
        const QVariantMap& pointMachinePositions = QVariantMap()
    );

    // === VALIDATION METHODS ===
    Q_INVOKABLE QVariantMap validateRouteRequest(
        const QString& sourceSignalId,
        const QString& destSignalId,
        const QString& direction,
        const QString& operatorId = "system"
        );

    Q_INVOKABLE bool isValidSignalProgression(
        const QString& sourceSignalType,
        const QString& destSignalType
        ) const;

    Q_INVOKABLE QVariantMap validateResourceAvailability(
        const QStringList& circuits,
        const QStringList& pointMachines
        );

    Q_INVOKABLE QVariantMap validateAgainstInterlocking(const QVariantMap& routeData);

    // === ROUTE STATE MANAGEMENT ===
    Q_INVOKABLE bool updateRouteState(const QString& routeId, const QString& newState);
    Q_INVOKABLE QVariantMap getRouteStatus(const QString& routeId) const;
    Q_INVOKABLE QVariantList getActiveRoutes() const;
    Q_INVOKABLE QVariantMap getRouteStatistics() const;

    // === SAFETY MONITORING ===
    Q_INVOKABLE ValidationResult performSafetyCheck(const QString& routeId = QString());
    Q_INVOKABLE QVariantMap getSafetyStatus() const;
    Q_INVOKABLE QVariantList detectSafetyViolations() const;

    // === RESOURCE MANAGEMENT ===
    Q_INVOKABLE ValidationResult lockRouteResources(
        const QString& routeId,
        const QStringList& circuits,
        const QStringList& pointMachines
        );

    Q_INVOKABLE bool unlockRouteResources(const QString& routeId);
    Q_INVOKABLE QVariantMap getResourceUtilization() const;

public slots:
    void initialize();
    void onTrackCircuitOccupancyChanged(const QString& circuitId, bool isOccupied);
    void onPointMachinePositionChanged(const QString& machineId, const QString& position);
    void onSignalAspectChanged(const QString& signalId, const QString& aspect);
    void performPeriodicSafetyCheck();

    //   NEW: Timer callback slots
    void performPeriodicValidation();
    void performHealthCheck();

signals:
    void operationalStateChanged();
    void safetyStatusChanged();
    void routeCountChanged();
    void performanceChanged();

    void routeReserved(const QString& routeId, const QString& sourceSignal, const QString& destSignal);
    void routeActivated(const QString& routeId);
    void routeReleased(const QString& routeId, const QString& reason);
    void emergencyReleasePerformed(const QString& routeId, const QString& reason);

    void safetyViolationDetected(const QString& routeId, const QString& violationType, const QString& details);
    void safetySystemFailure(const QString& component, const QString& error);
    void resourceConflictDetected(const QString& resourceType, const QString& resourceId, const QStringList& conflictingRoutes);

private:
    // Core validation logic
    ValidationResult reserveRouteResourcesInternal(RouteAssignment& route);
    ValidationResult releaseRouteResourcesInternal(const QString& routeId);
    ValidationResult emergencyReleaseInternal(const QString& routeId, const QString& reason);

    // Validation helpers
    ValidationResult validateRouteRequestInternal(
        const QString& sourceSignalId,
        const QString& destSignalId,
        const QString& direction,
        const QString& operatorId
        );

    ValidationResult validateSignalProgression(
        const QString& sourceSignalId,
        const QString& destSignalId
        ) const;

    ValidationResult validateResourceAvailabilityInternal(
        const QStringList& circuits,
        const QStringList& pointMachines
        );

    ValidationResult validateAgainstInterlockingInternal(const RouteAssignment& route);

    // Safety validation
    bool isRouteConflictFree(const RouteAssignment& route) const;
    bool areResourcesAvailable(const QStringList& circuits, const QStringList& pointMachines) const;
    QStringList findConflictingRoutes(const RouteAssignment& route) const;

    // Interlocking integration
    bool validateWithInterlockingService(const RouteAssignment& route, ValidationResult& result);
    bool checkSignalInterlocking(const QString& signalId, const QString& requestedAspect);
    bool checkPointMachineInterlocking(const QString& machineId, const QString& requestedPosition);

    // Resource management
    bool lockResourcesForRoute(const RouteAssignment& route, const QStringList& affectedSignals);
    bool unlockResourcesForRoute(const QString& routeId);
    QStringList getLockedResourcesForRoute(const QString& routeId) const;

    // Route state management
    bool persistRouteToDatabase(const RouteAssignment& route);
    bool updateRouteInDatabase(const RouteAssignment& route);
    bool removeRouteFromDatabase(const QString& routeId);
    bool loadActiveRoutesFromDatabase();

    // Safety monitoring
    void recordSafetyEvent(const QString& eventType, const QString& routeId, const QString& details);
    void recordSafetyViolation(const QString& routeId, const QString& description);
    void checkForSafetyViolations();
    void updateSafetySystemHealth();

    // Performance monitoring
    void recordValidationTime(const QString& operation, std::chrono::milliseconds duration);
    void updateAverageValidationTime();

    // Emergency procedures
    void activateEmergencyProtocol(const QString& reason);
    void notifyEmergencyServices(const QString& routeId, const QString& reason);

    // Utility methods
    QString routeStateToString(RouteState state) const;
    RouteState stringToRouteState(const QString& stateStr) const;
    QString safetyLevelToString(SafetyLevel level) const;
    SafetyLevel stringToSafetyLevel(const QString& levelStr) const;
    QVariantMap validationResultToVariantMap(const ValidationResult& result) const;
    QVariantMap routeAssignmentToVariantMap(const RouteAssignment& route) const;
    RouteAssignment variantMapToRouteAssignment(const QVariantMap& map) const;

    // Signal type validation helpers
    bool isValidSourceSignalType(const QString& signalType) const;
    bool isValidDestSignalType(const QString& signalType) const;
    bool isValidProgressionSequence(const QString& sourceType, const QString& destType) const;
    bool isAdvancedStarterDestination(const QString& signalId) const;

private:
    // Service dependencies
    DatabaseManager* m_dbManager;
    InterlockingService* m_interlockingService;
    ResourceLockService* m_resourceLockService;
    TelemetryService* m_telemetryService;
    RailFlux::Interlocking::AspectPropagationService* m_aspectPropagationService = nullptr;

    //   NEW: Timer management
    std::unique_ptr<QTimer> m_validationTimer;
    std::unique_ptr<QTimer> m_healthCheckTimer;

    // Operational state
    bool m_isOperational = false;
    bool m_safetySystemHealthy = true;

    // Route storage
    QHash<QString, RouteAssignment> m_activeRoutes; // routeId -> route
    QHash<QString, QStringList> m_routesByCircuit; // circuitId -> routeIds using this circuit

    //   NEW: Time tracking
    qint64 m_systemStartTime = 0;
    QDateTime m_lastHealthCheck;

    // Performance monitoring
    QList<std::chrono::milliseconds> m_validationTimes;
    double m_averageValidationTime = 0.0;  //   EXISTING: Keep original
    double m_averageValidationTimeMs = 0.0;  //   NEW: Add expected member
    QDateTime m_lastPerformanceUpdate;

    // Safety monitoring
    QDateTime m_lastSafetyCheck;
    QList<SafetyViolation> m_recentSafetyViolations;
    int m_consecutiveFailures = 0;

    //   NEW: Timer interval constants
    static constexpr int PERIODIC_VALIDATION_INTERVAL_MS = 5000;  // 5 seconds
    static constexpr int HEALTH_CHECK_INTERVAL_MS = 10000;       // 10 seconds

    // Configuration constants
    static constexpr std::chrono::milliseconds TARGET_VALIDATION_TIME{50};
    static constexpr std::chrono::milliseconds MAX_VALIDATION_TIME{200};
    static constexpr int MAX_CONSECUTIVE_FAILURES = 3;
    static constexpr int SAFETY_CHECK_INTERVAL_MS = 5000; // 5 seconds
    static constexpr int PERFORMANCE_HISTORY_SIZE = 100;
    static constexpr int MAX_CONCURRENT_ROUTES = 10;
    static constexpr int EMERGENCY_COOLDOWN_MS = 30000; // 30 seconds

    // Statistics
    mutable int m_totalValidations = 0;
    mutable int m_successfulValidations = 0;
    mutable int m_emergencyReleases = 0;
    mutable int m_safetyViolations = 0;
};

} // namespace RailFlux::Route
