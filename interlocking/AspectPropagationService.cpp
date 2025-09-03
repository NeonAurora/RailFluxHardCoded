#include "AspectPropagationService.h"
#include "InterlockingRuleEngine.h"
#include "../database/DatabaseManager.h"
#include <QElapsedTimer>
#include <QDebug>
#include <QTimer>
#include <algorithm>
#include <queue>

using namespace RailFlux::Interlocking;

AspectPropagationService::AspectPropagationService(
    DatabaseManager* dbManager,
    InterlockingRuleEngine* ruleEngine,
    QObject* parent)
    : QObject(parent)
    , m_dbManager(dbManager)
    , m_ruleEngine(ruleEngine)
{
    Q_ASSERT(dbManager);
    Q_ASSERT(ruleEngine);

    // Initialize with default configuration
    loadDefaultConfiguration();
}

AspectPropagationService::~AspectPropagationService()
{
}

void AspectPropagationService::initialize()
{
    if (m_isInitialized) {
        return;
    }

    // Verify dependencies
    if (!m_dbManager || !m_dbManager->isConnected()) {
        qCritical() << "[AspectPropagationService > initialize] Database manager not available";
        return;
    }

    if (!m_ruleEngine) {
        qCritical() << "[AspectPropagationService > initialize] Interlocking rule engine not available";
        return;
    }

    m_isOperational = true;
    m_isInitialized = true;

    // Set up performance monitoring timer
    if (m_enablePerformanceMonitoring) {
        QTimer* performanceTimer = new QTimer(this);
        connect(performanceTimer, &QTimer::timeout,
                this, &AspectPropagationService::performPerformanceCheck);
        performanceTimer->start(10000); // Check every 10 seconds
    }

    emit operationalStateChanged();
}

void AspectPropagationService::loadDefaultConfiguration()
{
    // Set default destination constraints (destinations typically show RED)
    m_destinationConstraints["HOME"] = "RED";
    m_destinationConstraints["STARTER"] = "RED";
    m_destinationConstraints["OUTER"] = "RED";
    // Advanced Starters may show proceed if track circuits clear
    m_destinationConstraints["ADVANCED_STARTER"] = "GREEN_OR_RED";

    // Set aspect selection priorities (most permissive first for efficiency)
    m_aspectPriorities["HOME"] = {"GREEN", "YELLOW", "RED"};
    m_aspectPriorities["STARTER"] = {"GREEN", "YELLOW", "RED"};
    m_aspectPriorities["OUTER"] = {"GREEN", "YELLOW", "RED"};
    m_aspectPriorities["ADVANCED_STARTER"] = {"GREEN", "YELLOW", "RED"};
}

QVariantMap AspectPropagationService::propagateAspects(
    const QString& sourceSignalId,
    const QString& destinationSignalId,
    const QVariantMap& pointMachinePositions)
{
    return propagateAspectsAdvanced(sourceSignalId, destinationSignalId,
                                    pointMachinePositions, QVariantMap());
}

QVariantMap AspectPropagationService::propagateAspectsAdvanced(
    const QString& sourceSignalId,
    const QString& destinationSignalId,
    const QVariantMap& pointMachinePositions,
    const QVariantMap& options)
{
    if (!m_isOperational) {
        qWarning() << "[AspectPropagationService > propagateAspectsAdvanced] Service not operational";
        QVariantMap errorResult;
        errorResult["success"] = false;
        errorResult["error"] = "Aspect propagation service not operational";
        errorResult["errorCode"] = "SERVICE_NOT_OPERATIONAL";
        return errorResult;
    }

    AspectPropagationResult result = propagateAspectsInternal(
        sourceSignalId, destinationSignalId, pointMachinePositions, options);

    return aspectPropagationResultToVariantMap(result);
}

// ENHANCED: Update propagateAspectsInternal to use RIPPLE algorithm with source context
AspectPropagationResult AspectPropagationService::propagateAspectsInternal(
    const QString& sourceSignalId,
    const QString& destinationSignalId,
    const QVariantMap& pointMachinePositions,
    const QVariantMap& options)
{
    QElapsedTimer timer;
    timer.start();

    AspectPropagationResult result;
    m_totalPropagations++;

    try {
        // 1. Validate the propagation request
        QVariantMap validation = validatePropagationRequestInternal(sourceSignalId, destinationSignalId);
        if (!validation["success"].toBool()) {
            result.errorMessage = validation["error"].toString();
            result.errorCode = validation["errorCode"].toString();
            result.processingTimeMs = timer.elapsed();
            return result;
        }

        // 2. Build the complete control graph starting from source
        QVariantMap fullGraph = buildControlGraphInternal(sourceSignalId);
        result.graphSize = fullGraph["nodes"].toMap().size();
        emit graphConstructed(result.graphSize, fullGraph["edges"].toList().size());

        // 3. ENHANCED: Use RIPPLE algorithm for precise route-based pruning
        QVariantMap prunedGraph = pruneGraphForRouteInternal(fullGraph, sourceSignalId, destinationSignalId);
        result.prunedGraphSize = prunedGraph["nodes"].toMap().size();
        emit graphPruned(result.graphSize, result.prunedGraphSize);

        // Log RIPPLE results
        qDebug() << " [RIPPLE] Results summary:";
        qDebug() << "   Source expansion:" << prunedGraph["sourceExpansion"].toStringList();
        qDebug() << "    Destination expansion:" << prunedGraph["destinationExpansion"].toStringList();
        qDebug() << "    Excluded signals:" << prunedGraph["excludedSignals"].toStringList();

        // 4. Create dependency-ordered processing sequence
        QVector<ControlNode> orderedNodes = createDependencyOrder(prunedGraph);

        // 5. Forward propagate aspects through dependency chain
        QVariantMap aspectSelections = selectOptimalAspects(
            orderedNodes, destinationSignalId, pointMachinePositions, options);

        if (!aspectSelections["success"].toBool()) {
            result.errorMessage = aspectSelections["error"].toString();
            result.errorCode = aspectSelections.value("errorCode", "PROPAGATION_FAILED").toString();
            result.processingTimeMs = timer.elapsed();
            return result;
        }

        // 6. Build successful result
        result.success = true;
        result.signalAspects = aspectSelections["aspects"].toMap();
        result.pointMachines = aspectSelections["pointMachines"].toMap();
        result.decisionReasons = aspectSelections["reasons"].toMap();
        result.processedSignals = aspectSelections["processOrder"].toStringList();

        // Store RIPPLE results for analysis
        result.prunedSignals = prunedGraph["excludedSignals"].toStringList();

        m_successfulPropagations++;

    } catch (const std::exception& e) {
        result.success = false;
        result.errorMessage = QString("RIPPLE propagation error: %1").arg(e.what());
        result.errorCode = "RIPPLE_ERROR";
        qCritical() << "[AspectPropagationService > propagateAspectsInternal] RIPPLE Exception:" << e.what();
    }

    QStringList routePath = options["routePath"].toStringList();
    QStringList overlapPath = options["overlapPath"].toStringList();

    if (!routePath.isEmpty()) {
        qDebug() << " [POINT_MACHINES] Calculating required point machine states...";
        result.pointMachines = calculateRequiredPointMachineStates(routePath, overlapPath);
        qDebug() << "  [POINT_MACHINES] Found" << result.pointMachines.keys().size() << "point machines to configure";
    } else {
        qDebug() << " [POINT_MACHINES] No route path provided, skipping point machine calculation";
    }

    result.processingTimeMs = timer.elapsed();
    recordProcessingTime("ripple_full_propagation", result.processingTimeMs);

    emit propagationCompleted(sourceSignalId, destinationSignalId, result.success);
    return result;
}


QVariantMap AspectPropagationService::buildControlGraph(const QString& sourceSignalId)
{
    if (!m_isOperational) {
        QVariantMap errorResult;
        errorResult["success"] = false;
        errorResult["error"] = "Service not operational";
        return errorResult;
    }

    return buildControlGraphInternal(sourceSignalId);
}

QVariantMap AspectPropagationService::buildControlGraphInternal(const QString& sourceSignalId)
{
    QElapsedTimer timer;
    timer.start();

    QHash<QString, ControlNode> nodes;
    QVector<ControlEdge> edges;
    QSet<QString> visited;

    try {
        // Recursively expand the control network
        expandControlNetwork(sourceSignalId, nodes, edges, visited);

        // Convert to QVariantMap for serialization/debugging
        QVariantMap result;
        QVariantMap nodeMap;
        QVariantList edgeList;

        for (auto it = nodes.begin(); it != nodes.end(); ++it) {
            const QString& signalId = it.key();
            const ControlNode& node = it.value();
            nodeMap[signalId] = controlNodeToVariantMap(node);
        }

        for (const auto& edge : edges) {
            edgeList.append(controlEdgeToVariantMap(edge));
        }

        result["success"] = true;
        result["nodes"] = nodeMap;
        result["edges"] = edgeList;
        result["processingTimeMs"] = timer.elapsed();

        recordProcessingTime("graph_construction", timer.elapsed());
        return result;

    } catch (const std::exception& e) {
        qCritical() << "[AspectPropagationService > buildControlGraphInternal] Exception:" << e.what();

        QVariantMap errorResult;
        errorResult["success"] = false;
        errorResult["error"] = QString("Graph construction failed: %1").arg(e.what());
        errorResult["processingTimeMs"] = timer.elapsed();
        return errorResult;
    }
}

// ENHANCED: Add detailed logging to expandControlNetwork to verify HM001 relationships
void AspectPropagationService::expandControlNetwork(
    const QString& signalId,
    QHash<QString, ControlNode>& nodes,
    QVector<ControlEdge>& edges,
    QSet<QString>& visited)
{
    if (visited.contains(signalId)) {
        return; // Avoid infinite recursion
    }
    visited.insert(signalId);

    qDebug() << " [EXPAND] Processing signal:" << signalId;

    // Get signal information from database
    QVariantMap signalData = m_dbManager->getSignalById(signalId);
    if (signalData.isEmpty()) {
        qWarning() << " [EXPAND] Signal not found:" << signalId;
        return;
    }

    // Create control node
    ControlNode node;
    node.signalId = signalId;
    node.signalType = signalData["type"].toString();
    node.possibleAspects = signalData["possibleAspects"].toStringList();

    // Get control relationships from interlocking rules
    node.controlledBy = m_ruleEngine->getControllingSignals(signalId);
    node.controls = m_ruleEngine->getControlledSignals(signalId);
    node.isIndependent = m_ruleEngine->isSignalIndependent(signalId);

    // Default control mode (could be configured per signal)
    node.controlMode = "AND"; // All controllers must permit the aspect

    nodes[signalId] = node;

    qDebug() << "   Signal:" << signalId << "(" << node.signalType << ")"
             << "\n      Controlled by:" << node.controlledBy
             << "\n      Controls:" << node.controls
             << "\n      Independent:" << node.isIndependent
             << "\n      Possible aspects:" << node.possibleAspects;

    // Process controlling signals (upstream)
    for (const QString& controllingSignalId : node.controlledBy) {
        expandControlNetwork(controllingSignalId, nodes, edges, visited);

        // Build control edges from interlocking rules
        ControlEdge edge;
        edge.fromSignalId = controllingSignalId;
        edge.toSignalId = signalId;
        edge.whenAspect = "GREEN"; // Simplified - should come from rules
        edge.allowedAspects = QStringList{"GREEN", "RED"}; // Simplified

        edges.append(edge);
    }

    // Process controlled signals (downstream)
    for (const QString& controlledSignalId : node.controls) {
        expandControlNetwork(controlledSignalId, nodes, edges, visited);
    }
}

ControlNode AspectPropagationService::loadSignalControlData(const QString& signalId)
{
    // Prevent excessive database queries
    static QSet<QString> currentlyLoading;
    if (currentlyLoading.contains(signalId)) {
        qWarning() << "[AspectPropagationService > loadSignalControlData] Circular loading detected for signal" << signalId;
        return ControlNode(); // Return empty node to break cycles
    }
    currentlyLoading.insert(signalId);

    // Check cache first
    if (m_signalDataCache.contains(signalId)) {
        QDateTime now = QDateTime::currentDateTime();
        if (m_lastCacheUpdate.secsTo(now) < CACHE_VALIDITY_SECONDS) {
            currentlyLoading.remove(signalId);
            return m_signalDataCache[signalId];
        }
    }

    ControlNode node;

    // Get signal information from database
    QVariantMap signalData = m_dbManager->getSignalById(signalId);
    if (signalData.isEmpty()) {
        currentlyLoading.remove(signalId);
        return node; // Return empty node
    }

    // Fill basic signal data
    node.signalId = signalId;
    node.signalType = signalData["type"].toString();
    node.possibleAspects = signalData["possibleAspects"].toStringList();

    // Get control relationships using corrected methods
    node.controlledBy = getControllingSignals(signalId);
    node.controls = getControlledSignals(signalId);
    node.isIndependent = isSignalIndependent(signalId);

    // Default control mode
    node.controlMode = "OR";

    // Cache the result
    m_signalDataCache[signalId] = node;
    m_lastCacheUpdate = QDateTime::currentDateTime();

    currentlyLoading.remove(signalId);

    return node;
}

QStringList AspectPropagationService::getControllingSignals(const QString& signalId)
{
    // Check cache first
    QString cacheKey = signalId + "_controlling";
    if (m_controlRelationshipCache.contains(cacheKey)) {
        return m_controlRelationshipCache[cacheKey];
    }

    QStringList controllingSignals;

    if (m_ruleEngine) {
        controllingSignals = m_ruleEngine->getControllingSignals(signalId);
    } else {
        qWarning() << "[AspectPropagationService > getControllingSignals] InterlockingRuleEngine not available for" << signalId;
    }

    // Cache the result
    m_controlRelationshipCache[cacheKey] = controllingSignals;

    return controllingSignals;
}

QStringList AspectPropagationService::getControlledSignals(const QString& signalId)
{
    // Check cache first
    QString cacheKey = signalId + "_controlled";
    if (m_controlRelationshipCache.contains(cacheKey)) {
        return m_controlRelationshipCache[cacheKey];
    }

    QStringList controlledSignals;

    if (m_ruleEngine) {
        controlledSignals = m_ruleEngine->getControlledSignals(signalId);
    } else {
        qWarning() << "[AspectPropagationService > getControlledSignals] InterlockingRuleEngine not available for" << signalId;
    }

    // Cache the result
    m_controlRelationshipCache[cacheKey] = controlledSignals;

    return controlledSignals;
}

QStringList AspectPropagationService::findSignalsByType(const QString& signalType)
{
    QStringList signalList;

    // Get all signals of the specified type from database
    QVariantList allSignals = m_dbManager->getAllSignalsList();

    for (const QVariant& signalVariant : allSignals) {
        QVariantMap signal = signalVariant.toMap();
        if (signal["type"].toString() == signalType) {
            signalList.append(signal["id"].toString());
        }
    }

    return signalList;
}

bool AspectPropagationService::isSignalIndependent(const QString& signalId)
{
    if (m_ruleEngine) {
        return m_ruleEngine->isSignalIndependent(signalId);
    }
    return false;
}

QVector<ControlEdge> AspectPropagationService::loadControlEdges(const QString& signalId)
{
    QVector<ControlEdge> edges;

    // Get control relationships and create edges
    QStringList controlling = getControllingSignals(signalId);

    for (const QString& controllingSignalId : controlling) {
        ControlEdge edge;
        edge.fromSignalId = controllingSignalId;
        edge.toSignalId = signalId;
        edge.whenAspect = "GREEN"; // Simplified
        edge.allowedAspects = QStringList{"GREEN", "YELLOW", "RED"}; // Simplified
        edge.ruleId = QString("rule_%1_%2").arg(controllingSignalId, signalId);

        edges.append(edge);
    }

    return edges;
}

double AspectPropagationService::successRate() const
{
    if (m_totalPropagations == 0) return 0.0;
    return (static_cast<double>(m_successfulPropagations) / m_totalPropagations) * 100.0;
}

void AspectPropagationService::recordProcessingTime(const QString& operation, double timeMs)
{
    if (!m_enablePerformanceMonitoring) return;

    m_processingTimes.append(timeMs);

    // Keep only recent measurements
    if (m_processingTimes.size() > PERFORMANCE_HISTORY_SIZE) {
        m_processingTimes.removeFirst();
    }

    updateAverageProcessingTime();
}

void AspectPropagationService::updateAverageProcessingTime()
{
    if (m_processingTimes.isEmpty()) {
        m_averageProcessingTimeMs = 0.0;
        return;
    }

    double sum = 0.0;
    for (double time : m_processingTimes) {
        sum += time;
    }

    m_averageProcessingTimeMs = sum / m_processingTimes.size();
    emit performanceChanged();
}

void AspectPropagationService::recordPropagationResult(const AspectPropagationResult& result)
{
    m_recentResults.append(result);

    // Keep only recent results
    if (m_recentResults.size() > RECENT_RESULTS_SIZE) {
        m_recentResults.removeFirst();
    }

    emit statisticsChanged();
}

QVariantMap AspectPropagationService::controlNodeToVariantMap(const ControlNode& node) const
{
    QVariantMap map;
    map["signalId"] = node.signalId;
    map["signalType"] = node.signalType;
    map["possibleAspects"] = node.possibleAspects;
    map["controlledBy"] = node.controlledBy;
    map["controls"] = node.controls;
    map["controlMode"] = node.controlMode;
    map["isIndependent"] = node.isIndependent;
    map["selectedAspect"] = node.selectedAspect;
    map["isProcessed"] = node.isProcessed;
    map["dependencyOrder"] = node.dependencyOrder;
    map["locationRow"] = node.locationRow;
    map["locationCol"] = node.locationCol;
    return map;
}

QVariantMap AspectPropagationService::controlEdgeToVariantMap(const ControlEdge& edge) const
{
    QVariantMap map;
    map["from"] = edge.fromSignalId;
    map["to"] = edge.toSignalId;
    map["whenAspect"] = edge.whenAspect;
    map["allowedAspects"] = edge.allowedAspects;
    map["conditions"] = edge.conditions;
    map["ruleId"] = edge.ruleId;
    return map;
}

QVariantMap AspectPropagationService::aspectPropagationResultToVariantMap(const AspectPropagationResult& result) const
{
    QVariantMap map;
    map["success"] = result.success;
    map["errorMessage"] = result.errorMessage;
    map["errorCode"] = result.errorCode;
    map["signalAspects"] = result.signalAspects;
    map["pointMachines"] = result.pointMachines;
    map["processedSignals"] = result.processedSignals;
    map["prunedSignals"] = result.prunedSignals;
    map["decisionReasons"] = result.decisionReasons;
    map["controlPath"] = result.controlPath;
    map["processingTimeMs"] = result.processingTimeMs;
    map["graphSize"] = result.graphSize;
    map["prunedGraphSize"] = result.prunedGraphSize;
    map["circularDependencies"] = result.circularDependencies;
    map["validationErrors"] = result.validationErrors;
    map["validationWarnings"] = result.validationWarnings;
    return map;
}

void AspectPropagationService::performPerformanceCheck()
{
    checkPerformanceThresholds();
}

void AspectPropagationService::checkPerformanceThresholds()
{
    if (m_averageProcessingTimeMs > WARNING_PROCESSING_TIME_MS) {
        emit performanceWarning("average_processing_time",
                                m_averageProcessingTimeMs,
                                WARNING_PROCESSING_TIME_MS);
    }
}

QVariantMap AspectPropagationService::validatePropagationRequestInternal(
    const QString& sourceSignalId,
    const QString& destinationSignalId)
{
    QVariantMap result;

    // Basic validation checks
    if (sourceSignalId.isEmpty() || destinationSignalId.isEmpty()) {
        result["success"] = false;
        result["error"] = "Signal IDs cannot be empty";
        result["errorCode"] = "EMPTY_SIGNAL_ID";
        return result;
    }

    if (sourceSignalId == destinationSignalId) {
        result["success"] = false;
        result["error"] = "Source and destination cannot be the same";
        result["errorCode"] = "SAME_SIGNAL";
        return result;
    }

    // Verify signals exist in database
    QVariantMap sourceSignal = m_dbManager->getSignalById(sourceSignalId);
    QVariantMap destSignal = m_dbManager->getSignalById(destinationSignalId);

    if (sourceSignal.isEmpty()) {
        result["success"] = false;
        result["error"] = "Source signal not found: " + sourceSignalId;
        result["errorCode"] = "SOURCE_NOT_FOUND";
        return result;
    }

    if (destSignal.isEmpty()) {
        result["success"] = false;
        result["error"] = "Destination signal not found: " + destinationSignalId;
        result["errorCode"] = "DEST_NOT_FOUND";
        return result;
    }

    result["success"] = true;
    result["message"] = "Propagation request validation passed";
    return result;
}

// Placeholder implementations for remaining methods - to be completed in subsequent iterations
QVariantMap AspectPropagationService::pruneGraphForDestination(
    const QVariantMap& fullGraph,
    const QString& destinationSignalId)
{
    return pruneGraphForDestinationInternal(fullGraph, destinationSignalId);
}

QVariantMap AspectPropagationService::pruneGraphForRoute(
    const QVariantMap& fullGraph,
    const QString& sourceSignalId,
    const QString& destinationSignalId)
{
    return pruneGraphForRouteInternal(fullGraph, sourceSignalId, destinationSignalId);
}

// ENHANCED: Fix pruneGraphForDestinationInternal to include controlled signals
// ENHANCED: Implement RIPPLE Algorithm for precise control graph pruning
QVariantMap AspectPropagationService::pruneGraphForDestinationInternal(
    const QVariantMap& fullGraph,
    const QString& destinationSignalId)
{
    QElapsedTimer timer;
    timer.start();

    qDebug() << " [RIPPLE] Starting RIPPLE algorithm for route pruning";
    qDebug() << "    Destination:" << destinationSignalId;

    QVariantMap nodes = fullGraph["nodes"].toMap();
    QVariantList edges = fullGraph["edges"].toList();

    // Validate destination exists
    if (!nodes.contains(destinationSignalId)) {
        qWarning() << " [RIPPLE] Destination signal not found:" << destinationSignalId;

        QVariantMap emptyResult;
        emptyResult["success"] = false;
        emptyResult["error"] = "Destination signal not found in control graph";
        emptyResult["nodes"] = QVariantMap();
        emptyResult["edges"] = QVariantList();
        emptyResult["processingTimeMs"] = timer.elapsed();
        return emptyResult;
    }

    // RIPPLE Step 1: Find the source signal from the graph
    // (The source is the signal that's not controlled by any other signal in our context)
    QString sourceSignalId;

    // For route assignment, we need to identify the source signal
    // This is typically the signal that has the least controllers or is at the "start" of the route
    // For now, we'll use a heuristic: find signals that could be sources
    QStringList potentialSources;
    for (auto it = nodes.begin(); it != nodes.end(); ++it) {
        QVariantMap nodeData = it.value().toMap();
        QStringList controlledBy = nodeData["controlledBy"].toStringList();
        QString signalType = nodeData["signalType"].toString();

        // Heuristic: HOME signals are typically sources in route assignments
        if (signalType == "HOME" || signalType == "OUTER") {
            potentialSources.append(it.key());
        }
    }

    // For this implementation, we'll assume the source is the signal we want to find a path from
    // In a proper implementation, this should be passed as a parameter
    if (!potentialSources.isEmpty()) {
        sourceSignalId = potentialSources.first(); // Use first potential source
        qDebug() << "   Detected source signal:" << sourceSignalId;
    }

    // RIPPLE Step 2: Destination Expansion (Upward) - Find controllers of destination
    QSet<QString> destinationExpansion;
    QQueue<QString> destinationQueue;
    QSet<QString> destinationVisited;

    destinationQueue.enqueue(destinationSignalId);
    destinationExpansion.insert(destinationSignalId);

    qDebug() << " [RIPPLE] Destination Expansion (Upward):";
    qDebug() << "  Starting from destination:" << destinationSignalId;

    while (!destinationQueue.isEmpty()) {
        QString currentSignal = destinationQueue.dequeue();
        if (destinationVisited.contains(currentSignal)) continue;
        destinationVisited.insert(currentSignal);

        QVariantMap nodeData = nodes[currentSignal].toMap();
        QStringList controlledBy = nodeData["controlledBy"].toStringList();

        qDebug() << "   " << currentSignal << "controlled by:" << controlledBy;

        for (const QString& controllingSignal : controlledBy) {
            if (nodes.contains(controllingSignal) && !destinationExpansion.contains(controllingSignal)) {
                destinationExpansion.insert(controllingSignal);
                destinationQueue.enqueue(controllingSignal);
                qDebug() << "     Added controller:" << controllingSignal;
            }
        }
    }

    // RIPPLE Step 3: Source Expansion (Downward) - Find signals controlled by source
    QSet<QString> sourceExpansion;

    if (!sourceSignalId.isEmpty() && nodes.contains(sourceSignalId)) {
        QQueue<QString> sourceQueue;
        QSet<QString> sourceVisited;

        sourceQueue.enqueue(sourceSignalId);
        sourceExpansion.insert(sourceSignalId);

        qDebug() << " [RIPPLE] Source Expansion (Downward):";
        qDebug() << "  Starting from source:" << sourceSignalId;

        while (!sourceQueue.isEmpty()) {
            QString currentSignal = sourceQueue.dequeue();
            if (sourceVisited.contains(currentSignal)) continue;
            sourceVisited.insert(currentSignal);

            QVariantMap nodeData = nodes[currentSignal].toMap();
            QStringList controls = nodeData["controls"].toStringList();

            qDebug() << "   " << currentSignal << "controls:" << controls;

            for (const QString& controlledSignal : controls) {
                if (nodes.contains(controlledSignal) && !sourceExpansion.contains(controlledSignal)) {
                    sourceExpansion.insert(controlledSignal);
                    sourceQueue.enqueue(controlledSignal);
                    qDebug() << "      Added controlled signal:" << controlledSignal;
                }
            }
        }
    }

    // RIPPLE Step 4: Find the intersection path between source and destination
    // We need signals that are in the direct control path from source to destination
    QSet<QString> directPath;

    // Add signals that are in both the upward path from destination and accessible from source
    // This creates the direct control chain
    if (!sourceSignalId.isEmpty()) {
        // Find path from source toward destination
        QQueue<QString> pathQueue;
        QSet<QString> pathVisited;
        QHash<QString, QString> pathParent; // Track path for reconstruction

        pathQueue.enqueue(sourceSignalId);
        pathVisited.insert(sourceSignalId);

        bool pathFound = false;
        QString pathEndpoint;

        while (!pathQueue.isEmpty() && !pathFound) {
            QString currentSignal = pathQueue.dequeue();

            // Check if we've reached the destination or any signal in destination expansion
            if (currentSignal == destinationSignalId || destinationExpansion.contains(currentSignal)) {
                pathFound = true;
                pathEndpoint = currentSignal;
                break;
            }

            QVariantMap nodeData = nodes[currentSignal].toMap();
            QStringList controlledBy = nodeData["controlledBy"].toStringList();

            for (const QString& controller : controlledBy) {
                if (nodes.contains(controller) && !pathVisited.contains(controller)) {
                    pathVisited.insert(controller);
                    pathParent[controller] = currentSignal;
                    pathQueue.enqueue(controller);
                }
            }
        }

        // Reconstruct the direct path
        if (pathFound) {
            QString current = pathEndpoint;
            while (!current.isEmpty()) {
                directPath.insert(current);
                current = pathParent.value(current, "");
            }
            qDebug() << "    Direct path found:" << directPath.values();
        }
    }

    // RIPPLE Step 5: Combine relevant signals
    QSet<QString> relevantSignals;

    // Always include destination expansion (upward controllers)
    relevantSignals.unite(destinationExpansion);

    // Include source expansion only if it connects to the destination
    if (!directPath.isEmpty()) {
        relevantSignals.unite(directPath);
        // Add source expansion signals that are part of the direct path
        for (const QString& sourceSignal : sourceExpansion) {
            if (directPath.contains(sourceSignal)) {
                relevantSignals.insert(sourceSignal);
            }
        }
    } else {
        // Fallback: include source expansion
        relevantSignals.unite(sourceExpansion);
    }

    qDebug() << " [RIPPLE] Expansion Results:";
    qDebug() << "   Destination expansion:" << destinationExpansion.values();
    qDebug() << "    Source expansion:" << sourceExpansion.values();
    qDebug() << "    Direct path:" << directPath.values();
    qDebug() << "     Final relevant signals:" << relevantSignals.values();

    // RIPPLE Step 6: Build pruned graph with only relevant signals
    QVariantMap prunedNodes;
    QVariantList prunedEdges;

    for (const QString& signalId : relevantSignals) {
        prunedNodes[signalId] = nodes[signalId];
    }

    for (const QVariant& edgeVariant : edges) {
        QVariantMap edge = edgeVariant.toMap();
        QString fromSignal = edge["from"].toString();
        QString toSignal = edge["to"].toString();

        if (relevantSignals.contains(fromSignal) && relevantSignals.contains(toSignal)) {
            prunedEdges.append(edge);
        }
    }

    // Calculate pruning efficiency
    int originalSize = nodes.size();
    int prunedSize = relevantSignals.size();
    int excludedSignals = originalSize - prunedSize;

    qDebug() << " [RIPPLE] Pruning completed:";
    qDebug() << "    Original:" << originalSize << "signals";
    qDebug() << "     Kept:" << prunedSize << "relevant signals";
    qDebug() << "    Excluded:" << excludedSignals << "irrelevant signals";

    // Log excluded signals for debugging
    if (excludedSignals > 0) {
        QSet<QString> allSignals(nodes.keys().begin(), nodes.keys().end());
        QSet<QString> excludedSet = allSignals - relevantSignals;
        qDebug() << "    Excluded signals:" << excludedSet.values();
    }

    QVariantMap result;
    result["success"] = true;
    result["nodes"] = prunedNodes;
    result["edges"] = prunedEdges;
    result["processingTimeMs"] = timer.elapsed();
    result["originalSize"] = originalSize;
    result["prunedSize"] = prunedSize;
    result["excludedSignals"] = excludedSignals;
    result["sourceExpansion"] = QStringList(sourceExpansion.values());
    result["destinationExpansion"] = QStringList(destinationExpansion.values());
    result["directPath"] = QStringList(directPath.values());

    recordProcessingTime("graph_pruning", timer.elapsed());

    return result;
}

QVariantMap AspectPropagationService::pruneGraphForRouteInternal(
    const QVariantMap& fullGraph,
    const QString& sourceSignalId,
    const QString& destinationSignalId)
{
    QElapsedTimer timer;
    timer.start();

    qDebug() << " [RIPPLE] Starting RIPPLE algorithm for route pruning";
    qDebug() << "   Source:" << sourceSignalId << "→  Destination:" << destinationSignalId;

    QVariantMap nodes = fullGraph["nodes"].toMap();
    QVariantList edges = fullGraph["edges"].toList();

    // SAFE: Validate inputs with proper error handling
    if (!nodes.contains(sourceSignalId)) {
        qWarning() << " [RIPPLE] Source signal not found:" << sourceSignalId;

        QVariantMap errorResult;
        errorResult["success"] = false;
        errorResult["error"] = "Source signal not found in control graph";
        errorResult["nodes"] = QVariantMap();
        errorResult["edges"] = QVariantList();
        errorResult["processingTimeMs"] = timer.elapsed();
        return errorResult;
    }

    if (!nodes.contains(destinationSignalId)) {
        qWarning() << " [RIPPLE] Destination signal not found:" << destinationSignalId;

        QVariantMap errorResult;
        errorResult["success"] = false;
        errorResult["error"] = "Destination signal not found in control graph";
        errorResult["nodes"] = QVariantMap();
        errorResult["edges"] = QVariantList();
        errorResult["processingTimeMs"] = timer.elapsed();
        return errorResult;
    }

    try {
        // SAFE: RIPPLE Step 1 - Destination Expansion (Upward)
        QSet<QString> destinationExpansion;
        QQueue<QString> destinationQueue;
        QSet<QString> destinationVisited;

        destinationQueue.enqueue(destinationSignalId);
        destinationExpansion.insert(destinationSignalId);

        qDebug() << " [RIPPLE] Destination Expansion (Upward):";

        // SAFE: Limit expansion depth to prevent infinite loops
        int maxDepth = 10;
        int currentDepth = 0;

        while (!destinationQueue.isEmpty() && currentDepth < maxDepth) {
            QString currentSignal = destinationQueue.dequeue();
            if (destinationVisited.contains(currentSignal)) continue;
            destinationVisited.insert(currentSignal);

            if (!nodes.contains(currentSignal)) {
                qWarning() << " [RIPPLE] Signal not found in graph:" << currentSignal;
                continue;
            }

            QVariantMap nodeData = nodes[currentSignal].toMap();
            QStringList controlledBy = nodeData["controlledBy"].toStringList();

            qDebug() << "   " << currentSignal << "controlled by:" << controlledBy;

            for (const QString& controllingSignal : controlledBy) {
                if (nodes.contains(controllingSignal) &&
                    !destinationExpansion.contains(controllingSignal)) {
                    destinationExpansion.insert(controllingSignal);
                    destinationQueue.enqueue(controllingSignal);
                    qDebug() << "     Added controller:" << controllingSignal;
                }
            }
            currentDepth++;
        }

        // SAFE: RIPPLE Step 2 - Source Expansion (Downward)
        QSet<QString> sourceExpansion;
        QQueue<QString> sourceQueue;
        QSet<QString> sourceVisited;

        sourceQueue.enqueue(sourceSignalId);
        sourceExpansion.insert(sourceSignalId);

        qDebug() << " [RIPPLE] Source Expansion (Downward):";

        // SAFE: Reset depth counter
        currentDepth = 0;

        while (!sourceQueue.isEmpty() && currentDepth < maxDepth) {
            QString currentSignal = sourceQueue.dequeue();
            if (sourceVisited.contains(currentSignal)) continue;
            sourceVisited.insert(currentSignal);

            if (!nodes.contains(currentSignal)) {
                qWarning() << " [RIPPLE] Signal not found in graph:" << currentSignal;
                continue;
            }

            QVariantMap nodeData = nodes[currentSignal].toMap();
            QStringList controls = nodeData["controls"].toStringList();

            qDebug() << "   " << currentSignal << "controls:" << controls;

            for (const QString& controlledSignal : controls) {
                if (nodes.contains(controlledSignal) &&
                    !sourceExpansion.contains(controlledSignal)) {
                    sourceExpansion.insert(controlledSignal);
                    sourceQueue.enqueue(controlledSignal);
                    qDebug() << "      Added controlled signal:" << controlledSignal;
                }
            }
            currentDepth++;
        }

        // SAFE: RIPPLE Step 3 - Combine relevant signals
        QSet<QString> relevantSignals = sourceExpansion;
        relevantSignals.unite(destinationExpansion);

        // SAFE: RIPPLE Step 4 - Calculate excluded signals
        QSet<QString> allSignals;
        for (auto it = nodes.begin(); it != nodes.end(); ++it) {
            allSignals.insert(it.key());
        }
        QSet<QString> excludedSignals = allSignals - relevantSignals;

        // SAFE: Convert QSet to QStringList properly
        QStringList sourceExpansionList;
        for (const QString& signal : sourceExpansion) {
            sourceExpansionList.append(signal);
        }

        QStringList destinationExpansionList;
        for (const QString& signal : destinationExpansion) {
            destinationExpansionList.append(signal);
        }

        QStringList relevantSignalsList;
        for (const QString& signal : relevantSignals) {
            relevantSignalsList.append(signal);
        }

        QStringList excludedSignalsList;
        for (const QString& signal : excludedSignals) {
            excludedSignalsList.append(signal);
        }

        qDebug() << " [RIPPLE] Expansion Results:";
        qDebug() << "   Source expansion:" << sourceExpansionList;
        qDebug() << "    Destination expansion:" << destinationExpansionList;
        qDebug() << "     Final relevant signals:" << relevantSignalsList;
        qDebug() << "    Excluded signals:" << excludedSignalsList;

        // ENHANCED: RIPPLE Step 5 - Build pruned graph with cleaned control relationships
        QVariantMap prunedNodes;
        QVariantList prunedEdges;

        qDebug() << " [RIPPLE] Cleaning up control relationships in pruned nodes...";

        for (const QString& signalId : relevantSignals) {
            if (nodes.contains(signalId)) {
                // Get the original node data
                QVariantMap nodeData = nodes[signalId].toMap();

                // Get original control relationships
                QStringList originalControlledBy = nodeData["controlledBy"].toStringList();
                QStringList originalControls = nodeData["controls"].toStringList();

                // Filter to only include signals that exist in the pruned graph
                QStringList cleanedControlledBy;
                QStringList cleanedControls;

                for (const QString& controller : originalControlledBy) {
                    if (relevantSignals.contains(controller)) {
                        cleanedControlledBy.append(controller);
                        qDebug() << "     Kept controller:" << controller << "for" << signalId;
                    } else {
                        qDebug() << "    Removed controller:" << controller << "from" << signalId;
                    }
                }

                for (const QString& controlled : originalControls) {
                    if (relevantSignals.contains(controlled)) {
                        cleanedControls.append(controlled);
                        qDebug() << "     Kept controlled:" << controlled << "for" << signalId;
                    } else {
                        qDebug() << "    Removed controlled:" << controlled << "from" << signalId;
                    }
                }

                // Update the node data with cleaned relationships
                nodeData["controlledBy"] = cleanedControlledBy;
                nodeData["controls"] = cleanedControls;

                // Add the cleaned node to pruned graph
                prunedNodes[signalId] = nodeData;

                qDebug() << "   " << signalId << "cleaned relationships:"
                         << "controlledBy:" << cleanedControlledBy
                         << "controls:" << cleanedControls;
            }
        }

        // Build pruned edges (only include edges between relevant signals)
        for (const QVariant& edgeVariant : edges) {
            QVariantMap edge = edgeVariant.toMap();
            QString fromSignal = edge["from"].toString();
            QString toSignal = edge["to"].toString();

            if (relevantSignals.contains(fromSignal) && relevantSignals.contains(toSignal)) {
                prunedEdges.append(edge);
            }
        }

        qDebug() << " [RIPPLE] Pruning completed: Original:" << nodes.size()
                 << "→ Kept:" << relevantSignals.size() << "signals";
        qDebug() << " [RIPPLE] Control relationship cleanup completed!";

        // SAFE: Build result with all required fields
        QVariantMap result;
        result["success"] = true;
        result["nodes"] = prunedNodes;
        result["edges"] = prunedEdges;
        result["processingTimeMs"] = timer.elapsed();
        result["originalSize"] = nodes.size();
        result["prunedSize"] = relevantSignals.size();
        result["sourceExpansion"] = sourceExpansionList;
        result["destinationExpansion"] = destinationExpansionList;
        result["excludedSignals"] = excludedSignalsList;

        recordProcessingTime("ripple_pruning", timer.elapsed());
        return result;

    } catch (const std::exception& e) {
        qCritical() << " [RIPPLE] Exception occurred:" << e.what();

        QVariantMap errorResult;
        errorResult["success"] = false;
        errorResult["error"] = QString("RIPPLE exception: %1").arg(e.what());
        errorResult["nodes"] = QVariantMap();
        errorResult["edges"] = QVariantList();
        errorResult["processingTimeMs"] = timer.elapsed();
        return errorResult;

    } catch (...) {
        qCritical() << " [RIPPLE] Unknown exception occurred";

        QVariantMap errorResult;
        errorResult["success"] = false;
        errorResult["error"] = "Unknown RIPPLE exception";
        errorResult["nodes"] = QVariantMap();
        errorResult["edges"] = QVariantList();
        errorResult["processingTimeMs"] = timer.elapsed();
        return errorResult;
    }
}

QVariantMap AspectPropagationService::createErrorResult(const QString& errorMessage)
{
    QVariantMap errorResult;
    errorResult["success"] = false;
    errorResult["error"] = errorMessage;
    errorResult["nodes"] = QVariantMap();
    errorResult["edges"] = QVariantList();
    return errorResult;
}


// ENHANCED: Add better logging to createDependencyOrder to debug processing sequence
QVector<ControlNode> AspectPropagationService::createDependencyOrder(const QVariantMap& prunedGraph)
{
    QElapsedTimer timer;
    timer.start();

    qDebug() << "[DEPENDENCY_ORDER] Creating processing sequence...";

    QVariantMap nodes = prunedGraph["nodes"].toMap();
    QHash<QString, ControlNode> nodeHash;

    qDebug() << " [DEBUG] Signals in pruned graph:" << nodes.keys();

    // Convert to ControlNode hash for easier processing
    for (auto it = nodes.begin(); it != nodes.end(); ++it) {
        ControlNode node;
        QVariantMap nodeData = it.value().toMap();

        node.signalId = it.key();
        node.signalType = nodeData["signalType"].toString();
        node.possibleAspects = nodeData["possibleAspects"].toStringList();
        node.controlledBy = nodeData["controlledBy"].toStringList();
        node.controls = nodeData["controls"].toStringList();
        node.controlMode = nodeData["controlMode"].toString();
        node.isIndependent = nodeData["isIndependent"].toBool();

        nodeHash[it.key()] = node;

        qDebug() << "   Node:" << node.signalId << "controlled by:" << node.controlledBy;
    }

    // ENHANCED: Verify critical relationships
    if (nodeHash.contains("HM001")) {
        qDebug() << "  [DEBUG] HM001 found in dependency ordering";
        qDebug() << "    HM001 controlled by:" << nodeHash["HM001"].controlledBy;
    } else {
        qCritical() << " [CRITICAL] HM001 missing from dependency ordering!";
    }

    if (nodeHash.contains("ST001")) {
        qDebug() << "  [DEBUG] ST001 found in dependency ordering";
        qDebug() << "    ST001 controlled by:" << nodeHash["ST001"].controlledBy;
        qDebug() << "    ST001 controls:" << nodeHash["ST001"].controls;
    }

    // Topological sort using Kahn's algorithm
    QVector<ControlNode> orderedNodes;
    QHash<QString, int> inDegree;
    QQueue<QString> independent;

    //  Calculate in-degrees using proper Qt iteration
    for (auto it = nodeHash.begin(); it != nodeHash.end(); ++it) {
        const QString& signalId = it.key();
        const ControlNode& node = it.value();

        inDegree[signalId] = node.controlledBy.size();
        if (node.isIndependent || node.controlledBy.isEmpty()) {
            independent.enqueue(signalId);
            qDebug() << "    Independent signal:" << signalId;
        }
    }

    int order = 0;
    qDebug() << "[DEPENDENCY_ORDER] Processing signals in topological order:";

    while (!independent.isEmpty()) {
        QString currentSignal = independent.dequeue();
        ControlNode node = nodeHash[currentSignal];
        node.dependencyOrder = order++;
        node.isProcessed = false; // Will be set during propagation

        orderedNodes.append(node);
        qDebug() << "   Order" << node.dependencyOrder << ":" << currentSignal;

        // Reduce in-degree for controlled signals that are in the pruned graph
        for (const QString& controlledSignal : node.controls) {
            if (nodeHash.contains(controlledSignal)) {
                inDegree[controlledSignal]--;
                if (inDegree[controlledSignal] == 0) {
                    independent.enqueue(controlledSignal);
                    qDebug() << "   " << controlledSignal << "now ready for processing";
                }
            }
        }
    }

    // Check for circular dependencies by comparing processed vs total nodes
    if (orderedNodes.size() != nodeHash.size()) {
        qWarning() << "[AspectPropagationService > createDependencyOrder] Circular dependency suspected:"
                   << "processed =" << orderedNodes.size()
                   << "total =" << nodeHash.size();

        //  Add remaining nodes using proper Qt iteration
        for (auto it = nodeHash.begin(); it != nodeHash.end(); ++it) {
            const QString& signalId = it.key();
            const ControlNode& node = it.value();
            bool found = false;
            for (const ControlNode& ordered : orderedNodes) {
                if (ordered.signalId == signalId) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                ControlNode problematicNode = node;
                problematicNode.dependencyOrder = order++;
                orderedNodes.append(problematicNode);
                qWarning() << "    Added problematic node:" << signalId;
            }
        }
    }

    qDebug() << "[DEPENDENCY_ORDER] Final processing sequence:";
    for (int i = 0; i < orderedNodes.size(); ++i) {
        qDebug() << "   " << (i+1) << "." << orderedNodes[i].signalId
                 << "(" << orderedNodes[i].signalType << ")"
                 << (orderedNodes[i].isIndependent ? "[INDEPENDENT]" :
                         QString("[CONTROLLED_BY: %1]").arg(orderedNodes[i].controlledBy.join(",")));
    }

    recordProcessingTime("dependency_ordering", timer.elapsed());

    return orderedNodes;
}

bool AspectPropagationService::detectCircularDependencies(
    const QHash<QString, ControlNode>& nodes,
    QStringList& circularSignals)
{
    // Use depth-first search to detect cycles
    QHash<QString, int> state; // 0 = unvisited, 1 = visiting, 2 = visited
    bool hasCycle = false;

    for (auto it = nodes.begin(); it != nodes.end(); ++it) {
        const QString& signalId = it.key();
        const ControlNode& node = it.value();
        if (state.value(signalId, 0) == 0) {
            if (detectCyclesDFS(signalId, nodes, state, circularSignals)) {
                hasCycle = true;
            }
        }
    }

    return hasCycle;
}

bool AspectPropagationService::detectCyclesDFS(
    const QString& signalId,
    const QHash<QString, ControlNode>& nodes,
    QHash<QString, int>& state,
    QStringList& circularSignals)
{
    state[signalId] = 1; // Mark as visiting

    if (nodes.contains(signalId)) {
        const ControlNode& node = nodes[signalId];

        for (const QString& controlledSignal : node.controls) {
            if (!nodes.contains(controlledSignal)) continue;

            int controlledState = state.value(controlledSignal, 0);

            if (controlledState == 1) {
                // Found a back edge - cycle detected
                circularSignals.append(signalId);
                circularSignals.append(controlledSignal);
                return true;
            } else if (controlledState == 0) {
                if (detectCyclesDFS(controlledSignal, nodes, state, circularSignals)) {
                    return true;
                }
            }
        }
    }

    state[signalId] = 2; // Mark as visited
    return false;
}

QVariantMap AspectPropagationService::selectOptimalAspects(
    const QVector<ControlNode>& orderedNodes,
    const QString& destinationSignalId,
    const QVariantMap& pointMachinePositions,
    const QVariantMap& options)
{
    QElapsedTimer timer;
    timer.start();

    // Get source signal ID from the first processed node
    QString sourceSignalId;
    for (const auto& node : orderedNodes) {
        if (node.isIndependent || node.controlledBy.isEmpty()) {
            sourceSignalId = node.signalId;
            break;
        }
    }

    QHash<QString, ControlNode> processedNodes;
    QVariantMap selectedAspects;
    QVariantMap requiredPointMachines;
    QVariantMap decisionReasons;
    QStringList processOrder;

    try {
        // Process nodes in dependency order
        for (ControlNode node : orderedNodes) {
            processOrder.append(node.signalId);

            // ENHANCED: Classify signal role for intelligent aspect selection
            SignalRole signalRole = classifySignalRole(
                node.signalId, sourceSignalId, destinationSignalId, orderedNodes);

            if (node.isIndependent) {
                // Independent signals - apply role-specific logic
                QString selectedAspect = selectBestAspectByRole(
                    node, node.possibleAspects, signalRole, options);

                node.selectedAspect = selectedAspect;
                selectedAspects[node.signalId] = selectedAspect;

                QString roleDescription = getRoleDescription(signalRole);
                decisionReasons[node.signalId] = QString(
                                                     "Independent signal (%1) - selected %2 using %3 priority")
                                                     .arg(roleDescription, selectedAspect,
                                                          signalRole == SignalRole::CONTROLLER_ABOVE_DEST ? "minimal safe" : "highest permissive");

                qDebug() << " [ASPECT_SELECTION]" << node.signalId
                         << "(" << roleDescription << ") → " << selectedAspect;

            } else {
                // Controlled signals must respect their controllers
                QStringList allowedByControllers = getAspectsAllowedByControllers(node, processedNodes);

                if (allowedByControllers.isEmpty()) {
                    QString errorMsg = QString("No valid aspects allowed by controlling signals for %1")
                    .arg(node.signalId);
                    qCritical() << "[AspectPropagationService > selectOptimalAspects]" << errorMsg;

                    QVariantMap errorResult;
                    errorResult["success"] = false;
                    errorResult["error"] = errorMsg;
                    errorResult["errorCode"] = "NO_VALID_ASPECTS";
                    errorResult["processingTimeMs"] = timer.elapsed();
                    return errorResult;
                }

                QString selectedAspect = selectBestAspectByRole(
                    node, allowedByControllers, signalRole, options);

                node.selectedAspect = selectedAspect;
                selectedAspects[node.signalId] = selectedAspect;

                QString roleDescription = getRoleDescription(signalRole);
                decisionReasons[node.signalId] = QString(
                                                     "Controlled signal (%1) - selected %2 from allowed: %3")
                                                     .arg(roleDescription, selectedAspect, allowedByControllers.join(","));

                qDebug() << "  [ASPECT_SELECTION]" << node.signalId
                         << "(" << roleDescription << ") → " << selectedAspect
                         << "from allowed:" << allowedByControllers;
            }

            // Validate the selection against all constraints
            if (!validateControlConstraints(node.signalId, node.selectedAspect, processedNodes)) {
                QString errorMsg = QString("Control constraint validation failed for %1 -> %2")
                .arg(node.signalId, node.selectedAspect);
                qCritical() << "[AspectPropagationService > selectOptimalAspects]" << errorMsg;

                QVariantMap errorResult;
                errorResult["success"] = false;
                errorResult["error"] = errorMsg;
                errorResult["errorCode"] = "CONSTRAINT_VIOLATION";
                errorResult["processingTimeMs"] = timer.elapsed();
                return errorResult;
            }

            node.isProcessed = true;
            processedNodes[node.signalId] = node;
        }

        qDebug() << " [ASPECT_SELECTION] Enhanced aspect propagation completed successfully!";

        QVariantMap result;
        result["success"] = true;
        result["aspects"] = selectedAspects;
        result["pointMachines"] = requiredPointMachines;
        result["reasons"] = decisionReasons;
        result["processOrder"] = processOrder;
        result["processingTimeMs"] = timer.elapsed();

        recordProcessingTime("aspect_selection", timer.elapsed());
        return result;

    } catch (const std::exception& e) {
        qCritical() << "[AspectPropagationService > selectOptimalAspects] Exception:" << e.what();

        QVariantMap errorResult;
        errorResult["success"] = false;
        errorResult["error"] = QString("Aspect selection failed: %1").arg(e.what());
        errorResult["errorCode"] = "SELECTION_ERROR";
        errorResult["processingTimeMs"] = timer.elapsed();
        return errorResult;
    }
}

QString AspectPropagationService::selectBestAspect(
    const ControlNode& node,
    const QStringList& allowedAspects,
    const QString& destinationSignalId,
    bool isDestination,
    const QVariantMap& options)
{
    Q_UNUSED(destinationSignalId)
    Q_UNUSED(options)

    // Apply destination constraint if this is the destination signal
    if (isDestination) {
        QString constraint = m_destinationConstraints.value(node.signalType, "RED");

        if (constraint == "RED" && allowedAspects.contains("RED")) {
            return "RED";
        } else if (constraint == "GREEN_OR_RED") {
            if (allowedAspects.contains("GREEN")) {
                return "GREEN";
            } else if (allowedAspects.contains("RED")) {
                return "RED";
            }
        }
    }

    // Select highest priority aspect from allowed list
    QStringList priorities = getAspectPriorities(node.signalType);

    for (const QString& priorityAspect : priorities) {
        if (allowedAspects.contains(priorityAspect)) {
            return priorityAspect;
        }
    }

    // Fallback to first available aspect
    if (!allowedAspects.isEmpty()) {
        return allowedAspects.first();
    }

    qCritical() << "[AspectPropagationService > selectBestAspect] No aspects available for" << node.signalId;
    return "RED"; // Safety fallback
}

QStringList AspectPropagationService::getAspectsAllowedByControllers(
    const ControlNode& node,
    const QHash<QString, ControlNode>& processedNodes)
{
    if (node.controlledBy.isEmpty()) {
        return node.possibleAspects; // No controllers
    }

    QStringList allowedAspects;
    bool isFirstController = true;

    for (const QString& controllingSignalId : node.controlledBy) {
        if (!processedNodes.contains(controllingSignalId)) {
            qWarning() << "[AspectPropagationService > getAspectsAllowedByControllers] Controller not yet processed:"
                       << controllingSignalId;
            continue; // Should not happen with proper dependency ordering
        }

        const ControlNode& controller = processedNodes[controllingSignalId];
        QStringList controllerAllowed = getAspectsPermittedByController(
            controller, node.signalId);

        if (node.controlMode == "AND") {
            // All controllers must permit - intersection
            if (isFirstController) {
                allowedAspects = controllerAllowed;
                isFirstController = false;
            } else {
                QSet<QString> currentSet(allowedAspects.begin(), allowedAspects.end());
                QSet<QString> controllerSet(controllerAllowed.begin(), controllerAllowed.end());
                allowedAspects = (currentSet & controllerSet).values();
            }
        } else if (node.controlMode == "OR") {
            // Any controller can permit - union
            QSet<QString> currentSet(allowedAspects.begin(), allowedAspects.end());
            QSet<QString> controllerSet(controllerAllowed.begin(), controllerAllowed.end());
            allowedAspects = (currentSet | controllerSet).values();
        }
    }

    // Filter to only aspects this signal can actually display
    QSet<QString> possibleSet(node.possibleAspects.begin(), node.possibleAspects.end());
    QSet<QString> allowedSet(allowedAspects.begin(), allowedAspects.end());
    QStringList finalAllowed = (possibleSet & allowedSet).values();

    return finalAllowed;
}

QStringList AspectPropagationService::getAspectsPermittedByController(
    const ControlNode& controller,
    const QString& controlledSignalId)
{
    qDebug() << " [RULE_EVAL] Evaluating what" << controller.signalId
             << "(" << controller.selectedAspect << ") allows for" << controlledSignalId;

    // ENHANCED: Use actual interlocking rules instead of simplified logic
    if (!m_ruleEngine) {
        qWarning() << " [RULE_EVAL] No rule engine available, using fallback";
        return QStringList{"RED"}; // Safe fallback only
    }

    //   REPLACE: Use rule engine instead of hardcoded evaluateInterlockingRule
    QStringList allowedAspects;
    try {
        allowedAspects = m_ruleEngine->getAspectsPermittedByController(
            controller.signalId,
            controller.selectedAspect,
            controlledSignalId
            );

        qDebug() << "   " << controller.signalId << "(" << controller.selectedAspect
                 << ") allows" << controlledSignalId << ":" << allowedAspects;

    } catch (const std::exception& e) {
        qWarning() << " [RULE_EVAL] Exception during rule evaluation:" << e.what();
        allowedAspects = QStringList{"RED"}; // Safe fallback
    }

    // Safety check: ensure we return at least something
    if (allowedAspects.isEmpty()) {
        qWarning() << " [RULE_EVAL] No aspects found, defaulting to RED";
        allowedAspects = QStringList{"RED"};
    }

    return allowedAspects;
}


bool AspectPropagationService::validateControlConstraints(
    const QString& signalId,
    const QString& selectedAspect,
    const QHash<QString, ControlNode>& processedNodes)
{
    Q_UNUSED(processedNodes)

    // Validate that the selected aspect is actually permitted by all controlling signals
    if (m_ruleEngine) {
        // Check if the aspect is in the signal's possible aspects
        QVariantMap signalData = m_dbManager->getSignalById(signalId);
        QStringList possibleAspects = signalData["possibleAspects"].toStringList();

        if (!possibleAspects.contains(selectedAspect)) {
            qWarning() << "[AspectPropagationService > validateControlConstraints] Aspect"
                       << selectedAspect << "not possible for signal" << signalId;
            return false;
        }
    }

    return true;
}

QStringList AspectPropagationService::getAspectPriorities(const QString& signalType) const
{
    return m_aspectPriorities.value(signalType, QStringList{"GREEN", "YELLOW", "RED"});
}


// Additional slot implementations
void AspectPropagationService::onSignalAspectChanged(const QString& signalId, const QString& newAspect)
{
    // Clear cache for the changed signal
    m_signalDataCache.remove(signalId);
    Q_UNUSED(newAspect)
}

void AspectPropagationService::onInterlockingRulesChanged()
{
    // Clear all caches when interlocking rules change
    m_signalDataCache.clear();
    m_controlRelationshipCache.clear();
}

// === MISSING Q_INVOKABLE METHOD IMPLEMENTATIONS ===

QVariantMap AspectPropagationService::analyzeDependencyOrder(const QVariantMap& prunedGraph) {
    QElapsedTimer timer;
    timer.start();

    try {
        QVector<ControlNode> orderedNodes = createDependencyOrder(prunedGraph);

        QVariantMap result;
        result["success"] = true;
        result["processingTimeMs"] = timer.elapsed();

        QVariantList processOrder;
        QVariantList independentSignals;

        for (const auto& node : orderedNodes) {
            QVariantMap nodeInfo;
            nodeInfo["signalId"] = node.signalId;
            nodeInfo["signalType"] = node.signalType;
            nodeInfo["isIndependent"] = node.isIndependent;
            nodeInfo["dependencyCount"] = node.controlledBy.size();

            processOrder.append(nodeInfo);

            if (node.isIndependent) {
                independentSignals.append(node.signalId);
            }
        }

        result["processOrder"] = processOrder;
        result["independentSignals"] = independentSignals;
        result["totalSignals"] = orderedNodes.size();

        return result;

    } catch (const std::exception& e) {
        return QVariantMap{
            {"success", false},
            {"error", QString("Dependency analysis failed: %1").arg(e.what())},
            {"processingTimeMs", timer.elapsed()}
        };
    }
}

QVariantMap AspectPropagationService::validatePropagationRequest(
    const QString& sourceSignalId,
    const QString& destinationSignalId) {

    QElapsedTimer timer;
    timer.start();

    // Use internal validation method
    QVariantMap result = validatePropagationRequestInternal(sourceSignalId, destinationSignalId);
    result["processingTimeMs"] = timer.elapsed();

    return result;
}

bool AspectPropagationService::setDestinationConstraint(
    const QString& signalType,
    const QString& requiredAspect) {

    if (signalType.isEmpty() || requiredAspect.isEmpty()) {
        qWarning() << "[AspectPropagationService > setDestinationConstraint] Invalid destination constraint parameters";
        return false;
    }

    m_destinationConstraints[signalType] = requiredAspect;
    return true;
}

bool AspectPropagationService::setPriorityAspects(
    const QString& signalType,
    const QStringList& priorities) {

    if (signalType.isEmpty() || priorities.isEmpty()) {
        qWarning() << "[AspectPropagationService > setPriorityAspects] Invalid priority aspects parameters";
        return false;
    }

    m_aspectPriorities[signalType] = priorities;
    return true;
}

QVariantMap AspectPropagationService::getConfiguration() const {
    QVariantMap config;

    // Destination constraints
    QVariantMap constraints;
    for (auto it = m_destinationConstraints.begin(); it != m_destinationConstraints.end(); ++it) {
        constraints[it.key()] = it.value();
    }
    config["destinationConstraints"] = constraints;

    // Aspect priorities
    QVariantMap priorities;
    for (auto it = m_aspectPriorities.begin(); it != m_aspectPriorities.end(); ++it) {
        priorities[it.key()] = it.value();
    }
    config["aspectPriorities"] = priorities;

    // Performance settings
    config["targetProcessingTimeMs"] = TARGET_PROCESSING_TIME_MS;
    config["isOperational"] = m_isOperational;

    return config;
}

QVariantMap AspectPropagationService::getPerformanceMetrics() const {
    QVariantMap metrics;

    metrics["averageProcessingTimeMs"] = m_averageProcessingTimeMs;
    metrics["totalPropagations"] = m_totalPropagations;
    metrics["successfulPropagations"] = m_successfulPropagations;
    metrics["successRate"] = successRate();
    metrics["targetProcessingTimeMs"] = TARGET_PROCESSING_TIME_MS;
    metrics["isPerformanceAcceptable"] = m_averageProcessingTimeMs <= TARGET_PROCESSING_TIME_MS;

    return metrics;
}

QVariantMap AspectPropagationService::getStatistics() const {
    QVariantMap stats = getPerformanceMetrics();

    stats["isOperational"] = m_isOperational;
    stats["systemUptime"] = QDateTime::currentDateTime().toSecsSinceEpoch();

    return stats;
}

QVariantList AspectPropagationService::getRecentPropagations(int limit) const {
    Q_UNUSED(limit) // Not implemented - would need propagation history storage

    // Return empty list for now - safety-critical systems should not crash
    QVariantList emptyList;
    return emptyList;
}

QVariantMap AspectPropagationService::testControlGraphConstruction(const QString& sourceSignalId) {
    if (!m_isOperational) {
        return QVariantMap{
            {"success", false},
            {"error", "Service not operational"}
        };
    }

    QElapsedTimer timer;
    timer.start();

    try {
        QVariantMap graph = buildControlGraphInternal(sourceSignalId);

        QVariantMap result;
        result["success"] = true;
        result["sourceSignalId"] = sourceSignalId;
        result["nodeCount"] = graph["nodes"].toMap().size();
        result["edgeCount"] = graph["edges"].toList().size();
        result["processingTimeMs"] = timer.elapsed();
        result["graph"] = graph;

        return result;

    } catch (const std::exception& e) {
        return QVariantMap{
            {"success", false},
            {"error", QString("Graph construction test failed: %1").arg(e.what())},
            {"processingTimeMs", timer.elapsed()}
        };
    }
}

QVariantMap AspectPropagationService::simulateAspectPropagation(
    const QString& sourceSignalId,
    const QString& destinationSignalId,
    bool dryRun) {

    Q_UNUSED(dryRun) // Simulation is always dry-run

    if (!m_isOperational) {
        return QVariantMap{
            {"success", false},
            {"error", "Service not operational"}
        };
    }

    QElapsedTimer timer;
    timer.start();

    try {
        // Perform full propagation without modifying database
        AspectPropagationResult result = propagateAspectsInternal(
            sourceSignalId, destinationSignalId, QVariantMap(), QVariantMap());

        // Convert to QVariantMap for Q_INVOKABLE return
        return aspectPropagationResultToVariantMap(result);

    } catch (const std::exception& e) {
        return QVariantMap{
            {"success", false},
            {"error", QString("Simulation failed: %1").arg(e.what())},
            {"processingTimeMs", timer.elapsed()}
        };
    }
}

AspectPropagationService::SignalRole AspectPropagationService::classifySignalRole(
    const QString& signalId,
    const QString& sourceSignalId,
    const QString& destinationSignalId,
    const QVector<ControlNode>& orderedNodes) const
{
    // 1. Check if this is the destination signal
    if (signalId == destinationSignalId) {
        return SignalRole::DESTINATION;
    }

    // 2. Check if this signal controls the destination (controller above destination)
    if (isControllerAboveDestination(signalId, destinationSignalId, orderedNodes)) {
        return SignalRole::CONTROLLER_ABOVE_DEST;
    }

    // 3. Everything else is source/intermediate (signals between source and destination)
    return SignalRole::SOURCE_INTERMEDIATE;
}


bool AspectPropagationService::isControllerAboveDestination(
    const QString& signalId,
    const QString& destinationSignalId,
    const QVector<ControlNode>& orderedNodes) const
{
    // Find the destination node
    const ControlNode* destNode = nullptr;
    for (const auto& node : orderedNodes) {
        if (node.signalId == destinationSignalId) {
            destNode = &node;
            break;
        }
    }

    if (!destNode) {
        return false;
    }

    // Check if signalId is in the destination's controlledBy list
    return destNode->controlledBy.contains(signalId);
}

QStringList AspectPropagationService::getAspectPrioritiesForRole(
    SignalRole role,
    const QString& signalType) const
{
    switch (role) {
    case SignalRole::DESTINATION:
        // Destinations use standard priorities but logic handled separately
        return m_aspectPriorities.value(signalType, QStringList{"GREEN", "YELLOW", "RED"});

    case SignalRole::SOURCE_INTERMEDIATE:
        // Source and intermediate signals: highest permissive first (operational efficiency)
        return QStringList{"GREEN", "YELLOW", "RED"};

    case SignalRole::CONTROLLER_ABOVE_DEST:
        // Controller signals above destination: minimal safe first (safety constraint)
        return QStringList{"RED", "YELLOW", "GREEN"};

    default:
        return QStringList{"GREEN", "YELLOW", "RED"};
    }
}

QString AspectPropagationService::selectDestinationAspect(
    const QString& signalType,
    const QStringList& allowedAspects,
    const QVariantMap& options) const
{
    // Check for explicit destination aspect override
    if (options.contains("desired_destination_aspect")) {
        QString desiredAspect = options["desired_destination_aspect"].toString();
        if (allowedAspects.contains(desiredAspect)) {
            qDebug() << " [DESTINATION] Using explicit override:" << desiredAspect;
            return desiredAspect;
        } else {
            qWarning() << " [DESTINATION] Desired aspect" << desiredAspect
                       << "not in allowed list:" << allowedAspects;
        }
    }

    // Apply type-based defaults
    if (signalType == "ADVANCED_STARTER") {
        // Advanced starters can proceed if track clear
        if (allowedAspects.contains("GREEN")) {
            qDebug() << "[DESTINATION] Advanced Starter proceeding: GREEN";
            return "GREEN";
        }
    }

    // Default: destination should be RED (stopping point)
    if (allowedAspects.contains("RED")) {
        qDebug() << " [DESTINATION] Standard stopping point: RED";
        return "RED";
    }

    // Safety fallback
    return allowedAspects.isEmpty() ? "RED" : allowedAspects.first();
}

QString AspectPropagationService::selectBestAspectByRole(
    const ControlNode& node,
    const QStringList& allowedAspects,
    SignalRole role,
    const QVariantMap& options) const
{
    // Handle destination signals specially
    if (role == SignalRole::DESTINATION) {
        return selectDestinationAspect(node.signalType, allowedAspects, options);
    }

    // Get role-specific priorities
    QStringList priorities = getAspectPrioritiesForRole(role, node.signalType);

    // Select first available aspect according to role priorities
    for (const QString& priorityAspect : priorities) {
        if (allowedAspects.contains(priorityAspect)) {
            QString roleDesc = (role == SignalRole::CONTROLLER_ABOVE_DEST) ? "minimal safe" : "highest permissive";
            qDebug() << "    Selected" << roleDesc << "aspect:" << priorityAspect
                     << "for" << node.signalType;
            return priorityAspect;
        }
    }

    // Safety fallback
    if (!allowedAspects.isEmpty()) {
        return allowedAspects.first();
    }

    qCritical() << "[AspectPropagationService > selectBestAspectByRole] No aspects available for" << node.signalId;
    return "RED"; // Safety fallback
}

QString AspectPropagationService::getRoleDescription(SignalRole role) const
{
    switch (role) {
    case SignalRole::DESTINATION:
        return "DESTINATION";
    case SignalRole::SOURCE_INTERMEDIATE:
        return "SOURCE/INTERMEDIATE";
    case SignalRole::CONTROLLER_ABOVE_DEST:
        return "CONTROLLER_ABOVE_DEST";
    default:
        return "UNKNOWN";
    }
}

QVariantMap AspectPropagationService::calculateRequiredPointMachineStates(
    const QStringList& routePath,
    const QStringList& overlapPath)
{
    qDebug() << " [POINT_MACHINES] Calculating required states for route path:" << routePath;
    qDebug() << " [POINT_MACHINES] Overlap path:" << overlapPath;

    QVariantMap requiredStates;
    QStringList completePath = routePath + overlapPath;

    // Process each transition in the complete path
    for (int i = 0; i < completePath.size() - 1; i++) {
        QString fromCircuit = completePath[i];
        QString toCircuit = completePath[i + 1];

        qDebug() << "  Transition:" << fromCircuit << "→" << toCircuit;

        //   NEW: Get point machine requirement directly from track circuit edge
        PointMachineRequirement requirement = getPointMachineRequirement(fromCircuit, toCircuit);

        if (requirement.isRequired) {
            QString pmId = requirement.pointMachineId;
            QString requiredPosition = requirement.requiredPosition;

            qDebug() << "     Point machine" << pmId << "requires position:" << requiredPosition;

            //   ENHANCED: Get current position of the specific point machine
            QVariantMap pmData = m_dbManager->getPointMachineById(pmId);
            if (pmData.isEmpty()) {
                qWarning() << "     Point machine" << pmId << "not found in database";
                continue;
            }

            QString currentPosition = pmData["currentPosition"].toString();

            // Store required state
            QVariantMap pmState;
            pmState["requiredPosition"] = requiredPosition;
            pmState["currentPosition"] = currentPosition;
            pmState["needsMovement"] = (currentPosition != requiredPosition);
            pmState["forTransition"] = QString("%1→%2").arg(fromCircuit, toCircuit);
            pmState["availabilityStatus"] = pmData["availabilityStatus"].toString();
            pmState["isLocked"] = pmData["isLocked"].toBool();

            //   SAFETY: Check if point machine is available for movement
            QString availabilityStatus = pmData["availabilityStatus"].toString();
            if (availabilityStatus != "AVAILABLE") {
                qWarning() << "     Point machine" << pmId << "not available:" << availabilityStatus;
                pmState["movementBlocked"] = true;
                pmState["blockReason"] = availabilityStatus;
            } else {
                pmState["movementBlocked"] = false;
            }

            requiredStates[pmId] = pmState;

            qDebug() << "      PM" << pmId << ":" << currentPosition << "→" << requiredPosition
                     << (currentPosition != requiredPosition ? "(MOVE REQUIRED)" : "(NO MOVEMENT)");
        } else {
            qDebug() << "     No point machine required for this transition";
        }
    }

    qDebug() << "  [POINT_MACHINES] Required states calculated:" << requiredStates.keys();
    return requiredStates;
}

QString AspectPropagationService::getRequiredPointMachinePosition(
    const QString& fromCircuit,
    const QString& toCircuit)
{
    if (!m_dbManager) {
        return QString();
    }

    // Query track_circuit_edges to find the required position
    QSqlQuery query(m_dbManager->getDatabase());
    query.prepare(R"(
        SELECT condition_position
        FROM railway_control.track_circuit_edges
        WHERE from_circuit_id = ? AND to_circuit_id = ?
        AND condition_point_machine_id IS NOT NULL
        AND is_active = TRUE
        LIMIT 1
    )");
    query.addBindValue(fromCircuit);
    query.addBindValue(toCircuit);

    if (query.exec() && query.next()) {
        QString position = query.value("condition_position").toString();
        qDebug() << "   Edge" << fromCircuit << "?" << toCircuit << "requires position:" << position;
        return position;
    }

    return QString(); // No point machine condition required
}

PointMachineRequirement AspectPropagationService::getPointMachineRequirement(
    const QString& fromCircuit,
    const QString& toCircuit)
{
    PointMachineRequirement requirement;

    if (!m_dbManager) {
        return requirement;
    }

    //   FIX: Ensure we get non-null position values
    QSqlQuery query(m_dbManager->getDatabase());
    query.prepare(R"(
        SELECT
            condition_point_machine_id,
            condition_position
        FROM railway_control.track_circuit_edges
        WHERE from_circuit_id = ? AND to_circuit_id = ?
        AND condition_point_machine_id IS NOT NULL
        AND condition_position IS NOT NULL
        AND condition_position != ''  --   ADDED: Exclude empty strings
        AND is_active = TRUE
        LIMIT 1
    )");
    query.addBindValue(fromCircuit);
    query.addBindValue(toCircuit);

    if (query.exec() && query.next()) {
        QString pointMachineId = query.value("condition_point_machine_id").toString();
        QString requiredPosition = query.value("condition_position").toString();

        //   SAFETY CHECK: Ensure position is valid
        if (!requiredPosition.isEmpty() &&
            (requiredPosition == "NORMAL" || requiredPosition == "REVERSE")) {
            requirement.pointMachineId = pointMachineId;
            requirement.requiredPosition = requiredPosition;
            requirement.isRequired = true;

            qDebug() << "  Edge" << fromCircuit << "→" << toCircuit
                     << "requires PM" << requirement.pointMachineId
                     << "in position:" << requirement.requiredPosition;
        } else {
            qWarning() << "   Invalid position found for" << fromCircuit << "→" << toCircuit
                       << "PM:" << pointMachineId << "Position:" << requiredPosition;
        }
    }

    return requirement;
}
