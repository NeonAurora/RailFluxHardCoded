#include "GraphService.h"
#include "../database/DatabaseManager.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariant>
#include <QElapsedTimer>
#include <QtMath>
#include <algorithm>

namespace RailFlux::Route {

GraphService::GraphService(DatabaseManager* dbManager, QObject* parent)
    : QObject(parent)
    , m_dbManager(dbManager)
{
    if (!m_dbManager) {
        qCritical() << "GraphService: DatabaseManager is null";
        return;
    }

    // Connect to database changes for automatic graph updates
    connect(m_dbManager, &DatabaseManager::connectionStateChanged,
            this, [this](bool connected) {
                if (connected) {
                    loadGraphFromDatabase();
                }
            });
}

GraphService::~GraphService() = default;

bool GraphService::loadGraphFromDatabase() {
    if (!m_dbManager || !m_dbManager->isConnected()) {
        emit graphLoadError("Database not connected");
        return false;
    }

    clearGraph();

    QElapsedTimer timer;
    timer.start();

    try {
        // Load circuit positions first
        if (!loadCircuitPositionsFromDatabase()) {
            emit graphLoadError("Failed to load circuit positions");
            return false;
        }

        // Load edges
        if (!loadEdgesFromDatabase()) {
            emit graphLoadError("Failed to load graph edges");
            return false;
        }

        // Build adjacency map for fast neighbor lookups
        buildAdjacencyMap();

        m_isLoaded = true;

        emit isLoadedChanged();
        emit graphChanged();

        // Validate graph integrity
        QStringList warnings = validateGraphIntegrity();
        if (!warnings.isEmpty()) {
            qWarning() << "GraphService: Graph integrity warnings:";
            for (const QString& warning : warnings) {
                qWarning() << " -" << warning;
            }
        }

        return true;

    } catch (const std::exception& e) {
        QString error = QString("Graph loading failed: %1").arg(e.what());
        qCritical() << "GraphService:" << error;
        emit graphLoadError(error);
        return false;
    }
}

bool GraphService::loadCircuitPositionsFromDatabase() {
    QSqlQuery query(m_dbManager->getDatabase());
    query.prepare(R"(
        SELECT
            tc.circuit_id,
            ts.start_row,
            ts.start_col,
            ts.end_row,
            ts.end_col,
            ts.length_meters,
            tc.is_active
        FROM railway_control.track_circuits tc
        LEFT JOIN railway_control.track_segments ts ON tc.circuit_id = ts.circuit_id
        WHERE tc.is_active = TRUE
        ORDER BY tc.circuit_id
    )");

    if (!query.exec()) {
        qCritical() << "GraphService: Failed to load circuit positions:" << query.lastError().text();
        return false;
    }

    while (query.next()) {
        QString circuitId = query.value("circuit_id").toString();

        QVariantMap circuitData;
        circuitData["start_row"] = query.value("start_row").toDouble();
        circuitData["start_col"] = query.value("start_col").toDouble();
        circuitData["end_row"] = query.value("end_row").toDouble();
        circuitData["end_col"] = query.value("end_col").toDouble();
        circuitData["length_meters"] = query.value("length_meters").toDouble();
        circuitData["is_active"] = query.value("is_active").toBool();

        // Calculate center position for heuristic calculations
        double centerRow = (circuitData["start_row"].toDouble() + circuitData["end_row"].toDouble()) / 2.0;
        double centerCol = (circuitData["start_col"].toDouble() + circuitData["end_col"].toDouble()) / 2.0;
        circuitData["center_row"] = centerRow;
        circuitData["center_col"] = centerCol;

        m_circuitNodes[circuitId] = circuitData;
    }

    return true;
}

bool GraphService::loadEdgesFromDatabase() {
    QSqlQuery query(m_dbManager->getDatabase());
    query.prepare(R"(
        SELECT
            from_circuit_id,
            to_circuit_id,
            side,
            condition_point_machine_id,
            condition_position,
            weight,
            is_active
        FROM railway_control.track_circuit_edges
        WHERE is_active = TRUE
        ORDER BY from_circuit_id, side
    )");

    if (!query.exec()) {
        qCritical() << "GraphService: Failed to load graph edges:" << query.lastError().text();
        return false;
    }

    m_edges.clear();
    while (query.next()) {
        GraphEdge edge;
        edge.fromCircuitId = query.value("from_circuit_id").toString();
        edge.toCircuitId = query.value("to_circuit_id").toString();
        edge.side = query.value("side").toString();
        edge.conditionPmId = query.value("condition_point_machine_id").toString();
        edge.conditionPosition = query.value("condition_position").toString();
        edge.weight = query.value("weight").toDouble();
        edge.isActive = query.value("is_active").toBool();

        // Validate edge references exist in circuit nodes
        if (!m_circuitNodes.contains(edge.fromCircuitId)) {
            qWarning() << "GraphService: Edge references unknown from_circuit:" << edge.fromCircuitId;
            continue;
        }
        if (!m_circuitNodes.contains(edge.toCircuitId)) {
            qWarning() << "GraphService: Edge references unknown to_circuit:" << edge.toCircuitId;
            continue;
        }

        m_edges.append(edge);
    }

    return true;
}

void GraphService::buildAdjacencyMap() {
    m_adjacencyMap.clear();

    for (GraphEdge& edge : m_edges) {
        if (!m_adjacencyMap.contains(edge.fromCircuitId)) {
            m_adjacencyMap[edge.fromCircuitId] = QList<GraphEdge*>();
        }
        m_adjacencyMap[edge.fromCircuitId].append(&edge);
    }
}

void GraphService::clearGraph() {
    m_edges.clear();
    m_circuitNodes.clear();
    m_adjacencyMap.clear();
    m_isLoaded = false;
    emit isLoadedChanged();
    emit graphChanged();
}

QVariantMap GraphService::findRoute(
    const QString& startCircuitId,
    const QString& goalCircuitId,
    const QString& direction,
    const QVariantMap& pointMachineStates,
    int timeoutMs
    ) {
    QElapsedTimer timer;
    timer.start();

    m_totalPathfindingCalls++;

    if (!m_isLoaded) {
        qWarning() << "GraphService: Graph not loaded";
        return QVariantMap{
            {"success", false},
            {"error", "Graph not loaded"},
            {"path", QStringList()},
            {"cost", 0.0},
            {"timeMs", 0.0},
            {"nodesExplored", 0}
        };
    }

    if (startCircuitId == goalCircuitId) {
        return QVariantMap{
            {"success", true},
            {"path", QStringList{startCircuitId}},
            {"cost", 0.0},
            {"timeMs", timer.elapsed()},
            {"nodesExplored", 0}
        };
    }

    // Validate circuit existence
    if (!m_circuitNodes.contains(startCircuitId)) {
        qWarning() << "GraphService: Start circuit not found:" << startCircuitId;
        return QVariantMap{
            {"success", false},
            {"error", QString("Start circuit not found: %1").arg(startCircuitId)},
            {"path", QStringList()},
            {"cost", 0.0},
            {"timeMs", timer.elapsed()},
            {"nodesExplored", 0}
        };
    }

    if (!m_circuitNodes.contains(goalCircuitId)) {
        qWarning() << "GraphService: Goal circuit not found:" << goalCircuitId;
        return QVariantMap{
            {"success", false},
            {"error", QString("Goal circuit not found: %1").arg(goalCircuitId)},
            {"path", QStringList()},
            {"cost", 0.0},
            {"timeMs", timer.elapsed()},
            {"nodesExplored", 0}
        };
    }

    // Perform A* pathfinding
    Direction dir = (direction.toUpper() == "DOWN") ? Direction::DOWN : Direction::UP;

    PathfindingResult result = findPathAStar(startCircuitId, goalCircuitId, dir, pointMachineStates, timeoutMs);

    double totalTimeMs = timer.elapsed();
    m_lastPathfindingTimeMs = totalTimeMs;
    m_totalPathfindingTime += totalTimeMs;

    if (!result.success) {
        qWarning() << "GraphService: Pathfinding failed for"
                   << startCircuitId << "->" << goalCircuitId << ":" << result.error;
    }

    // Performance warning
    if (totalTimeMs > PATHFINDING_WARNING_THRESHOLD_MS) {
        qWarning() << "GraphService: Slow pathfinding:" << totalTimeMs << "ms for"
                   << startCircuitId << "->" << goalCircuitId;
    }

    emit pathfindingCompleted(totalTimeMs, result.success);

    return QVariantMap{
        {"success", result.success},
        {"error", result.error},
        {"path", result.path},
        {"cost", result.totalCost},
        {"timeMs", totalTimeMs},
        {"nodesExplored", result.nodesExplored}
    };
}

GraphService::PathfindingResult GraphService::findPathAStar(
    const QString& start,
    const QString& goal,
    Direction direction,
    const QVariantMap& pmStates,
    int timeoutMs
) const {
    QElapsedTimer timer;
    timer.start();

    PathfindingResult result;
    result.success = false;

    // Priority queue for A* (min-heap by f-cost)
    std::priority_queue<PathfindingNode> openSet;
    QSet<QString> closedSet;
    QHash<QString, PathfindingNode> allNodes;

    // Initialize start node
    PathfindingNode startNode;
    startNode.circuitId = start;
    startNode.gCost = 0.0;
    startNode.hCost = calculateHeuristic(start, goal);
    startNode.parent = "";

    openSet.push(startNode);
    allNodes[start] = startNode;

    while (!openSet.empty() && timer.elapsed() < timeoutMs) {
        // Get node with lowest f-cost
        PathfindingNode current = openSet.top();
        openSet.pop();

        result.nodesExplored++;

        // Check if we've already processed this node
        if (closedSet.contains(current.circuitId)) {
            continue;
        }

        closedSet.insert(current.circuitId);

        // Goal check
        if (current.circuitId == goal) {
            QHash<QString, QString> cameFrom;
            for (auto it = allNodes.begin(); it != allNodes.end(); ++it) {
                if (!it.value().parent.isEmpty()) {
                    cameFrom[it.key()] = it.value().parent;
                }
            }

            result.path = reconstructPath(goal, cameFrom);
            result.totalCost = current.gCost;
            result.success = true;
            result.timeMs = timer.elapsed();
            return result;
        }

        // Prevent infinite exploration
        if (result.nodesExplored > MAX_NODES_EXPLORED) {
            result.error = "Maximum nodes explored limit reached";
            break;
        }

        // Explore neighbors
        QStringList neighbors = getViableNeighbors(current.circuitId, direction, pmStates);
        for (const QString& neighbor : neighbors) {
            if (closedSet.contains(neighbor)) {
                continue;
            }

            // Find the edge to calculate cost
            double edgeCost = 1.0; // Default cost
            if (m_adjacencyMap.contains(current.circuitId)) {
                QString targetSide = directionToSide(direction);
                for (const GraphEdge* edge : m_adjacencyMap[current.circuitId]) {
                    if (edge->toCircuitId == neighbor && edge->side == targetSide && edge->isViable(pmStates)) {
                        edgeCost = edge->weight;
                        break;
                    }
                }
            }

            double tentativeGCost = current.gCost + edgeCost;

            // Check if this path to neighbor is better
            bool isNewNode = !allNodes.contains(neighbor);
            bool isBetterPath = isNewNode || tentativeGCost < allNodes[neighbor].gCost;

            if (isBetterPath) {
                PathfindingNode neighborNode;
                neighborNode.circuitId = neighbor;
                neighborNode.gCost = tentativeGCost;
                neighborNode.hCost = calculateHeuristic(neighbor, goal);
                neighborNode.parent = current.circuitId;

                allNodes[neighbor] = neighborNode;
                openSet.push(neighborNode);
            }
        }
    }

    // Pathfinding failed
    if (timer.elapsed() >= timeoutMs) {
        result.error = QString("Pathfinding timeout (%1ms)").arg(timeoutMs);
    } else if (result.error.isEmpty()) {
        result.error = "No path found";
    }

    result.timeMs = timer.elapsed();
    return result;
}

QStringList GraphService::getViableNeighbors(
    const QString& circuitId,
    Direction direction,
    const QVariantMap& pointMachineStates
    ) const {
    QStringList neighbors;

    QString targetSide = (direction == Direction::UP) ? "RIGHT" : "LEFT";

    for (const auto& edge : m_edges) {
        if (edge.fromCircuitId == circuitId &&
            edge.side == targetSide &&
            edge.isActive) {

            if (isEdgeAccessible(edge, pointMachineStates)) {
                neighbors.append(edge.toCircuitId);
            }
        }
    }

    return neighbors;
}

// Add this helper function to GraphService class
bool GraphService::isEdgeAccessible(const GraphEdge& edge, const QVariantMap& pointMachineStates) const {
    // Unconditional edges are always accessible
    if (edge.conditionPmId.isEmpty()) {
        return true;
    }

    // Check if PM data exists
    if (!pointMachineStates.contains(edge.conditionPmId)) {
        qWarning() << "GraphService: Missing PM data for:" << edge.conditionPmId;
        return false;
    }

    QVariantMap pmData = pointMachineStates[edge.conditionPmId].toMap();
    QString currentPosition = pmData["current_position"].toString();
    bool isMoveable = pmData["is_moveable"].toBool();

    // Edge is accessible if:
    // 1. PM is already in required position, OR
    // 2. PM can be moved to required position
    bool isCurrentPosition = (currentPosition == edge.conditionPosition);
    bool canBeMoved = isMoveable;

    bool accessible = isCurrentPosition || canBeMoved;
    return accessible;
}

QVariantMap GraphService::getEdgeInfo(const QString& fromCircuit, const QString& toCircuit, const QString& side) const {
    QVariantMap edgeInfo;

    for (const auto& edge : m_edges) {
        if (edge.fromCircuitId == fromCircuit &&
            edge.toCircuitId == toCircuit &&
            edge.side == side &&
            edge.isActive) {

            edgeInfo["condition_pm_id"] = edge.conditionPmId;
            edgeInfo["condition_position"] = edge.conditionPosition;
            edgeInfo["weight"] = edge.weight;
            edgeInfo["side"] = edge.side;
            break;
        }
    }

    return edgeInfo;
}

double GraphService::calculateHeuristic(const QString& from, const QString& to) const {
    return getCircuitDistance(from, to);
}

double GraphService::getCircuitDistance(const QString& circuitId1, const QString& circuitId2) const {
    if (!m_circuitNodes.contains(circuitId1) || !m_circuitNodes.contains(circuitId2)) {
        return 1000.0; // Large penalty for unknown circuits
    }

    QVariantMap circuit1 = m_circuitNodes[circuitId1];
    QVariantMap circuit2 = m_circuitNodes[circuitId2];

    double row1 = circuit1["center_row"].toDouble();
    double col1 = circuit1["center_col"].toDouble();
    double row2 = circuit2["center_row"].toDouble();
    double col2 = circuit2["center_col"].toDouble();

    // Euclidean distance
    double deltaRow = row2 - row1;
    double deltaCol = col2 - col1;
    return qSqrt(deltaRow * deltaRow + deltaCol * deltaCol);
}

QStringList GraphService::reconstructPath(
    const QString& goal,
    const QHash<QString, QString>& cameFrom
) const {
    QStringList path;
    QString current = goal;

    while (!current.isEmpty()) {
        path.prepend(current);
        current = cameFrom.value(current, QString());
    }

    return path;
}

QString GraphService::directionToSide(Direction direction) const {
    switch (direction) {
        case Direction::UP: return "RIGHT";
        case Direction::DOWN: return "LEFT";
        default: return "RIGHT";
    }
}

QStringList GraphService::getNeighbors(
    const QString& circuitId,
    const QString& direction,
    const QVariantMap& pointMachineStates
    ) const {
    Direction dir = (direction.toUpper() == "DOWN") ? Direction::DOWN : Direction::UP;
    return getViableNeighbors(circuitId, dir, pointMachineStates);
}

double GraphService::calculatePathWeight(const QStringList& path) const {
    if (path.size() < 2) {
        return 0.0;
    }

    double totalWeight = 0.0;

    for (int i = 0; i < path.size() - 1; ++i) {
        QString from = path[i];
        QString to = path[i + 1];

        // Find edge weight
        double edgeWeight = 1.0; // Default weight
        if (m_adjacencyMap.contains(from)) {
            for (const GraphEdge* edge : m_adjacencyMap[from]) {
                if (edge->toCircuitId == to) {
                    edgeWeight = edge->weight;
                    break;
                }
            }
        }

        totalWeight += edgeWeight;
    }

    return totalWeight;
}

bool GraphService::isCircuitReachable(const QString& circuitId) const {
    return m_circuitNodes.contains(circuitId) && m_circuitNodes[circuitId]["is_active"].toBool();
}

QVariantMap GraphService::getGraphStatistics() const {
    int conditionalEdges = 0;
    int unconditionalEdges = 0;

    for (const GraphEdge& edge : m_edges) {
        if (edge.conditionPmId.isEmpty()) {
            unconditionalEdges++;
        } else {
            conditionalEdges++;
        }
    }

    double successRate = m_totalPathfindingCalls > 0 ?
                        (double)m_successfulPaths / m_totalPathfindingCalls * 100.0 : 0.0;
    double avgTimeMs = m_totalPathfindingCalls > 0 ?
                      m_totalPathfindingTime / m_totalPathfindingCalls : 0.0;

    return QVariantMap{
        {"isLoaded", m_isLoaded},
        {"totalCircuits", m_circuitNodes.size()},
        {"totalEdges", m_edges.size()},
        {"conditionalEdges", conditionalEdges},
        {"unconditionalEdges", unconditionalEdges},
        {"totalPathfindingCalls", m_totalPathfindingCalls},
        {"successfulPaths", m_successfulPaths},
        {"successRate", successRate},
        {"averagePathfindingTimeMs", avgTimeMs},
        {"lastPathfindingTimeMs", m_lastPathfindingTimeMs}
    };
}

QStringList GraphService::validateGraphIntegrity() const {
    QStringList warnings;

    // Check for orphaned circuits
    QSet<QString> referencedCircuits;
    for (const GraphEdge& edge : m_edges) {
        referencedCircuits.insert(edge.fromCircuitId);
        referencedCircuits.insert(edge.toCircuitId);
    }

    for (auto it = m_circuitNodes.begin(); it != m_circuitNodes.end(); ++it) {
        if (!referencedCircuits.contains(it.key())) {
            warnings.append(QString("Orphaned circuit (no edges): %1").arg(it.key()));
        }
    }

    // Check for bidirectional connectivity
    QHash<QString, QSet<QString>> connections;
    for (const GraphEdge& edge : m_edges) {
        connections[edge.fromCircuitId].insert(edge.toCircuitId);
    }

    for (auto it = connections.begin(); it != connections.end(); ++it) {
        for (const QString& neighbor : it.value()) {
            if (!connections.contains(neighbor) || !connections[neighbor].contains(it.key())) {
                warnings.append(QString("Unidirectional connection: %1 → %2").arg(it.key(), neighbor));
            }
        }
    }

    return warnings;
}

void GraphService::refreshGraph() {
    loadGraphFromDatabase();
}

void GraphService::onTrackCircuitEdgeChanged(const QString& edgeId) {
    Q_UNUSED(edgeId)
    // For now, do a full reload. Could be optimized to update specific edge
    refreshGraph();
}

QVariantList GraphService::getAlternativeRoutes(
    const QString& startCircuitId,
    const QString& goalCircuitId,
    const QString& direction,
    const QVariantMap& pointMachineStates,
    int maxAlternatives
) {
    QVariantList alternatives;

    // For now, return just the primary route
    // Future enhancement: implement k-shortest paths algorithm
    QVariantMap primaryRoute = findRoute(startCircuitId, goalCircuitId, direction, pointMachineStates);
    if (primaryRoute["success"].toBool()) {
        alternatives.append(primaryRoute);
    }

    return alternatives;
}

// GraphEdge method implementation
bool GraphEdge::isViable(const QVariantMap& pmStates) const {
    if (conditionPmId.isEmpty()) {
        return isActive; // Unconditional edge
    }

    // Check if point machine is in required position
    if (!pmStates.contains(conditionPmId)) {
        return false; // PM state unknown
    }

    QString currentPosition = pmStates[conditionPmId].toString();
    return isActive && (currentPosition == conditionPosition);
}

} // namespace RailFlux::Route
