#pragma once

#include <QObject>
#include <QVariantMap>
#include <QVariantList>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QSet>
#include <QDebug>
#include <QElapsedTimer>
#include <queue>
#include <vector>
#include <memory>
#include <functional>

// Forward declaration
class DatabaseManager;

namespace RailFlux::Route {

struct PathfindingNode {
    QString circuitId;
    double gCost = 0.0;      // Actual cost from start
    double hCost = 0.0;      // Heuristic cost to goal
    double fCost() const { return gCost + hCost; }
    QString parent;
    
    bool operator<(const PathfindingNode& other) const {
        return fCost() > other.fCost(); // For min-heap
    }
};

struct GraphEdge {
    QString fromCircuitId;
    QString toCircuitId;
    QString side;                    // LEFT/RIGHT
    QString conditionPmId;           // Optional point machine dependency
    QString conditionPosition;       // NORMAL/REVERSE
    double weight = 1.0;
    bool isActive = true;
    
    bool isViable(const QVariantMap& pmStates) const;
};

enum class Direction { UP, DOWN };

class GraphService : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isLoaded READ isLoaded NOTIFY isLoadedChanged)
    Q_PROPERTY(int totalEdges READ totalEdges NOTIFY graphChanged)
    Q_PROPERTY(int totalCircuits READ totalCircuits NOTIFY graphChanged)
    Q_PROPERTY(double lastPathfindingTimeMs READ lastPathfindingTimeMs NOTIFY pathfindingCompleted)

public:
    explicit GraphService(DatabaseManager* dbManager, QObject* parent = nullptr);
    ~GraphService();

    // Properties
    bool isLoaded() const { return m_isLoaded; }
    int totalEdges() const { return m_edges.size(); }
    int totalCircuits() const { return m_circuitNodes.size(); }
    double lastPathfindingTimeMs() const { return m_lastPathfindingTimeMs; }
    QVariantMap getEdgeInfo(const QString& fromCircuit, const QString& toCircuit, const QString& side) const;

    // Main pathfinding API
    Q_INVOKABLE QVariantMap findRoute(
        const QString& startCircuitId,
        const QString& goalCircuitId,
        const QString& direction,
        const QVariantMap& pointMachineStates = QVariantMap(),
        int timeoutMs = 500
    );

    // Graph management
    Q_INVOKABLE bool loadGraphFromDatabase();
    Q_INVOKABLE void clearGraph();
    Q_INVOKABLE bool isCircuitReachable(const QString& circuitId) const;
    
    // Pathfinding utilities
    Q_INVOKABLE QStringList getNeighbors(
        const QString& circuitId, 
        const QString& direction,
        const QVariantMap& pointMachineStates = QVariantMap()
    ) const;
    
    Q_INVOKABLE double calculatePathWeight(const QStringList& path) const;
    Q_INVOKABLE QVariantList getAlternativeRoutes(
        const QString& startCircuitId,
        const QString& goalCircuitId,
        const QString& direction,
        const QVariantMap& pointMachineStates = QVariantMap(),
        int maxAlternatives = 3
    );

    // Graph analysis
    Q_INVOKABLE QVariantMap getGraphStatistics() const;
    Q_INVOKABLE QStringList validateGraphIntegrity() const;

public slots:
    void refreshGraph();
    void onTrackCircuitEdgeChanged(const QString& edgeId);

signals:
    void isLoadedChanged();
    void graphChanged();
    void pathfindingCompleted(double timeMs, bool success);
    void graphLoadError(const QString& error);

private:
    // Core pathfinding implementation
    struct PathfindingResult {
        QStringList path;
        double totalCost = 0.0;
        bool success = false;
        QString error;
        double timeMs = 0.0;
        int nodesExplored = 0;
    };

    PathfindingResult findPathAStar(
        const QString& start,
        const QString& goal,
        Direction direction,
        const QVariantMap& pmStates,
        int timeoutMs
    ) const;

    // Heuristic functions
    double calculateHeuristic(const QString& from, const QString& to) const;
    bool isEdgeAccessible(const GraphEdge& edge, const QVariantMap& pointMachineStates) const;
    double getCircuitDistance(const QString& circuitId1, const QString& circuitId2) const;

    // Graph utilities
    QStringList getViableNeighbors(
        const QString& circuitId,
        Direction direction,
        const QVariantMap& pmStates
    ) const;
    
    QString directionToSide(Direction direction) const;
    
    // Path reconstruction
    QStringList reconstructPath(
        const QString& goal,
        const QHash<QString, QString>& cameFrom
    ) const;

    // Graph loading helpers
    bool loadEdgesFromDatabase();
    bool loadCircuitPositionsFromDatabase();
    void buildAdjacencyMap();

private:
    DatabaseManager* m_dbManager;
    bool m_isLoaded = false;
    double m_lastPathfindingTimeMs = 0.0;

    // Graph data structures
    QList<GraphEdge> m_edges;
    QHash<QString, QVariantMap> m_circuitNodes;  // circuitId -> {position, properties}
    QHash<QString, QList<GraphEdge*>> m_adjacencyMap;  // circuitId -> outgoing edges
    
    // Performance settings
    static constexpr int DEFAULT_TIMEOUT_MS = 500;
    static constexpr int MAX_NODES_EXPLORED = 1000;
    static constexpr double PATHFINDING_WARNING_THRESHOLD_MS = 100.0;
    
    // Pathfinding statistics
    mutable int m_totalPathfindingCalls = 0;
    mutable int m_successfulPaths = 0;
    mutable double m_totalPathfindingTime = 0.0;
};

} // namespace RailFlux::Route
