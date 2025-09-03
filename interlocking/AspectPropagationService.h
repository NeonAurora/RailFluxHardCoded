#pragma once

#include <QtCore>
#include <QObject>
#include <QVariantMap>
#include <QVariantList>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QSet>
#include <QVector>
#include <QQueue>
#include <QElapsedTimer>
#include <QDebug>
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <optional>

// Forward declarations
class DatabaseManager;
class InterlockingRuleEngine;

namespace RailFlux::Interlocking {

struct ControlNode {
    QString signalId;
    QString signalType;
    QStringList possibleAspects;
    QStringList controlledBy;        // Signals that control this one
    QStringList controls;            // Signals this one controls
    QString controlMode{"AND"};      // "AND", "OR" for multiple controllers
    bool isIndependent{false};       // Can set aspect freely
    QString selectedAspect;          // Chosen aspect for this signal
    
    // Processing state
    bool isProcessed{false};
    int dependencyOrder{-1};
    
    // Grid position for debugging
    double locationRow{0.0};
    double locationCol{0.0};
};

struct ControlEdge {
    QString fromSignalId;           // Controller signal
    QString toSignalId;             // Controlled signal
    QString whenAspect;             // Controlling aspect
    QStringList allowedAspects;     // What aspects are permitted
    QStringList conditions;         // Point machine conditions
    QString ruleId;                 // Reference to interlocking rule
};

struct AspectPropagationResult {
    bool success{false};
    QString errorMessage;
    QString errorCode;
    
    // Selected aspects for each signal
    QVariantMap signalAspects;      // signalId -> selectedAspect
    QVariantMap pointMachines;      // pmId -> requiredPosition
    
    // Analysis details
    QStringList processedSignals;   // Processing order
    QStringList prunedSignals;      // Signals removed from consideration
    QVariantMap decisionReasons;    // signalId -> selection reasoning
    QVariantMap controlPath;        // Source to destination control chain
    
    // Performance metrics
    double processingTimeMs{0.0};
    int graphSize{0};
    int prunedGraphSize{0};
    int circularDependencies{0};
    
    // Validation details
    QStringList validationErrors;
    QStringList validationWarnings;
};

struct PointMachineRequirement {
    QString pointMachineId;
    QString requiredPosition;
    bool isRequired{false};
};

class AspectPropagationService : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isOperational READ isOperational NOTIFY operationalStateChanged)
    Q_PROPERTY(double averageProcessingTimeMs READ averageProcessingTimeMs NOTIFY performanceChanged)
    Q_PROPERTY(double successRate READ successRate NOTIFY statisticsChanged)
    Q_PROPERTY(int totalPropagations READ totalPropagations NOTIFY statisticsChanged)

public:
    explicit AspectPropagationService(
        DatabaseManager* dbManager,
        InterlockingRuleEngine* ruleEngine,
        QObject* parent = nullptr
    );
    ~AspectPropagationService();

    enum class SignalRole {
        DESTINATION,           // The final signal in the route
        SOURCE_INTERMEDIATE,   // Source signal or signals between source and destination
        CONTROLLER_ABOVE_DEST  // Signals that control the destination (upstream controllers)
    };

    // Properties
    bool isOperational() const { return m_isOperational; }
    double averageProcessingTimeMs() const { return m_averageProcessingTimeMs; }
    double successRate() const;
    int totalPropagations() const { return m_totalPropagations; }

    // === MAIN PROPAGATION INTERFACE ===
    Q_INVOKABLE QVariantMap propagateAspects(
        const QString& sourceSignalId,
        const QString& destinationSignalId,
        const QVariantMap& pointMachinePositions = QVariantMap()
    );

    Q_INVOKABLE QVariantMap propagateAspectsAdvanced(
        const QString& sourceSignalId,
        const QString& destinationSignalId,
        const QVariantMap& pointMachinePositions = QVariantMap(),
        const QVariantMap& options = QVariantMap()
    );

    // === GRAPH ANALYSIS METHODS ===
    Q_INVOKABLE QVariantMap buildControlGraph(const QString& sourceSignalId);
    Q_INVOKABLE QVariantMap pruneGraphForDestination(
        const QVariantMap& fullGraph,
        const QString& destinationSignalId
    );
    Q_INVOKABLE QVariantMap analyzeDependencyOrder(const QVariantMap& prunedGraph);

    // === VALIDATION AND CONFIGURATION ===
    Q_INVOKABLE QVariantMap validatePropagationRequest(
        const QString& sourceSignalId,
        const QString& destinationSignalId
    );
    
    Q_INVOKABLE bool setDestinationConstraint(const QString& signalType, const QString& requiredAspect);
    Q_INVOKABLE bool setPriorityAspects(const QString& signalType, const QStringList& priorities);
    Q_INVOKABLE QVariantMap getConfiguration() const;

    // === PERFORMANCE AND DIAGNOSTICS ===
    Q_INVOKABLE QVariantMap getPerformanceMetrics() const;
    Q_INVOKABLE QVariantMap getStatistics() const;
    Q_INVOKABLE QVariantList getRecentPropagations(int limit = 10) const;

    // === TESTING AND DEBUGGING ===
    Q_INVOKABLE QVariantMap testControlGraphConstruction(const QString& sourceSignalId);
    Q_INVOKABLE QVariantMap simulateAspectPropagation(
        const QString& sourceSignalId,
        const QString& destinationSignalId,
        bool dryRun = true
    );

    Q_INVOKABLE QVariantMap pruneGraphForRoute(
        const QVariantMap& fullGraph,
        const QString& sourceSignalId,
        const QString& destinationSignalId
        );

public slots:
    void initialize();
    void onSignalAspectChanged(const QString& signalId, const QString& newAspect);
    void onInterlockingRulesChanged();
    void performPerformanceCheck();

signals:
    void operationalStateChanged();
    void performanceChanged();
    void statisticsChanged();

    void propagationCompleted(const QString& sourceId, const QString& destId, bool success);
    void graphConstructed(int totalNodes, int totalEdges);
    void graphPruned(int originalSize, int prunedSize);
    void aspectPropagationFailed(const QString& sourceId, const QString& destId, const QString& reason);
    void performanceWarning(const QString& operation, double timeMs, double threshold);

private:
    // === CORE ALGORITHM IMPLEMENTATION ===
    AspectPropagationResult propagateAspectsInternal(
        const QString& sourceSignalId,
        const QString& destinationSignalId,
        const QVariantMap& pointMachinePositions,
        const QVariantMap& options = QVariantMap()
    );

    // === GRAPH CONSTRUCTION ===
    QVariantMap buildControlGraphInternal(const QString& sourceSignalId);
    void expandControlNetwork(
        const QString& signalId,
        QHash<QString, ControlNode>& nodes,
        QVector<ControlEdge>& edges,
        QSet<QString>& visited
    );
    
    // === GRAPH PRUNING ===
    QVariantMap pruneGraphForDestinationInternal(
        const QVariantMap& fullGraph,
        const QString& destinationSignalId
    );

    QVariantMap pruneGraphForRouteInternal(
        const QVariantMap& fullGraph,
        const QString& sourceSignalId,
        const QString& destinationSignalId
        );

    QStringList findControlPath(
        const QString& sourceId,
        const QString& destinationId,
        const QHash<QString, ControlNode>& nodes
    );
    bool isSignalRelevantToDestination(
        const QString& signalId,
        const QString& destinationId,
        const QHash<QString, ControlNode>& nodes
    );

    // === DEPENDENCY ORDERING ===
    QVector<ControlNode> createDependencyOrder(const QVariantMap& prunedGraph);
    bool detectCircularDependencies(
        const QHash<QString, ControlNode>& nodes,
        QStringList& circularSignals
    );
    
    bool detectCyclesDFS(
        const QString& signalId,
        const QHash<QString, ControlNode>& nodes,
        QHash<QString, int>& state,
        QStringList& circularSignals
    );
    
    // === ASPECT SELECTION AND PROPAGATION ===
    QVariantMap selectOptimalAspects(
        const QVector<ControlNode>& orderedNodes,
        const QString& destinationSignalId,
        const QVariantMap& pointMachinePositions,
        const QVariantMap& options
    );

    QString selectBestAspect(
        const ControlNode& node,
        const QStringList& allowedByControllers,
        const QString& destinationSignalId,
        bool isDestination,
        const QVariantMap& options
    );
    
    QStringList getAspectsAllowedByControllers(
        const ControlNode& node,
        const QHash<QString, ControlNode>& processedNodes
    );

    QStringList getAspectsPermittedByController(
        const ControlNode& controller,
        const QString& controlledSignalId
    );

    // === VALIDATION HELPERS ===
    bool validateControlConstraints(
        const QString& signalId,
        const QString& selectedAspect,
        const QHash<QString, ControlNode>& processedNodes
    );

    bool checkPointMachineConditions(
        const ControlEdge& edge,
        const QVariantMap& pointMachinePositions
    );

    QVariantMap validatePropagationRequestInternal(
        const QString& sourceSignalId,
        const QString& destinationSignalId
    );

    SignalRole classifySignalRole(
        const QString& signalId,
        const QString& sourceSignalId,
        const QString& destinationSignalId,
        const QVector<ControlNode>& orderedNodes
        ) const;

    bool isControllerAboveDestination(
        const QString& signalId,
        const QString& destinationSignalId,
        const QVector<ControlNode>& orderedNodes
        ) const;

    QStringList getAspectPrioritiesForRole(
        SignalRole role,
        const QString& signalType
        ) const;

    QString selectDestinationAspect(
        const QString& signalType,
        const QStringList& allowedAspects,
        const QVariantMap& options = QVariantMap()
        ) const;

    QVariantMap calculateRequiredPointMachineStates(
        const QStringList& routePath,
        const QStringList& overlapPath = {}
        );

    QString getRequiredPointMachinePosition(
        const QString& fromCircuit,
        const QString& toCircuit
        );

    PointMachineRequirement getPointMachineRequirement(
        const QString& fromCircuit,
        const QString& toCircuit
        );

    // === DATA LOADING AND INTEGRATION ===
    ControlNode loadSignalControlData(const QString& signalId);
    QVector<ControlEdge> loadControlEdges(const QString& signalId);
    QStringList getControllingSignals(const QString& signalId);
    QStringList getControlledSignals(const QString& signalId);
    bool isSignalIndependent(const QString& signalId);
    QStringList findSignalsByType(const QString& signalType);

    // === PERFORMANCE MONITORING ===
    void recordProcessingTime(const QString& operation, double timeMs);
    void updateAverageProcessingTime();
    void checkPerformanceThresholds();
    void recordPropagationResult(const AspectPropagationResult& result);

    // === UTILITY METHODS ===
    QVariantMap controlNodeToVariantMap(const ControlNode& node) const;
    QVariantMap controlEdgeToVariantMap(const ControlEdge& edge) const;
    QVariantMap aspectPropagationResultToVariantMap(const AspectPropagationResult& result) const;
    
    QString formatControlPath(const QStringList& path) const;
    QString formatProcessingOrder(const QStringList& order) const;
    
    // === CONFIGURATION HELPERS ===
    void loadDefaultConfiguration();
    void applyDestinationConstraints(const QString& signalType, QString& selectedAspect, bool isDestination);
    QStringList getAspectPriorities(const QString& signalType) const;
    QString selectBestAspectByRole(
        const ControlNode& node,
        const QStringList& allowedAspects,
        SignalRole role,
        const QVariantMap& options) const;
    QString getRoleDescription(SignalRole role) const;

    QVariantMap createErrorResult(const QString& errorMessage);

private:
    // Service dependencies
    DatabaseManager* m_dbManager;
    InterlockingRuleEngine* m_ruleEngine;
    
    // Operational state
    bool m_isOperational = false;
    bool m_isInitialized = false;
    
    // Configuration
    QHash<QString, QString> m_destinationConstraints;      // signalType -> requiredAspect
    QHash<QString, QStringList> m_aspectPriorities;        // signalType -> priority order
    int m_processingTimeoutMs = 100;                        // Max processing time for safety
    int m_maxGraphSize = 50;                                // Prevent excessive graph expansion
    bool m_enableCircularDependencyDetection = true;
    bool m_enablePerformanceMonitoring = true;
    
    // Performance tracking
    mutable double m_averageProcessingTimeMs = 0.0;
    mutable QList<double> m_processingTimes;
    mutable int m_totalPropagations = 0;
    mutable int m_successfulPropagations = 0;
    mutable QList<AspectPropagationResult> m_recentResults;
    
    // Performance thresholds
    static constexpr double TARGET_PROCESSING_TIME_MS = 50.0;    // Railway safety target
    static constexpr double WARNING_PROCESSING_TIME_MS = 80.0;   // Performance warning
    static constexpr int PERFORMANCE_HISTORY_SIZE = 100;
    static constexpr int RECENT_RESULTS_SIZE = 20;
    
    // Cache for frequently accessed data
    mutable QHash<QString, ControlNode> m_signalDataCache;
    mutable QHash<QString, QStringList> m_controlRelationshipCache;
    mutable QDateTime m_lastCacheUpdate;
    static constexpr int CACHE_VALIDITY_SECONDS = 30;
};

} // namespace RailFlux::Interlocking
