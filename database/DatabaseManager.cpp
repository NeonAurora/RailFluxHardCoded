#include "DatabaseManager.h"
#include <QStandardPaths>
#include <QDir>
#include <QCoreApplication>
#include <QThread>
#include <QProcess>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QSqlRecord>
#include "../interlocking/InterlockingService.h"

DatabaseManager::DatabaseManager(QObject* parent)
    : QObject(parent)
    , pollingTimer(std::make_unique<QTimer>(this))
    , connected(false)
{
    connect(pollingTimer.get(), &QTimer::timeout, this, &DatabaseManager::pollDatabase);
    pollingTimer->setInterval(POLLING_INTERVAL_MS);

    // ADD: Health monitoring for notifications
    m_notificationHealthTimer = new QTimer(this);
    connect(m_notificationHealthTimer, &QTimer::timeout, this, &DatabaseManager::checkNotificationHealth);
    m_notificationHealthTimer->start(100000); // Check every minute
}


DatabaseManager::~DatabaseManager() {
    stopPolling();
    if (db.isOpen()) {
        db.close();
    }
    cleanup();
    qDebug() << "DatabaseManager destroyed";
}

bool DatabaseManager::connectToDatabase()
{
    // Try system PostgreSQL first
    if (connectToSystemPostgreSQL()) {
        qDebug() << "Connected to system PostgreSQL";
        enableRealTimeUpdates();  // Enable LISTEN/NOTIFY
        return true;
    }

    qDebug() << "System PostgreSQL unavailable, starting portable mode...";

    // Fall back to portable PostgreSQL
    if (startPortableMode()) {
        qDebug() << "Connected to portable PostgreSQL";
        enableRealTimeUpdates();  // Enable LISTEN/NOTIFY
        return true;
    }

    // Set disconnected state and emit signal
    connected = false;
    m_isConnected = false;
    emit connectionStateChanged(connected);
    emit errorOccurred("Failed to connect to any PostgreSQL instance");
    return false;
}

// In DatabaseManager.cpp - Fix connectToSystemPostgreSQL()
bool DatabaseManager::connectToSystemPostgreSQL()
{
    try {
        // CRITICAL: Check if connection already exists and is open
        if (QSqlDatabase::contains("system_connection")) {
            QSqlDatabase existingDb = QSqlDatabase::database("system_connection");
            if (existingDb.isOpen() && existingDb.isValid()) {
                qDebug() << "Using existing system PostgreSQL connection";
                db = existingDb;
                connected = true;
                m_isConnected = true;
                return true;
            }
            // CRITICAL: Only remove if connection is actually closed
            qDebug() << "Removing stale system connection";
            m_notificationsEnabled = false;
            m_notificationsWorking = false;
            QSqlDatabase::removeDatabase("system_connection");
        }

        db = QSqlDatabase::addDatabase("QPSQL", "system_connection");
        db.setHostName("localhost");
        db.setPort(m_systemPort);
        db.setDatabaseName("railway_control_system");
        db.setUserName("postgres");
        db.setPassword("qwerty");

        if (db.open()) {
            connected = true;
            m_isConnected = true;
            emit connectionStateChanged(connected);
            qDebug() << "Connected to system PostgreSQL";
            return true;
        }
    } catch (...) {
        qDebug() << "System PostgreSQL connection failed";
    }

    connected = false;
    m_isConnected = false;
    emit connectionStateChanged(connected);
    return false;
}

bool DatabaseManager::startPortableMode()
{
    m_appDirectory = getApplicationDirectory();
    m_postgresPath = m_appDirectory + "/database/postgresql";
    m_dataPath = m_appDirectory + "/database/data";

    // Initialize database if needed
    if (!QDir(m_dataPath).exists()) {
        if (!initializePortableDatabase()) {
            return false;
        }
    }

    // Check if server is already running before starting
    if (!isPortableServerRunning()) {
        if (!startPortablePostgreSQL()) {
            return false;
        }
        // Wait for server to start
        // QThread::sleep(1);
    } else {
        qDebug() << "Portable PostgreSQL server already running";
    }

    // Remove existing connection if it exists
    if (QSqlDatabase::contains("portable_connection")) {
        m_notificationsEnabled = false;
        m_notificationsWorking = false;
        QSqlDatabase::removeDatabase("portable_connection");
    }

    try {
        db = QSqlDatabase::addDatabase("QPSQL", "portable_connection");
        db.setHostName("localhost");
        db.setPort(m_portablePort);
        db.setDatabaseName("railway_control_system");
        db.setUserName("postgres");
        db.setPassword("qwerty");

        if (db.open()) {
            connected = true;
            m_isConnected = true;
            m_connectionStatus = "Connected to Portable PostgreSQL";
            emit connectionStateChanged(connected);
            qDebug() << "Portable PostgreSQL connected with schema created";
            return true;
        }
    } catch (const std::exception& e) {
        qDebug() << "Portable PostgreSQL connection failed:" << e.what();
    }

    connected = false;
    m_isConnected = false;
    emit connectionStateChanged(connected);
    return false;
}

bool DatabaseManager::initializePortableDatabase()
{
    QString initdbPath = m_postgresPath + "/bin/initdb.exe";

    if (!QFile::exists(initdbPath)) {
        qDebug() << "PostgreSQL binaries not found at:" << m_postgresPath;
        return false;
    }

    QProcess initProcess;
    QStringList arguments;
    arguments << "-D" << m_dataPath
              << "-U" << "postgres"      // CHANGED: Use postgres user
              << "-A" << "trust"         // Start with trust, convert later
              << "-E" << "UTF8";

    qDebug() << " Initializing portable database with postgres user...";
    initProcess.start(initdbPath, arguments);

    if (!initProcess.waitForFinished(100)) {
        qDebug() << "Database initialization timed out";
        return false;
    }

    if (initProcess.exitCode() != 0) {
        qDebug() << "Database initialization failed:" << initProcess.readAllStandardError();
        return false;
    }

    qDebug() << "Portable database initialized with postgres user";
    return true;
}

bool DatabaseManager::startPortablePostgreSQL()
{
    QString pgCtlPath = m_postgresPath + "/bin/pg_ctl.exe";
    QString logPath = m_appDirectory + "/database/logs/postgresql.log";

    // Ensure logs directory exists
    QDir().mkpath(QFileInfo(logPath).path());

    if (m_postgresProcess) {
        delete m_postgresProcess;
    }

    m_postgresProcess = new QProcess(this);

    QStringList arguments;
    arguments << "-D" << m_dataPath
              << "-l" << logPath
              << "start";  // REMOVED: -o port argument (port is in postgresql.conf)

    qDebug() << "Starting portable PostgreSQL server...";
    qDebug() << "Command:" << pgCtlPath << arguments.join(" ");

    m_postgresProcess->start(pgCtlPath, arguments);

    if (!m_postgresProcess->waitForFinished(100)) {  // Increased timeout
        qDebug() << "Failed to start PostgreSQL server (timeout)";
        return false;
    }

    if (m_postgresProcess->exitCode() != 0) {
        QString errorOutput = m_postgresProcess->readAllStandardError();
        QString standardOutput = m_postgresProcess->readAllStandardOutput();
        qDebug() << "PostgreSQL server start failed with exit code:" << m_postgresProcess->exitCode();
        qDebug() << "Error output:" << errorOutput;
        qDebug() << "Standard output:" << standardOutput;
        return false;
    }

    qDebug() << "Portable PostgreSQL server started on port" << m_portablePort;
    return true;
}

QString DatabaseManager::getApplicationDirectory()
{
    // Go up one level from app/ to get to the root project directory
    QDir appDir(QCoreApplication::applicationDirPath());
    appDir.cdUp();  // Go from "app/" to root directory
    return appDir.absolutePath();
}

void DatabaseManager::cleanup()
{
    if (m_postgresProcess) {
        stopPortablePostgreSQL();
        delete m_postgresProcess;
        m_postgresProcess = nullptr;
    }
}

bool DatabaseManager::stopPortablePostgreSQL()
{
    if (!m_postgresProcess) return true;

    QString pgCtlPath = m_postgresPath + "/bin/pg_ctl.exe";

    QProcess stopProcess;
    QStringList arguments;
    arguments << "-D" << m_dataPath << "stop";

    qDebug() << "Stopping portable PostgreSQL server...";
    stopProcess.start(pgCtlPath, arguments);

    if (stopProcess.waitForFinished(5000)) {
        qDebug() << "PostgreSQL server stopped successfully";
        return true;
    }

    qDebug() << "PostgreSQL server stop timed out";
    return false;
}

void DatabaseManager::enableRealTimeUpdates() {
    if (m_notificationsEnabled) {
        qDebug() << "Real-time updates already enabled";
        return;
    }

    if (!connected || !db.isOpen()) {
        qWarning() << "Cannot enable real-time updates - database not connected";
        return;
    }

    // Check if driver supports notifications
    if (!db.driver()->hasFeature(QSqlDriver::EventNotifications)) {
        qWarning() << "Database driver does not support event notifications";
        return;
    }

    // Use subscribeToNotification
    if (db.driver()->subscribeToNotification("railway_changes")) {
        qDebug() << "Subscribed to railway_changes notifications";

        // ENHANCED: Connect with health tracking
        QObject::connect(db.driver(), &QSqlDriver::notification,
                         this, [this](const QString& name, QSqlDriver::NotificationSource source, const QVariant& payload) {
                             // TRACK SEGMENT: Update health indicators
                             m_lastNotificationReceived = QDateTime::currentDateTime();
                             m_notificationsWorking = true;

                             qDebug() << " NOTIFICATION RECEIVED:" << name << "Payload:" << payload.toString();
                             this->handleDatabaseNotification(name, payload);

                             // HYBRID: Reduce polling frequency
                             if (pollingTimer->interval() != POLLING_INTERVAL_SLOW) {
                                 pollingTimer->setInterval(POLLING_INTERVAL_SLOW);
                                 qDebug() << "Reduced polling to" << POLLING_INTERVAL_SLOW << "ms - notifications working";
                             }
                         });

        m_notificationsEnabled = true;
        m_lastNotificationReceived = QDateTime::currentDateTime(); // Initialize

        // Send test notification
        QSqlQuery testQuery(db);
        if (testQuery.exec("SELECT pg_notify('railway_changes', "
                           "'{\"test\": \"startup\", \"timestamp\": \"" +
                           QString::number(QDateTime::currentSecsSinceEpoch()) + "\"}'::text)")) {
            qDebug() << "Test notification sent";
        }
    } else {
        qWarning() << "Failed to subscribe to railway_changes notifications";
    }
}

void DatabaseManager::checkNotificationHealth() {
    if (!m_notificationsEnabled) return;

    QDateTime now = QDateTime::currentDateTime();

    if (m_lastNotificationReceived.isValid() &&
        m_lastNotificationReceived.secsTo(now) > 300) {

        qWarning() << "No notifications for 1 seconds - assuming failure";
        m_notificationsWorking = false;

        // UPDATE: Emit signal when changing interval
        pollingTimer->setInterval(POLLING_INTERVAL_FAST);
        emit pollingIntervalChanged(POLLING_INTERVAL_FAST); // ADD

        qDebug() << "Increased polling to" << POLLING_INTERVAL_FAST << "ms (notification failover)";
    }
}

void DatabaseManager::handleDatabaseNotification(const QString& name, const QVariant& payload) {
    qDebug() << " NOTIFICATION HANDLER CALLED:" << name << payload.toString();

    if (name != "railway_changes") {
        qDebug() << "Unexpected notification channel:" << name;
        return;
    }

    QString payloadStr = payload.toString();
    if (payloadStr.isEmpty()) {
        qWarning() << "Empty notification payload";
        return;
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(payloadStr.toUtf8(), &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        qWarning() << "JSON parse error:" << parseError.errorString() << "Payload:" << payloadStr;
        return;
    }

    QJsonObject obj = doc.object();
    QString table = obj["table"].toString();
    QString operation = obj["operation"].toString();
    QString entityId = obj["entity_id"].toString();

    qDebug() << "Parsed notification:" << table << operation << entityId;

    // ADD: Update polling interval when notifications are working
    if (pollingTimer && pollingTimer->isActive() && pollingTimer->interval() != POLLING_INTERVAL_SLOW) {
        pollingTimer->setInterval(POLLING_INTERVAL_SLOW);
        emit pollingIntervalChanged(POLLING_INTERVAL_SLOW);
        qDebug() << "Reduced polling to" << POLLING_INTERVAL_SLOW << "ms - notifications working";
    }

    // UPDATE: Mark notifications as working (for health monitoring)
    m_notificationsWorking = true;
    m_lastNotificationReceived = QDateTime::currentDateTime();

    // SAFETY: No cache refreshing - just emit signals for UI updates
    if (obj["test"].toString() == "startup") {
        qDebug() << "Test notification received - system working";
        return; // ← Don't trigger data refresh
    }

    if (table == "signals") {
        emit signalsChanged();
        emit signalUpdated(entityId);
        qDebug() << "Emitted signalsChanged and signalUpdated(" << entityId << ")";
    } else if (table == "point_machines") {
        emit pointMachinesChanged();
        emit pointMachineUpdated(entityId);
        qDebug() << "Emitted pointMachinesChanged and pointMachineUpdated(" << entityId << ")";
    } else if (table == "track_segments") {
        emit trackSegmentsChanged();  //  Consistent naming
        emit trackSegmentUpdated(entityId);  //  Consistent naming
        qDebug() << "Emitted trackSegmentsChanged and trackSegmentUpdated(" << entityId << ")";
    } else if (table == "track_circuits") {  // NEW: Handle circuit notifications
        emit trackCircuitsChanged();
        emit trackSegmentsChanged(); // Segments depend on circuits
        qDebug() << "Emitted trackCircuitsChanged and trackSegmentsChanged (circuit affects segments)";
    }

    emit dataUpdated();
    qDebug() << "Emitted dataUpdated()";
}

int DatabaseManager::getCurrentPollingInterval() const {
    // ADD: Debug logging
    qDebug() << "getCurrentPollingInterval() called:";
    qDebug() << "   pollingTimer exists:" << (pollingTimer != nullptr);
    if (pollingTimer) {
        qDebug() << "   pollingTimer->isActive():" << pollingTimer->isActive();
        qDebug() << "   pollingTimer->interval():" << pollingTimer->interval();
    }

    if (!pollingTimer || !pollingTimer->isActive()) {
        qDebug() << "   → Returning 0 (Not polling)";
        return 0; // Not polling
    }

    int interval = pollingTimer->interval();
    qDebug() << "   → Returning interval:" << interval;
    return interval;
}

QString DatabaseManager::getPollingIntervalDisplay() const {
    int interval = getCurrentPollingInterval();

    // ADD: Debug logging
    qDebug() << "getPollingIntervalDisplay() called:";
    qDebug() << "   interval from getCurrentPollingInterval():" << interval;

    if (interval == 0) {
        qDebug() << "   → Returning 'Not polling'";
        return "Not polling";
    } else if (interval < 1000) {
        QString result = QString("%1ms").arg(interval);
        qDebug() << "   → Returning:" << result;
        return result;
    } else if (interval < 60000) {
        QString result = QString("%1s").arg(interval / 1000);
        qDebug() << "   → Returning:" << result;
        return result;
    } else {
        int minutes = interval / 60000;
        int seconds = (interval % 60000) / 1000;
        QString result;
        if (seconds == 0) {
            result = QString("%1m").arg(minutes);
        } else {
            result = QString("%1m %2s").arg(minutes).arg(seconds);
        }
        qDebug() << "   → Returning:" << result;
        return result;
    }
}

void DatabaseManager::startPolling() {
    if (connected) {
        // INTELLIGENT: Longer interval when notifications are working
        int interval = m_notificationsWorking ? POLLING_INTERVAL_SLOW : POLLING_INTERVAL_FAST;
        pollingTimer->setInterval(interval);
        pollingTimer->start();

        emit pollingIntervalChanged(interval);

        qDebug() << "HYBRID: Database polling started"
                 << "(interval:" << interval << "ms)"
                 << "Notifications working:" << m_notificationsWorking;
    }
}

void DatabaseManager::stopPolling() {
    pollingTimer->stop();
    qDebug() << "Database polling stopped";
}

bool DatabaseManager::isConnected() const {
    return connected;
}

void DatabaseManager::pollDatabase() {
    if (!connected) return;

    qDebug() << "SAFETY POLLING: Direct database state check";
    detectAndEmitChanges();
    emit dataUpdated(); // Trigger QML property updates
}

void DatabaseManager::detectAndEmitChanges() {
    // Poll signals
    QSqlQuery signalQuery("SELECT signal_id, current_aspect_id FROM railway_control.signals", db);
    while (signalQuery.next()) {
        QString signalId = signalQuery.value(0).toString();
        int aspectId = signalQuery.value(1).toInt();

        if (!lastSignalStates.contains(signalId.toInt()) || lastSignalStates[signalId.toInt()] != QString::number(aspectId)) {
            lastSignalStates[signalId.toInt()] = QString::number(aspectId);
            emit signalStateChanged(signalId.toInt(), QString::number(aspectId));
        }
    }

    //  Poll trackSegment circuits for occupancy (not segments)
    QSqlQuery circuitQuery("SELECT circuit_id, is_occupied FROM railway_control.track_circuits", db);
    while (circuitQuery.next()) {
        QString circuitId = circuitQuery.value(0).toString();
        bool isOccupied = circuitQuery.value(1).toBool();

        // Use circuit_id as key for tracking state changes
        int circuitKey = qHash(circuitId);
        if (!lastTrackSegmentStates.contains(circuitKey) || lastTrackSegmentStates[circuitKey] != isOccupied) {
            lastTrackSegmentStates[circuitKey] = isOccupied;
            emit trackCircuitStateChanged(circuitKey, isOccupied);
        }
    }
}

bool DatabaseManager::isPortableServerRunning()
{
    QString pgCtlPath = m_postgresPath + "/bin/pg_ctl.exe";

    QProcess checkProcess;
    QStringList arguments;
    arguments << "-D" << m_dataPath << "status";

    checkProcess.start(pgCtlPath, arguments);
    checkProcess.waitForFinished(100);

    // If exit code is 0, server is running
    bool isRunning = (checkProcess.exitCode() == 0);
    qDebug() << "Portable PostgreSQL server running check:" << isRunning;

    return isRunning;
}

// SAFETY: Direct database queries - NO CACHING
QVariantList DatabaseManager::getTrackSegmentsList() {
    if (!connected) return QVariantList();

    qDebug() << " SAFETY: getTrackSegmentsList() - DIRECT DATABASE QUERY with locking status";

    QVariantList trackSegments;
    QSqlQuery trackSegmentQuery(db);

    //   UPDATED: Query with new locking schema fields
    QString trackSegmentSql = R"(
        SELECT
            ts.id,
            ts.segment_id,
            ts.segment_name,
            ts.start_row,
            ts.start_col,
            ts.end_row,
            ts.end_col,
            ts.track_segment_type,
            ts.is_assigned,
            ts.is_overlap,  --   NEW: Track segment overlap status
            ts.is_active,
            ts.circuit_id,

            -- Metadata fields
            ts.length_meters,
            ts.max_speed_kmh,
            ts.protecting_signals,
            ts.created_at,
            ts.updated_at,

            -- Circuit status information
            COALESCE(tc.is_occupied, false) as is_occupied,
            COALESCE(tc.is_assigned, false) as circuit_is_assigned,  --   NEW
            COALESCE(tc.is_overlap, false) as circuit_is_overlap,    --   NEW
            tc.occupied_by,

            --   UPDATED: Enhanced route assignment eligibility logic
            CASE
                WHEN tc.is_occupied = true THEN false
                WHEN tc.is_assigned = true OR tc.is_overlap = true THEN false  --   NEW: Consider circuit locking
                WHEN ts.is_assigned = true OR ts.is_overlap = true THEN false  --   NEW: Consider segment locking
                WHEN rl.is_active = true THEN false
                ELSE true
            END as route_assignment_eligible

        FROM railway_control.track_segments ts
        LEFT JOIN railway_control.track_circuits tc ON ts.circuit_id = tc.circuit_id
        LEFT JOIN railway_control.resource_locks rl ON (
            rl.resource_type = 'TRACK_CIRCUIT'
            AND rl.resource_id = tc.circuit_id
            AND rl.is_active = true
        )
        WHERE ts.is_active = TRUE
        ORDER BY ts.segment_id
    )";

    if (trackSegmentQuery.exec(trackSegmentSql)) {
        while (trackSegmentQuery.next()) {
            trackSegments.append(convertTrackSegmentRowToVariant(trackSegmentQuery));
        }
        qDebug() << "  Loaded" << trackSegments.size() << "track segments with occupancy and locking data";
    } else {
        qWarning() << " SAFETY CRITICAL: Track Segment query failed:" << trackSegmentQuery.lastError().text();
    }

    return trackSegments;
}

QVariantList DatabaseManager::getAllSignalsList() {
    if (!connected) return QVariantList();

    qDebug() << "SAFETY: getAllSignalsList() - Loading signals with locking status from v_signals_complete";

    QVariantList signalsList;
    QSqlQuery signalQuery(db);

    // ? UPDATED: Include is_locked column for resource locking
    QString signalSql = R"(
        SELECT
            id,
            signal_id,
            signal_name,
            signal_type,
            signal_type_name,
            location_row as row,
            location_col as col,
            direction,
            is_locked,  -- ? NEW: Resource locking status
            current_aspect,
            current_aspect_name,
            current_aspect_color,
            calling_on_aspect,
            calling_on_aspect_name,
            calling_on_aspect_color,
            loop_aspect,
            loop_aspect_name,
            loop_aspect_color,
            loop_signal_configuration,
            aspect_count,
            possible_aspects,
            is_active,
            location_description as location,
            last_changed_at,
            last_changed_by,
            interlocked_with,
            protected_track_circuits,
            manual_control_active,
            preceded_by_circuit_id,
            succeeded_by_circuit_id,
            is_route_signal,
            route_signal_type,
            created_at,
            updated_at
        FROM railway_control.v_signals_complete
        ORDER BY signal_id
    )";

    if (signalQuery.exec(signalSql)) {
        while (signalQuery.next()) {
            signalsList.append(convertSignalRowToVariant(signalQuery));
        }
        qDebug() << "? Loaded" << signalsList.size() << "signals with complete aspect and locking information from view";
    } else {
        qWarning() << "? SAFETY CRITICAL: Enhanced signal view query failed:" << signalQuery.lastError().text();
    }

    return signalsList;
}

QVariantList DatabaseManager::getAllPointMachinesList() {
    if (!connected) return QVariantList();

    qDebug() << "SAFETY: getAllPointMachinesList() - DIRECT DATABASE QUERY from v_point_machines_complete";

    QVariantList points;
    QSqlQuery pointQuery(db);

    // ? UPDATED: Query the complete view with all route assignment fields
    QString pointSql = R"(
        SELECT
            id,
            machine_id,
            machine_name,
            junction_row,
            junction_col,
            root_track_segment_connection,
            normal_track_segment_connection,
            reverse_track_segment_connection,

            -- Position information
            current_position,
            current_position_name,
            position_description,
            position_pathfinding_weight,
            position_default_transition_time_ms,

            -- Operational status and timing
            operating_status,
            transition_time_ms,
            last_operated_at,
            last_operated_by,
            operation_count,

            -- Locking and safety
            is_locked,
            lock_reason,
            safety_interlocks,
            protected_signals,

            -- Route assignment extensions
            paired_entity,
            host_track_circuit,
            route_locking_enabled,
            auto_normalize_after_route,

            -- Paired entity information
            paired_machine_name,
            paired_current_position,
            paired_current_position_name,
            paired_operating_status,
            paired_is_locked,

            -- Resource lock status
            is_route_locked,
            locked_by_route_id,
            route_lock_type,
            route_locked_at,
            route_locked_by,
            route_lock_expires_at,

            -- Route assignment context
            route_source_signal,
            route_dest_signal,
            route_state,
            route_direction,

            -- Status fields
            paired_sync_status,
            availability_status,

            -- Performance metrics
            avg_time_between_operations_seconds,

            -- Timestamps
            created_at,
            updated_at

        FROM railway_control.v_point_machines_complete
        ORDER BY machine_id
    )";

    if (pointQuery.exec(pointSql)) {
        while (pointQuery.next()) {
            points.append(convertPointMachineRowToVariant(pointQuery));
        }
        qDebug() << "? Loaded" << points.size() << "point machines with complete route assignment information";
    } else {
        qWarning() << "? SAFETY CRITICAL: Point machine complete view query failed:" << pointQuery.lastError().text();
    }

    return points;
}

QVariantList DatabaseManager::getTextLabelsList() {
    if (!connected) return QVariantList();

    qDebug() << "SAFETY: getTextLabelsList() - DIRECT DATABASE QUERY";

    QVariantList labels;
    QSqlQuery labelQuery(db);
    QString labelSql = "SELECT label_text, position_row, position_col, font_size, color, font_family, is_visible, label_type FROM railway_control.text_labels ORDER BY id";

    if (labelQuery.exec(labelSql)) {
        while (labelQuery.next()) {
            QVariantMap label;
            label["text"] = labelQuery.value("label_text").toString();
            label["row"] = labelQuery.value("position_row").toDouble();
            label["col"] = labelQuery.value("position_col").toDouble();
            label["fontSize"] = labelQuery.value("font_size").toInt();
            label["color"] = labelQuery.value("color").toString();
            label["fontFamily"] = labelQuery.value("font_family").toString();
            label["isVisible"] = labelQuery.value("is_visible").toBool();
            label["type"] = labelQuery.value("label_type").toString();
            labels.append(label);
        }
    } else {
        qWarning() << "SAFETY CRITICAL: Text label query failed:" << labelQuery.lastError().text();
    }

    return labels;
}

QVariantList DatabaseManager::getOuterSignalsList() {
    QVariantList result;
    QVariantList allSignals = getAllSignalsList();  // This is fine

    for (const auto& signalVar : allSignals) {
        QVariantMap signal = signalVar.toMap();
        if (signal["type"].toString() == "OUTER") {
            result.append(signal);
        }
    }

    return result;
}

QVariantList DatabaseManager::getHomeSignalsList() {
    QVariantList result;
    QVariantList allSignals = getAllSignalsList();  // This is fine

    for (const auto& signalVar : allSignals) {
        QVariantMap signal = signalVar.toMap();
        if (signal["type"].toString() == "HOME") {
            result.append(signal);
        }
    }

    return result;
}

QVariantList DatabaseManager::getStarterSignalsList() {
    QVariantList result;
    QVariantList allSignals = getAllSignalsList();  // This is fine

    for (const auto& signalVar : allSignals) {
        QVariantMap signal = signalVar.toMap();
        if (signal["type"].toString() == "STARTER") {
            result.append(signal);
        }
    }

    return result;
}

QVariantList DatabaseManager::getAdvanceStarterSignalsList() {
    QVariantList result;
    QVariantList allSignals = getAllSignalsList();  // This is fine

    for (const auto& signalVar : allSignals) {
        QVariantMap signal = signalVar.toMap();
        if (signal["type"].toString() == "ADVANCED_STARTER") {
            result.append(signal);
        }
    }

    return result;
}

// SAFETY: Individual object queries - DIRECT DATABASE
QVariantMap DatabaseManager::getSignalById(const QString& signalId) {
    if (!connected) return QVariantMap();

    qDebug() << "SAFETY: getSignalById(" << signalId << ") - QUERYING COMPLETE SIGNAL VIEW";

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT
            id,
            signal_id,
            signal_name,
            signal_type,
            signal_type_name,
            location_row as row,
            location_col as col,
            direction,
            is_locked,
            current_aspect,
            current_aspect_name,
            current_aspect_color,
            calling_on_aspect,
            calling_on_aspect_name,
            calling_on_aspect_color,
            loop_aspect,
            loop_aspect_name,
            loop_aspect_color,
            loop_signal_configuration,
            aspect_count,
            possible_aspects,
            is_active,
            location_description as location,
            last_changed_at,
            last_changed_by,
            interlocked_with,
            protected_track_circuits,
            manual_control_active,
            preceded_by_circuit_id,
            succeeded_by_circuit_id,
            is_route_signal,
            route_signal_type,
            created_at,
            updated_at
        FROM railway_control.v_signals_complete
        WHERE signal_id = ?
    )");

    query.addBindValue(signalId);

    if (query.exec() && query.next()) {
        return convertSignalRowToVariant(query);
    }

    qWarning() << "SAFETY: Signal" << signalId << "not found in complete view";
    return QVariantMap();
}

QVariantMap DatabaseManager::getTrackSegmentById(const QString& trackSegmentId) {
    if (!connected) return QVariantMap();

    qDebug() << " QUERY: getTrackSegmentById(" << trackSegmentId << ") - with locking status";

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT
            ts.id,
            ts.segment_id,
            ts.segment_name,
            ts.start_row,
            ts.start_col,
            ts.end_row,
            ts.end_col,
            ts.track_segment_type,
            ts.is_assigned,
            ts.is_overlap,
            ts.is_active,
            ts.circuit_id,
            ts.length_meters,
            ts.max_speed_kmh,
            ts.protecting_signals,
            ts.created_at,
            ts.updated_at,

            -- Circuit status information
            COALESCE(tc.is_occupied, false) as is_occupied,
            COALESCE(tc.is_assigned, false) as circuit_is_assigned,  -- ? NEW
            COALESCE(tc.is_overlap, false) as circuit_is_overlap,    -- ? NEW
            tc.occupied_by,

            -- ? UPDATED: Enhanced route assignment eligibility logic
            CASE
                WHEN tc.is_occupied = true THEN false
                WHEN tc.is_assigned = true OR tc.is_overlap = true THEN false  -- ? NEW: Consider circuit locking
                WHEN ts.is_assigned = true OR ts.is_overlap = true THEN false  -- ? NEW: Consider segment locking
                WHEN rl.is_active = true THEN false
                ELSE true
            END as route_assignment_eligible

        FROM railway_control.track_segments ts
        LEFT JOIN railway_control.track_circuits tc ON ts.circuit_id = tc.circuit_id
        LEFT JOIN railway_control.resource_locks rl ON (
            rl.resource_type = 'TRACK_CIRCUIT'
            AND rl.resource_id = tc.circuit_id
            AND rl.is_active = true
        )
        WHERE ts.segment_id = ?
    )");

    query.addBindValue(trackSegmentId);

    if (query.exec() && query.next()) {
        return convertTrackSegmentRowToVariant(query);
    }

    qWarning() << "? Track segment" << trackSegmentId << "not found";
    return QVariantMap();
}

QVariantMap DatabaseManager::getPointMachineById(const QString& machineId) {
    if (!connected) return QVariantMap();

    qDebug() << "SAFETY: getPointMachineById(" << machineId << ") - DIRECT DATABASE QUERY";

    QSqlQuery query(db);

    //  Added paired_entity to SELECT statement
    query.prepare(R"(
        SELECT
            id,
            machine_id,
            machine_name,
            junction_row,
            junction_col,
            root_track_segment_connection,
            normal_track_segment_connection,
            reverse_track_segment_connection,

            -- Position information
            current_position,
            current_position_name,
            position_description,
            position_pathfinding_weight,
            position_default_transition_time_ms,

            -- Operational status and timing
            operating_status,
            transition_time_ms,
            last_operated_at,
            last_operated_by,
            operation_count,

            -- Locking and safety
            is_locked,
            lock_reason,
            safety_interlocks,
            protected_signals,

            -- Route assignment extensions
            paired_entity,
            host_track_circuit,
            route_locking_enabled,
            auto_normalize_after_route,

            -- Paired entity information
            paired_machine_name,
            paired_current_position,
            paired_current_position_name,
            paired_operating_status,
            paired_is_locked,

            -- Resource lock status
            is_route_locked,
            locked_by_route_id,
            route_lock_type,
            route_locked_at,
            route_locked_by,
            route_lock_expires_at,

            -- Route assignment context
            route_source_signal,
            route_dest_signal,
            route_state,
            route_direction,

            -- Status fields
            paired_sync_status,
            availability_status,

            -- Performance metrics
            avg_time_between_operations_seconds,

            -- Timestamps
            created_at,
            updated_at

        FROM railway_control.v_point_machines_complete pm
        WHERE pm.machine_id = ?
    )");
    query.addBindValue(machineId);

    if (query.exec() && query.next()) {
        return convertPointMachineRowToVariant(query);
    } else {
        qWarning() << "Failed to get point machine:" << machineId << query.lastError().text();
    }

    return QVariantMap();
}

QVariantList DatabaseManager::getPointMachinesByTrackCircuit(const QString& trackCircuitId) {
    if (!connected) return QVariantList();

    qDebug() << "SAFETY: getPointMachinesByTrackCircuit(" << trackCircuitId << ") - DIRECT DATABASE QUERY";

    QVariantList points;
    QSqlQuery query(db);

    query.prepare(R"(
        SELECT
            id, machine_id, machine_name, host_track_circuit,
            current_position, availability_status,
            junction_row, junction_col,
            normal_track_segment_connection,
            reverse_track_segment_connection
        FROM railway_control.v_point_machines_complete
        WHERE host_track_circuit = ?
        ORDER BY machine_id
    )");
    query.addBindValue(trackCircuitId);

    if (query.exec()) {
        while (query.next()) {
            points.append(convertPointMachineRowToVariant(query));
        }
        qDebug() << "  Found" << points.size() << "point machines for track circuit" << trackCircuitId;
    } else {
        qWarning() << " Failed to get point machines for track circuit:" << trackCircuitId << query.lastError().text();
    }

    return points;
}


bool DatabaseManager::updateMainSignalAspect(const QString& signalId, const QString& newAspect) {
    if (!connected) return false;

    QElapsedTimer timer;
    timer.start();

    qDebug() << "SAFETY: Updating MAIN signal aspect:" << signalId << "to aspect:" << newAspect;

    // Get current main aspect for interlocking validation
    QString currentAspect = getCurrentSignalAspect(signalId);
    if (currentAspect.isEmpty()) {
        qWarning() << "Could not get current main aspect for signal:" << signalId;
        emit operationBlocked(signalId, "Signal not found or invalid main aspect state");
        return false;
    }

    // Main signal interlocking validation
    if (m_interlockingService) {
        auto validation = m_interlockingService->validateMainSignalOperation(
            signalId, currentAspect, newAspect, "HMI_USER");

        if (!validation.isAllowed()) {
            qDebug() << "Main signal operation blocked by interlocking:" << validation.getReason();
            emit operationBlocked(signalId, validation.getReason());
            return false;
        }

        qDebug() << "Main signal interlocking validation passed for signal" << signalId;
    } else {
        qWarning() << "Interlocking service not available - proceeding without validation";
    }

    // Database transaction for main signal update
    QSqlQuery query(db);

    if (!db.transaction()) {
        qWarning() << "Failed to start transaction for main signal:" << db.lastError().text();
        return false;
    }

    // Call existing main signal update function
    query.prepare("SELECT railway_control.update_signal_aspect(?, ?, 'HMI_USER')");
    query.addBindValue(signalId);
    query.addBindValue(newAspect);

    bool success = false;
    if (query.exec() && query.next()) {
        success = query.value(0).toBool();
        if (success && db.commit()) {
            // Verify main aspect change
            QSqlQuery verifyQuery(db);
            verifyQuery.prepare("SELECT current_aspect_id FROM railway_control.signals WHERE signal_id = ?");
            verifyQuery.addBindValue(signalId);
            if (verifyQuery.exec() && verifyQuery.next()) {
                int currentAspectId = verifyQuery.value(0).toInt();
                qDebug() << "SAFETY: Main signal" << signalId << "now has aspect_id:" << currentAspectId;
            }

            // Emit success signals
            emit signalUpdated(signalId);
            emit signalsChanged();

            qDebug() << "Main signal operation completed in" << timer.elapsed() << "ms";
            return true;
        } else {
            qWarning() << "Main signal update failed:" << query.lastError().text();
            db.rollback();
            return false;
        }
    } else {
        qWarning() << "Main signal query execution failed:" << query.lastError().text();
        db.rollback();
        return false;
    }
}

bool DatabaseManager::updateSubsidiarySignalAspect(const QString& signalId,
                                                   const QString& aspectType,
                                                   const QString& newAspect) {
    if (!connected) return false;

    QElapsedTimer timer;
    timer.start();

    qDebug() << "SAFETY: Updating SUBSIDIARY signal aspect:" << signalId
             << "type:" << aspectType << "to aspect:" << newAspect;

    // Validate aspect type
    if (aspectType != "CALLING_ON" && aspectType != "LOOP") {
        qWarning() << "Invalid subsidiary aspect type:" << aspectType;
        emit operationBlocked(signalId, "Invalid subsidiary signal type: " + aspectType);
        return false;
    }

    // Get current subsidiary aspect for validation
    QString currentSubsidiaryAspect = getCurrentSubsidiaryAspect(signalId, aspectType);
    if (currentSubsidiaryAspect.isEmpty()) {
        qWarning() << "Could not get current subsidiary aspect for signal:" << signalId << "type:" << aspectType;
        emit operationBlocked(signalId, "Signal not found or invalid subsidiary aspect state");
        return false;
    }

    // Interlocking validation for subsidiary signals
    if (m_interlockingService) {
        auto validation = m_interlockingService->validateSubsidiarySignalOperation(
            signalId, aspectType, currentSubsidiaryAspect, newAspect, "HMI_USER");

        if (!validation.isAllowed()) {
            qDebug() << "Subsidiary signal operation blocked by interlocking:" << validation.getReason();
            emit operationBlocked(signalId, validation.getReason());
            return false;
        }

        qDebug() << "Subsidiary signal interlocking validation passed for signal" << signalId;
    } else {
        qWarning() << "Interlocking service not available - proceeding without validation";
    }

    // Database transaction for subsidiary signal update
    QSqlQuery query(db);

    if (!db.transaction()) {
        qWarning() << "Failed to start transaction for subsidiary signal:" << db.lastError().text();
        return false;
    }

    // Call subsidiary signal update function (TO BE CREATED)
    query.prepare("SELECT railway_control.update_subsidiary_signal_aspect(?, ?, ?, 'HMI_USER')");
    query.addBindValue(signalId);
    query.addBindValue(aspectType);
    query.addBindValue(newAspect);

    bool success = false;
    if (query.exec() && query.next()) {
        success = query.value(0).toBool();
        if (success && db.commit()) {
            // Verify subsidiary aspect change
            QString columnName = (aspectType == "CALLING_ON") ? "calling_on_aspect" : "loop_aspect";
            QSqlQuery verifyQuery(db);
            verifyQuery.prepare(QString("SELECT %1 FROM railway_control.signals WHERE signal_id = ?").arg(columnName));
            verifyQuery.addBindValue(signalId);
            if (verifyQuery.exec() && verifyQuery.next()) {
                QString currentValue = verifyQuery.value(0).toString();
                qDebug() << "SAFETY: Subsidiary signal" << signalId << aspectType
                         << "now has value:" << currentValue;
            }

            // Emit success signals
            emit signalUpdated(signalId);
            emit signalsChanged();

            qDebug() << "Subsidiary signal operation completed in" << timer.elapsed() << "ms";
            return true;
        } else {
            qWarning() << "Subsidiary signal update failed:" << query.lastError().text();
            db.rollback();
            return false;
        }
    } else {
        qWarning() << "Subsidiary signal query execution failed:" << query.lastError().text();
        db.rollback();
        return false;
    }
}

// SAFETY: Update operations - NO CACHE INVALIDATION
bool DatabaseManager::updateSignalAspect(const QString& signalId,
                                         const QString& aspectType,
                                         const QString& newAspect) {
    if (!connected) {
        qWarning() << "Database not connected - cannot update signal aspect";
        return false;
    }

    qDebug() << "ROUTER: Signal aspect update request:"
             << "Signal:" << signalId
             << "Type:" << aspectType
             << "New aspect:" << newAspect;

    // Validate aspect type parameter
    if (aspectType != "MAIN" && aspectType != "CALLING_ON" && aspectType != "LOOP") {
        qWarning() << "Invalid aspect type:" << aspectType
                   << "Must be 'MAIN', 'CALLING_ON', or 'LOOP'";
        emit operationBlocked(signalId, "Invalid aspect type: " + aspectType);
        return false;
    }

    // Route to appropriate function based on aspect type
    if (aspectType == "MAIN") {
        qDebug() << "ROUTER: Routing to updateMainSignalAspect()";
        return updateMainSignalAspect(signalId, newAspect);
    }
    else if (aspectType == "CALLING_ON" || aspectType == "LOOP") {
        qDebug() << "ROUTER: Routing to updateSubsidiarySignalAspect()";
        return updateSubsidiarySignalAspect(signalId, aspectType, newAspect);
    }

    // Should never reach here due to validation above
    qWarning() << "ROUTER: Unexpected routing failure for aspect type:" << aspectType;
    return false;
}

QString DatabaseManager::getPairedMachine(const QString& machineId) {
    QSqlQuery query(db);
    query.prepare("SELECT paired_entity FROM railway_control.point_machines WHERE machine_id = ?");
    query.addBindValue(machineId);

    if (query.exec() && query.next()) {
        return query.value(0).toString();
    }
    return QString();
}

bool DatabaseManager::updatePointMachinePosition(const QString& machineId, const QString& newPosition) {
    if (!connected) return false;

    qDebug() << "SAFETY: Updating point machine:" << machineId << "to position:" << newPosition;

    // Step 1: Get current positions for paired validation
    QString currentPosition = getCurrentPointPosition(machineId);
    if (currentPosition.isEmpty()) {
        qWarning() << "Could not get current position for point machine:" << machineId;
        emit operationBlocked(machineId, "Point machine not found or invalid state");
        return false;
    }

    // Step 2: Get paired machine info for comprehensive validation
    QString pairedMachineId = getPairedMachine(machineId);

    if (!pairedMachineId.isEmpty()) {
        QString pairedCurrentPosition = getCurrentPointPosition(pairedMachineId);

        // === USE PAIRED VALIDATION ===
        if (m_interlockingService) {
            auto validation = m_interlockingService->validatePairedPointMachineOperation(
                machineId, pairedMachineId, currentPosition, pairedCurrentPosition, newPosition, "HMI_USER");

            if (!validation.isAllowed()) {
                qDebug() << "Paired point machine operation blocked by interlocking:" << validation.getReason();
                emit operationBlocked(machineId, validation.getReason());
                return false;
            }
        }
    } else {
        // === SINGLE MACHINE VALIDATION ===
        if (m_interlockingService) {
            auto validation = m_interlockingService->validatePointMachineOperation(
                machineId, currentPosition, newPosition, "HMI_USER");

            if (!validation.isAllowed()) {
                qDebug() << "Point machine operation blocked by interlocking:" << validation.getReason();
                emit operationBlocked(machineId, validation.getReason());
                return false;
            }
        }
    }

    qDebug() << "Interlocking validation passed for all affected machines";

    // Step 3: Execute atomic database operation (rest remains unchanged)
    if (!db.transaction()) {
        qWarning() << "SAFETY CRITICAL: Failed to start transaction for point machine update";
        return false;
    }

    QSqlQuery query(db);
    query.prepare("SELECT railway_control.update_point_position_paired(?, ?, 'HMI_USER')");
    query.addBindValue(machineId);
    query.addBindValue(newPosition);

    bool success = false;
    if (query.exec() && query.next()) {
        QJsonDocument doc = QJsonDocument::fromJson(query.value(0).toString().toUtf8());
        QJsonObject result = doc.object();

        success = result["success"].toBool();
        bool positionMismatch = result["position_mismatch"].toBool();
        QJsonArray updatedMachines = result["machines_updated"].toArray();
        QString message = result["message"].toString();

        if (success) {
            if (db.commit()) {
                qDebug() << "Point machine update successful:" << message;

                // Emit appropriate signals
                QStringList machinesList;
                for (const auto& machine : updatedMachines) {
                    machinesList.append(machine.toString());
                    emit pointMachineUpdated(machine.toString());
                }

                if (machinesList.size() > 1) {
                    emit pairedMachinesUpdated(machinesList);
                }

                if (positionMismatch) {
                    qCritical() << "SAFETY WARNING: Position mismatch corrected for paired machines:"
                                << machineId << "and" << pairedMachineId;
                    emit positionMismatchCorrected(machineId, pairedMachineId);
                }

                emit pointMachinesChanged();
                return true;
            } else {
                qWarning() << "SAFETY CRITICAL: Failed to commit transaction:" << db.lastError().text();
                db.rollback();
                return false;
            }
        } else {
            qWarning() << "SAFETY CRITICAL: Point machine update failed:" << message;
            db.rollback();
            return false;
        }
    }

    qWarning() << "SAFETY CRITICAL: Point machine update query failed:" << query.lastError().text();
    db.rollback();
    return false;
}

QString DatabaseManager::getCurrentSignalAspect(const QString& signalId) {
    if (!connected) {
        qWarning() << "Database not connected - cannot get signal aspect";
        return QString();
    }

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT sa.aspect_code
        FROM railway_control.signals s
        LEFT JOIN railway_config.signal_aspects sa ON s.current_aspect_id = sa.id
        WHERE s.signal_id = ?
    )");
    query.addBindValue(signalId);

    if (!query.exec()) {
        qWarning() << "Failed to get current aspect for signal" << signalId << ":" << query.lastError().text();
        return QString();
    }

    if (query.next()) {
        return query.value(0).toString();
    }

    qWarning() << "Signal not found:" << signalId;
    return QString();
}

//   ENHANCED: getCurrentSubsidiaryAspect to work with new schema
QString DatabaseManager::getCurrentSubsidiaryAspect(const QString& signalId, const QString& aspectType) {
    if (!connected) return QString();

    QString columnName;
    if (aspectType == "CALLING_ON") {
        columnName = "sa_calling.aspect_code";
    } else if (aspectType == "LOOP") {
        columnName = "sa_loop.aspect_code";
    } else {
        qWarning() << " Invalid subsidiary aspect type:" << aspectType;
        return QString();
    }

    QSqlQuery query(db);
    QString sql = QString(R"(
        SELECT COALESCE(%1, 'OFF') as aspect_code
        FROM railway_control.signals s
        LEFT JOIN railway_config.signal_aspects sa_calling ON s.calling_on_aspect_id = sa_calling.id
        LEFT JOIN railway_config.signal_aspects sa_loop ON s.loop_aspect_id = sa_loop.id
        WHERE s.signal_id = ?
    )").arg(columnName);

    query.prepare(sql);
    query.addBindValue(signalId);

    if (query.exec() && query.next()) {
        return query.value(0).toString();
    }

    qWarning() << " Failed to get current subsidiary aspect:" << query.lastError().text();
    return QString();
}

QString DatabaseManager::getCurrentPointPosition(const QString& machineId) {
    QSqlQuery query(db);
    query.prepare(R"(
        SELECT pp.position_code
        FROM railway_control.point_machines pm
        LEFT JOIN railway_config.point_positions pp ON pm.current_position_id = pp.id
        WHERE pm.machine_id = ?
    )");
    query.addBindValue(machineId);

    if (query.exec() && query.next()) {
        return query.value(0).toString();
    }
    return QString();
}

QStringList DatabaseManager::getInterlockedSignals(const QString& signalId) {
    auto signalData = getSignalById(signalId);
    if (!signalData.isEmpty()) {
        return signalData["interlockedWith"].toStringList();
    }
    return QStringList();
}

void DatabaseManager::setInterlockingService(InterlockingService* service) {
    m_interlockingService = service;
    qDebug() << "Interlocking service connected to DatabaseManager";
}

// ADD: Database access method for interlocking branches
QSqlDatabase DatabaseManager::getDatabase() const {
    return db;
}


bool DatabaseManager::updateTrackSegmentOccupancy(const QString& trackSegmentId, bool isOccupied) {
    if (!connected) return false;

    qDebug() << "HARDWARE: Track segment occupancy change:" << trackSegmentId << "→" << isOccupied;
    qDebug() << "             (This updates the CIRCUIT that contains this segment)";

    // Get previous state for interlocking comparison
    bool wasOccupied = false;
    auto currentTrackSegmentData = getTrackSegmentById(trackSegmentId);
    if (!currentTrackSegmentData.isEmpty()) {
        wasOccupied = currentTrackSegmentData["occupied"].toBool();
    }

    // UPDATED: Use the wrapper function that maps segment to circuit
    QSqlQuery query(db);
    query.prepare("SELECT railway_control.update_track_segment_occupancy(?, ?, NULL, 'HARDWARE_AUTO')");
    query.addBindValue(trackSegmentId);
    query.addBindValue(isOccupied);

    if (query.exec() && query.next()) {
        bool success = query.value(0).toBool();
        if (success) {
            // REACTIVE: Trigger automatic interlocking enforcement
            if (m_interlockingService && m_interlockingService->isOperational()) {
                QMetaObject::invokeMethod(m_interlockingService,
                                          "reactToTrackSegmentOccupancyChange", Qt::QueuedConnection,
                                          Q_ARG(QString, trackSegmentId),
                                          Q_ARG(bool, wasOccupied),
                                          Q_ARG(bool, isOccupied));
            }

            emit trackSegmentUpdated(trackSegmentId);  //  Consistent naming
            emit trackSegmentsChanged();               //  Consistent naming
        }
        return success;
    }

    qCritical() << "HARDWARE FAILURE: Track segment occupancy update failed:" << query.lastError().text();
    return false;
}

bool DatabaseManager::updateTrackCircuitOccupancy(const QString& trackCircuitId, bool isOccupied) {
    if (!connected) return false;

    qDebug() << "CIRCUIT: Track Segment circuit occupancy change:" << trackCircuitId << "→" << isOccupied;

    QSqlQuery query(db);
    query.prepare("SELECT railway_control.update_track_circuit_occupancy(?, ?, NULL, 'HARDWARE_AUTO')");
    query.addBindValue(trackCircuitId);
    query.addBindValue(isOccupied);

    if (query.exec() && query.next()) {
        bool success = query.value(0).toBool();
        if (success) {
            emit trackCircuitsChanged();  // NEW: Circuit-specific signal
            emit trackSegmentsChanged();  // Also update segments since they depend on circuits
        }
        return success;
    }

    qCritical() << "CIRCUIT FAILURE: Track circuit occupancy update failed:" << query.lastError().text();
    return false;
}

bool DatabaseManager::getTrackCircuitOccupancy(const QString& trackCircuitId) {
    QSqlQuery query(db);
    query.prepare("SELECT is_occupied FROM railway_control.track_circuits WHERE circuit_id = ?");
    query.addBindValue(trackCircuitId);
    if (query.exec() && query.next()) {
        return query.value(0).toBool();
    }
    return true; // Safe default
}

QVariantList DatabaseManager::getTrackSegmentsByCircuitId(const QString& trackCircuitId) {
    if (!connected) return QVariantList();

    qDebug() << " QUERY: getTrackSegmentsByCircuitId(" << trackCircuitId << ") - with locking status";

    QVariantList segments;
    QSqlQuery query(db);
    query.prepare(R"(
        SELECT
            ts.id,
            ts.segment_id,
            ts.segment_name,
            ts.start_row,
            ts.start_col,
            ts.end_row,
            ts.end_col,
            ts.track_segment_type,
            ts.is_assigned,
            ts.is_overlap,  --   NEW: Track segment overlap status
            ts.is_active,
            ts.circuit_id,
            ts.length_meters,
            ts.max_speed_kmh,
            ts.protecting_signals,
            ts.created_at,
            ts.updated_at,

            -- Circuit status information
            COALESCE(tc.is_occupied, false) as is_occupied,
            COALESCE(tc.is_assigned, false) as circuit_is_assigned,  --   NEW
            COALESCE(tc.is_overlap, false) as circuit_is_overlap,    --   NEW
            tc.occupied_by,

            --   UPDATED: Enhanced route assignment eligibility logic
            CASE
                WHEN tc.is_occupied = true THEN false
                WHEN tc.is_assigned = true OR tc.is_overlap = true THEN false  --   NEW: Consider circuit locking
                WHEN ts.is_assigned = true OR ts.is_overlap = true THEN false  --   NEW: Consider segment locking
                WHEN rl.is_active = true THEN false
                ELSE true
            END as route_assignment_eligible

        FROM railway_control.track_segments ts
        LEFT JOIN railway_control.track_circuits tc ON ts.circuit_id = tc.circuit_id
        LEFT JOIN railway_control.resource_locks rl ON (
            rl.resource_type = 'TRACK_CIRCUIT'
            AND rl.resource_id = tc.circuit_id
            AND rl.is_active = true
        )
        WHERE ts.circuit_id = ?
        ORDER BY ts.segment_id
    )");

    query.addBindValue(trackCircuitId);

    if (query.exec()) {
        while (query.next()) {
            segments.append(convertTrackSegmentRowToVariant(query));
        }
        qDebug() << "  Found" << segments.size() << "segments with locking status for circuit" << trackCircuitId;
    } else {
        qWarning() << " Failed to get segments for circuit" << trackCircuitId << ":" << query.lastError().text();
    }

    return segments;
}

QVariantList DatabaseManager::getTrackCircuitsList() {
    if (!connected) return QVariantList();

    qDebug() << " SAFETY: getTrackCircuitsList() - DIRECT DATABASE QUERY with locking status";

    QVariantList circuits;
    QSqlQuery query(db);

    //   ENHANCED: Include all schema fields including new locking columns
    QString sql = R"(
        SELECT
            id,
            circuit_id,
            circuit_name,
            is_occupied,
            occupied_by,
            is_assigned,  --   NEW: Route assignment status
            is_overlap,   --   NEW: Overlap assignment status
            is_active,
            last_changed_at,
            protecting_signals,
            length_meters,
            max_speed_kmh,
            created_at,
            updated_at
        FROM railway_control.track_circuits
        ORDER BY circuit_id
    )";

    if (query.exec(sql)) {
        while (query.next()) {
            circuits.append(convertTrackCircuitRowToVariant(query));
        }
        qDebug() << "  Loaded" << circuits.size() << "track circuits with complete information and locking status";
    } else {
        qWarning() << " SAFETY CRITICAL: Track circuits query failed:" << query.lastError().text();
    }

    return circuits;
}

// === NEW: TRIPLE-SOURCE PROTECTION SIGNAL IMPLEMENTATIONS ===

QStringList DatabaseManager::getProtectingSignalsFromInterlockingRules(const QString& circuitId) {
    if (!connected) return QStringList();

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT source_entity_id
        FROM railway_control.interlocking_rules
        WHERE target_entity_type = 'TRACK_CIRCUIT'
          AND target_entity_id = ?
          AND source_entity_type = 'SIGNAL'
          AND target_constraint = 'MUST_BE_CLEAR'
          AND rule_type = 'PROTECTING'
          AND is_active = TRUE
        ORDER BY source_entity_id
    )");
    query.addBindValue(circuitId);

    QStringList signalList;
    if (query.exec()) {
        while (query.next()) {
            signalList.append(query.value(0).toString());
        }
    } else {
        qWarning() << " DatabaseManager: Failed to query interlocking rules for track circuit" << circuitId << ":" << query.lastError().text();
    }

    return signalList;
}

QStringList DatabaseManager::getProtectingSignalsFromTrackCircuits(const QString& circuitId) {
    if (!connected) return QStringList();

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT protecting_signals
        FROM railway_control.track_circuits
        WHERE circuit_id = ?
          AND is_active = TRUE
    )");
    query.addBindValue(circuitId);

    QStringList signalList;
    if (query.exec() && query.next()) {
        // Parse PostgreSQL TEXT[] array format: {signal1,signal2,signal3}
        QString protectingSignalsStr = query.value(0).toString();
        if (!protectingSignalsStr.isEmpty() && protectingSignalsStr != "{}") {
            protectingSignalsStr = protectingSignalsStr.mid(1, protectingSignalsStr.length() - 2); // Remove { }
            signalList = protectingSignalsStr.split(",", Qt::SkipEmptyParts);
            for (QString& signal : signalList) {
                signal = signal.trimmed();
            }
        }
    } else if (!query.exec()) {
        qWarning() << " DatabaseManager: Failed to query track circuits for circuit" << circuitId << ":" << query.lastError().text();
    }

    return signalList;
}

QStringList DatabaseManager::getProtectingSignalsFromTrackSegments(const QString& trackSegmentId) {
    if (!connected) return QStringList();

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT protecting_signals
        FROM railway_control.track_segments
        WHERE segment_id = ?
          AND is_active = TRUE
    )");
    query.addBindValue(trackSegmentId);

    QStringList signalList;
    if (query.exec() && query.next()) {
        // Parse PostgreSQL TEXT[] array format: {signal1,signal2,signal3}
        QString protectingSignalsStr = query.value(0).toString();
        if (!protectingSignalsStr.isEmpty() && protectingSignalsStr != "{}") {
            protectingSignalsStr = protectingSignalsStr.mid(1, protectingSignalsStr.length() - 2); // Remove { }
            signalList = protectingSignalsStr.split(",", Qt::SkipEmptyParts);
            for (QString& signal : signalList) {
                signal = signal.trimmed();
            }
        }
    } else if (!query.exec()) {
        qWarning() << " DatabaseManager: Failed to query track segments for segment" << trackSegmentId << ":" << query.lastError().text();
    }

    return signalList;
}

QStringList DatabaseManager::getProtectedTrackCircuitsFromInterlockingRules(const QString& signalId) {
    if (!connected) return QStringList();

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT target_entity_id
        FROM railway_control.interlocking_rules
        WHERE source_entity_type = 'SIGNAL'
          AND source_entity_id = ?
          AND target_entity_type = 'TRACK_CIRCUIT'
          AND target_constraint = 'MUST_BE_CLEAR'
          AND rule_type = 'PROTECTING'
          AND is_active = TRUE
        ORDER BY target_entity_id
    )");
    query.addBindValue(signalId);

    QStringList trackCircuits;
    if (!query.exec()) {
        qCritical() << " DatabaseManager: Failed to query interlocking rules for signal" << signalId << ":" << query.lastError().text();
        return trackCircuits;
    }

    while (query.next()) {
        trackCircuits.append(query.value(0).toString());
    }

    return trackCircuits;
}

QVariantMap DatabaseManager::getTrackCircuitById(const QString& circuitId) {
    if (!connected) return QVariantMap();

    qDebug() << " QUERY: getTrackCircuitById(" << circuitId << ") - with locking status";

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT
            id,
            circuit_id,
            circuit_name,
            is_occupied,
            occupied_by,
            is_assigned,  --   NEW: Route assignment status
            is_overlap,   --   NEW: Overlap assignment status
            is_active,
            last_changed_at,
            protecting_signals,
            length_meters,
            max_speed_kmh,
            created_at,
            updated_at
        FROM railway_control.track_circuits
        WHERE circuit_id = ?
    )");

    query.addBindValue(circuitId);

    if (query.exec() && query.next()) {
        return convertTrackCircuitRowToVariant(query);
    }

    qWarning() << " Track circuit" << circuitId << "not found";
    return QVariantMap();
}


// bool DatabaseManager::updateTrackSegmentAssignment(const QString& segmentId, bool isAssigned) {
//     if (!connected) return false;

//     qDebug() << "SAFETY: Updating trackSegment assignment:" << segmentId << "to" << isAssigned;

//     QSqlQuery query(db);
//     query.prepare("SELECT railway_control.update_track_segment_assignment(?, ?, 'HMI_USER')");
//     query.addBindValue(segmentId);
//     query.addBindValue(isAssigned);

//     if (query.exec() && query.next()) {
//         bool success = query.value(0).toBool();
//         if (success) {
//             // SAFETY: No cache invalidation - just emit signals
//             emit trackSegmentUpdated(segmentId);
//             emit trackSegmentsChanged();
//         }
//         return success;
//     }

//     qWarning() << "SAFETY CRITICAL: Track Segment assignment update failed:" << query.lastError().text();
//     return false;
// }

// SAFETY: Row conversion helpers (unchanged)
QVariantMap DatabaseManager::convertSignalRowToVariant(const QSqlQuery& query) {
    QVariantMap signal;

    //   BASIC SIGNAL INFO
    signal["id"] = query.value("signal_id").toString();
    signal["name"] = query.value("signal_name").toString();
    signal["type"] = query.value("signal_type").toString();
    signal["typeName"] = query.value("signal_type_name").toString();
    signal["row"] = query.value("row").toDouble();
    signal["col"] = query.value("col").toDouble();
    signal["direction"] = query.value("direction").toString();
    signal["isActive"] = query.value("is_active").toBool();
    signal["isLocked"] = query.value("is_locked").toBool();  //   NEW: Resource locking status
    signal["location"] = query.value("location").toString();

    //   ASPECT INFORMATION
    signal["currentAspect"] = query.value("current_aspect").toString();
    signal["currentAspectName"] = query.value("current_aspect_name").toString();
    signal["currentAspectColor"] = query.value("current_aspect_color").toString();
    signal["callingOnAspect"] = query.value("calling_on_aspect").toString();
    signal["callingOnAspectName"] = query.value("calling_on_aspect_name").toString();
    signal["callingOnAspectColor"] = query.value("calling_on_aspect_color").toString();
    signal["loopAspect"] = query.value("loop_aspect").toString();
    signal["loopAspectName"] = query.value("loop_aspect_name").toString();
    signal["loopAspectColor"] = query.value("loop_aspect_color").toString();
    signal["loopSignalConfiguration"] = query.value("loop_signal_configuration").toString();
    signal["aspectCount"] = query.value("aspect_count").toInt();

    //   OPERATIONAL INFO
    signal["manualControlActive"] = query.value("manual_control_active").toBool();
    signal["lastChangedAt"] = query.value("last_changed_at").toString();
    signal["lastChangedBy"] = query.value("last_changed_by").toString();

    //   ROUTE ASSIGNMENT FIELDS
    signal["precededByCircuitId"] = query.value("preceded_by_circuit_id").toString();
    signal["succeededByCircuitId"] = query.value("succeeded_by_circuit_id").toString();
    signal["isRouteSignal"] = query.value("is_route_signal").toBool();
    signal["routeSignalType"] = query.value("route_signal_type").toString();

    //   TIMESTAMPS
    signal["createdAt"] = query.value("created_at").toString();
    signal["updatedAt"] = query.value("updated_at").toString();

    //   HANDLE POSTGRESQL ARRAYS
    // Convert possible_aspects array
    QString aspectsStr = query.value("possible_aspects").toString();
    if (!aspectsStr.isEmpty()) {
        aspectsStr = aspectsStr.mid(1, aspectsStr.length() - 2); // Remove { }
        signal["possibleAspects"] = aspectsStr.split(",");
    } else {
        signal["possibleAspects"] = QStringList();
    }

    // Convert interlocked_with array (integers)
    QString interlockStr = query.value("interlocked_with").toString();
    if (!interlockStr.isEmpty()) {
        interlockStr = interlockStr.mid(1, interlockStr.length() - 2); // Remove { }
        QStringList interlockList = interlockStr.split(",");
        QVariantList interlockVariants;
        for (const QString& item : interlockList) {
            bool ok;
            int intValue = item.trimmed().toInt(&ok);
            if (ok) {
                interlockVariants.append(intValue);
            }
        }
        signal["interlocked_with"] = interlockVariants;
    } else {
        signal["interlocked_with"] = QVariantList();
    }

    // Convert protected_track_circuits array (text)
    QString circuitsStr = query.value("protected_track_circuits").toString();
    if (!circuitsStr.isEmpty()) {
        circuitsStr = circuitsStr.mid(1, circuitsStr.length() - 2); // Remove { }
        QStringList circuitsList = circuitsStr.split(",");
        QStringList cleanCircuits;
        for (const QString& circuit : circuitsList) {
            cleanCircuits.append(circuit.trimmed());
        }
        signal["protectedTrackCircuits"] = cleanCircuits;
    } else {
        signal["protectedTrackCircuits"] = QStringList();
    }

    return signal;
}

QVariantMap DatabaseManager::convertTrackSegmentRowToVariant(const QSqlQuery& query) {
    QVariantMap trackSegment;

    //   BASIC SEGMENT INFO
    trackSegment["id"] = query.value("segment_id").toString();
    trackSegment["name"] = query.value("segment_name").toString();
    trackSegment["startRow"] = query.value("start_row").toDouble();
    trackSegment["startCol"] = query.value("start_col").toDouble();
    trackSegment["endRow"] = query.value("end_row").toDouble();
    trackSegment["endCol"] = query.value("end_col").toDouble();
    trackSegment["trackSegmentType"] = query.value("track_segment_type").toString();
    trackSegment["isActive"] = query.value("is_active").toBool();
    trackSegment["circuitId"] = query.value("circuit_id").toString();

    //   SEGMENT ASSIGNMENT AND LOCKING STATUS
    trackSegment["assigned"] = query.value("is_assigned").toBool();
    trackSegment["isOverlap"] = query.value("is_overlap").toBool();  //   NEW: Segment overlap status

    //   CIRCUIT OCCUPANCY AND LOCKING STATUS
    trackSegment["occupied"] = query.value("is_occupied").toBool();
    trackSegment["occupiedBy"] = query.value("occupied_by").toString();
    trackSegment["circuitIsAssigned"] = query.value("circuit_is_assigned").toBool();  //   NEW: Circuit assignment status
    trackSegment["circuitIsOverlap"] = query.value("circuit_is_overlap").toBool();    //   NEW: Circuit overlap status

    //   PHYSICAL PROPERTIES
    trackSegment["lengthMeters"] = query.value("length_meters").toDouble();
    trackSegment["maxSpeedKmh"] = query.value("max_speed_kmh").toInt();

    //   TIMESTAMPS
    trackSegment["createdAt"] = query.value("created_at").toString();
    trackSegment["updatedAt"] = query.value("updated_at").toString();

    //   ROUTE ASSIGNMENT STATUS
    trackSegment["routeAssignmentEligible"] = query.value("route_assignment_eligible").toBool();

    //   HANDLE PROTECTING_SIGNALS ARRAY
    QString protectingSignalsStr = query.value("protecting_signals").toString();
    if (!protectingSignalsStr.isEmpty()) {
        protectingSignalsStr = protectingSignalsStr.mid(1, protectingSignalsStr.length() - 2); // Remove { }
        QStringList signalsList = protectingSignalsStr.split(",");
        QStringList cleanSignals;
        for (const QString& signal : signalsList) {
            cleanSignals.append(signal.trimmed());
        }
        trackSegment["protectingSignals"] = cleanSignals;
    } else {
        trackSegment["protectingSignals"] = QStringList();
    }

    return trackSegment;
}

QVariantMap DatabaseManager::convertPointMachineRowToVariant(const QSqlQuery& query) {
    QVariantMap pm;

    //   BASIC MACHINE INFO
    pm["id"] = query.value("machine_id").toString();
    pm["name"] = query.value("machine_name").toString();
    pm["operatingStatus"] = query.value("operating_status").toString();
    pm["transitionTime"] = query.value("transition_time_ms").toInt();

    //   POSITION INFORMATION (Enhanced)
    pm["position"] = query.value("current_position").toString();
    pm["currentPosition"] = query.value("current_position").toString();
    pm["currentPositionName"] = query.value("current_position_name").toString();
    pm["positionDescription"] = query.value("position_description").toString();
    pm["positionPathfindingWeight"] = query.value("position_pathfinding_weight").toDouble();
    pm["positionDefaultTransitionTime"] = query.value("position_default_transition_time_ms").toInt();

    //   OPERATIONAL STATUS AND TIMING
    pm["lastOperatedAt"] = query.value("last_operated_at").toString();
    pm["lastOperatedBy"] = query.value("last_operated_by").toString();
    pm["operationCount"] = query.value("operation_count").toInt();

    //   LOCKING AND SAFETY
    pm["isLocked"] = query.value("is_locked").toBool();
    pm["lockReason"] = query.value("lock_reason").toString();

    //   ROUTE ASSIGNMENT EXTENSIONS
    QString pairedEntity = query.value("paired_entity").toString();
    pm["pairedEntity"] = pairedEntity.isEmpty() ? QVariant() : pairedEntity;
    pm["isPaired"] = !pairedEntity.isEmpty();
    pm["hostTrackCircuit"] = query.value("host_track_circuit").toString();
    pm["routeLockingEnabled"] = query.value("route_locking_enabled").toBool();
    pm["autoNormalizeAfterRoute"] = query.value("auto_normalize_after_route").toBool();

    //   PAIRED ENTITY INFORMATION
    pm["pairedMachineName"] = query.value("paired_machine_name").toString();
    pm["pairedCurrentPosition"] = query.value("paired_current_position").toString();
    pm["pairedCurrentPositionName"] = query.value("paired_current_position_name").toString();
    pm["pairedOperatingStatus"] = query.value("paired_operating_status").toString();
    pm["pairedIsLocked"] = query.value("paired_is_locked").toBool();

    //   RESOURCE LOCK STATUS
    pm["isRouteLocked"] = query.value("is_route_locked").toBool();
    pm["lockedByRouteId"] = query.value("locked_by_route_id").toString();
    pm["routeLockType"] = query.value("route_lock_type").toString();
    pm["routeLockedAt"] = query.value("route_locked_at").toString();
    pm["routeLockedBy"] = query.value("route_locked_by").toString();
    pm["routeLockExpiresAt"] = query.value("route_lock_expires_at").toString();

    //   ROUTE ASSIGNMENT CONTEXT
    pm["routeSourceSignal"] = query.value("route_source_signal").toString();
    pm["routeDestSignal"] = query.value("route_dest_signal").toString();
    pm["routeState"] = query.value("route_state").toString();
    pm["routeDirection"] = query.value("route_direction").toString();

    //   STATUS FIELDS
    pm["pairedSyncStatus"] = query.value("paired_sync_status").toString();
    pm["availabilityStatus"] = query.value("availability_status").toString();
    pm["isActive"] = query.value("availability_status").toString() != "FAILED" &&
                     query.value("availability_status").toString() != "MAINTENANCE";

    //   PERFORMANCE METRICS
    QVariant avgTime = query.value("avg_time_between_operations_seconds");
    pm["avgTimeBetweenOperations"] = avgTime.isNull() ? QVariant() : avgTime.toDouble();

    //   TIMESTAMPS
    pm["createdAt"] = query.value("created_at").toString();
    pm["updatedAt"] = query.value("updated_at").toString();

    //   JUNCTION POINT
    QVariantMap junctionPoint;
    junctionPoint["row"] = query.value("junction_row").toDouble();
    junctionPoint["col"] = query.value("junction_col").toDouble();
    pm["junctionPoint"] = junctionPoint;

    //   TRACK SEGMENT CONNECTIONS (parse JSON)
    QString rootConnStr = query.value("root_track_segment_connection").toString();
    QString normalConnStr = query.value("normal_track_segment_connection").toString();
    QString reverseConnStr = query.value("reverse_track_segment_connection").toString();

    if (!rootConnStr.isEmpty()) {
        QJsonDocument rootDoc = QJsonDocument::fromJson(rootConnStr.toUtf8());
        pm["rootTrackSegment"] = rootDoc.object().toVariantMap();
    }

    if (!normalConnStr.isEmpty()) {
        QJsonDocument normalDoc = QJsonDocument::fromJson(normalConnStr.toUtf8());
        pm["normalTrackSegment"] = normalDoc.object().toVariantMap();
    }

    if (!reverseConnStr.isEmpty()) {
        QJsonDocument reverseDoc = QJsonDocument::fromJson(reverseConnStr.toUtf8());
        pm["reverseTrackSegment"] = reverseDoc.object().toVariantMap();
    }

    //   HANDLE POSTGRESQL ARRAYS
    // Convert safety_interlocks array (integers)
    QString interlocksStr = query.value("safety_interlocks").toString();
    if (!interlocksStr.isEmpty()) {
        interlocksStr = interlocksStr.mid(1, interlocksStr.length() - 2); // Remove { }
        QStringList interlockList = interlocksStr.split(",");
        QVariantList interlockVariants;
        for (const QString& item : interlockList) {
            bool ok;
            int intValue = item.trimmed().toInt(&ok);
            if (ok) {
                interlockVariants.append(intValue);
            }
        }
        pm["safetyInterlocks"] = interlockVariants;
    } else {
        pm["safetyInterlocks"] = QVariantList();
    }

    // Convert protected_signals array (text)
    QString signalsStr = query.value("protected_signals").toString();
    if (!signalsStr.isEmpty()) {
        signalsStr = signalsStr.mid(1, signalsStr.length() - 2); // Remove { }
        QStringList signalsList = signalsStr.split(",");
        QStringList cleanSignals;
        for (const QString& signal : signalsList) {
            cleanSignals.append(signal.trimmed());
        }
        pm["protectedSignals"] = cleanSignals;
    } else {
        pm["protectedSignals"] = QStringList();
    }

    return pm;
}

QVariantMap DatabaseManager::convertTrackCircuitRowToVariant(const QSqlQuery& query) {
    QVariantMap circuit;

    //   BASIC CIRCUIT INFO
    circuit["id"] = query.value("circuit_id").toString();
    circuit["databaseId"] = query.value("id").toInt(); // Primary key for internal use
    circuit["name"] = query.value("circuit_name").toString();
    circuit["isActive"] = query.value("is_active").toBool();

    //   OCCUPANCY STATUS
    circuit["occupied"] = query.value("is_occupied").toBool();
    circuit["occupiedBy"] = query.value("occupied_by").toString();
    circuit["lastChangedAt"] = query.value("last_changed_at").toString();

    //   NEW: ROUTE ASSIGNMENT AND LOCKING STATUS
    circuit["isAssigned"] = query.value("is_assigned").toBool();  //   NEW: Route assignment status
    circuit["isOverlap"] = query.value("is_overlap").toBool();    //   NEW: Overlap assignment status

    //   PHYSICAL PROPERTIES
    circuit["lengthMeters"] = query.value("length_meters").toDouble();
    circuit["maxSpeedKmh"] = query.value("max_speed_kmh").toInt();

    //   TIMESTAMPS
    circuit["createdAt"] = query.value("created_at").toString();
    circuit["updatedAt"] = query.value("updated_at").toString();

    //   HANDLE PROTECTING SIGNALS ARRAY
    QString protectingSignalsStr = query.value("protecting_signals").toString();
    if (!protectingSignalsStr.isEmpty()) {
        protectingSignalsStr = protectingSignalsStr.mid(1, protectingSignalsStr.length() - 2); // Remove { }
        QStringList signalsList = protectingSignalsStr.split(",");
        QStringList cleanSignals;
        for (const QString& signal : signalsList) {
            cleanSignals.append(signal.trimmed());
        }
        circuit["protectingSignals"] = cleanSignals;
    } else {
        circuit["protectingSignals"] = QStringList();
    }

    return circuit;
}

// Legacy methods for compatibility
QVariantMap DatabaseManager::getAllSignalStates() {
    QVariantMap states;
    QSqlQuery query("SELECT signal_id, current_aspect_id FROM railway_control.signals", db);
    while (query.next()) {
        states[query.value(0).toString()] = query.value(1).toString();
    }
    return states;
}

QString DatabaseManager::getSignalState(int signalId) {
    QSqlQuery query(db);
    query.prepare("SELECT current_aspect_id FROM railway_control.signals WHERE signal_id = ?");
    query.addBindValue(QString::number(signalId));
    if (query.exec() && query.next()) {
        return query.value(0).toString();
    }
    return "RED"; // Safe default
}

QVariantMap DatabaseManager::getAllTrackCircuitStates() {
    QVariantMap states;
    QSqlQuery query("SELECT circuit_id, is_occupied FROM railway_control.track_circuits", db);
    while (query.next()) {
        states[query.value(0).toString()] = query.value(1).toBool();
    }
    return states;
}

// === CIRCUIT LOOKUP IMPLEMENTATION ===
QString DatabaseManager::getCircuitIdByTrackSegmentId(const QString& trackSegmentId) {
    if (!connected) return QString();

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT circuit_id
        FROM railway_control.track_segments
        WHERE segment_id = ?
          AND is_active = TRUE
    )");
    query.addBindValue(trackSegmentId);

    if (query.exec() && query.next()) {
        return query.value(0).toString();
    } else if (!query.exec()) {
        qWarning() << " DatabaseManager: Failed to get circuit ID for track segment" << trackSegmentId << ":" << query.lastError().text();
    }

    return QString(); // Return empty if not found
}

QVariantMap DatabaseManager::getAllPointMachineStates() {
    QVariantMap states;

    QSqlQuery query(db);
    query.exec(R"(
        SELECT machine_id, current_position, availability_status, is_locked
        FROM railway_control.v_point_machines_complete
        WHERE operating_status != 'FAILED'
          AND operating_status != 'MAINTENANCE'
    )");

    while (query.next()) {
        QString machineId = query.value("machine_id").toString();
        QString positionCode = query.value("current_position").toString();
        QString availabilityStatus = query.value("availability_status").toString();
        bool isLocked = query.value("is_locked").toBool();

        // Store both current position and availability
        QVariantMap pmData;
        pmData["current_position"] = positionCode;
        pmData["availability_status"] = availabilityStatus;
        pmData["is_moveable"] = (availabilityStatus == "AVAILABLE" && !isLocked);

        states[machineId] = pmData;

        qDebug() << " [PM]" << machineId << "=" << positionCode
                 << "availability:" << availabilityStatus
                 << "moveable:" << pmData["is_moveable"].toBool();
    }

    qDebug() << " [PM] Total PM states loaded:" << states.size();
    return states;
}

QString DatabaseManager::getPointPosition(int machineId) {
    QSqlQuery query(db);
    query.prepare("SELECT current_position_id FROM railway_control.point_machines WHERE machine_id = ?");
    query.addBindValue(QString::number(machineId));
    if (query.exec() && query.next()) {
        return query.value(0).toString();
    }
    return "NORMAL"; // Safe default
}

void DatabaseManager::logError(const QString& operation, const QSqlError& error) {
    qWarning() << "Database error in" << operation << ":" << error.text();
}

bool DatabaseManager::insertRouteAssignment(
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
    ) {

    //   ENTRY LOGGING
    qDebug() << " [DB_INSERT] ==================== STARTING ROUTE INSERTION ====================";
    qDebug() << " [DB_INSERT] Route ID:" << routeId;
    qDebug() << " [DB_INSERT] Route:" << sourceSignalId << "→" << destSignalId;
    qDebug() << " [DB_INSERT] Direction:" << direction << "State:" << state << "Priority:" << priority;
    qDebug() << " [DB_INSERT] Assigned Circuits:" << assignedCircuits;
    qDebug() << " [DB_INSERT] Overlap Circuits:" << overlapCircuits;
    qDebug() << " [DB_INSERT] Locked Point Machines:" << lockedPointMachines;
    qDebug() << " [DB_INSERT] Operator:" << operatorId;
    qDebug() << " [DB_INSERT] Database Connected:" << connected;

    if (!connected) {
        qCritical() << " [DB_INSERT] Database not connected!";
        logError("insertRouteAssignment", QSqlError("Not connected to database", "", QSqlError::ConnectionError));
        return false;
    }

    QElapsedTimer timer;
    timer.start();

    //   PRE-INSERTION LOGGING
    qDebug() << " [DB_INSERT] Starting transaction...";

    if (!db.transaction()) {
        qCritical() << " [DB_INSERT] Failed to start transaction:" << db.lastError().text();
        qCritical() << " [DB_INSERT] Database error type:" << db.lastError().type();
        qCritical() << " [DB_INSERT] Database error number:" << db.lastError().nativeErrorCode();
        return false;
    }

    qDebug() << "  [DB_INSERT] Transaction started successfully";

    try {
        //   QUERY PREPARATION LOGGING
        qDebug() << " [DB_INSERT] Preparing SQL function call...";
        QSqlQuery query(db);
        query.prepare("SELECT railway_control.insert_route_assignment(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");

        // Convert parameters
        QString circuitsArray = "{" + assignedCircuits.join(",") + "}";
        QString overlapArray = "{" + overlapCircuits.join(",") + "}";
        QString lockedPMArray = "{" + lockedPointMachines.join(",") + "}";

        qDebug() << " [DB_INSERT] Converted arrays:";
        qDebug() << "   Circuits:" << circuitsArray;
        qDebug() << "   Overlap:" << overlapArray;
        qDebug() << "   Point Machines:" << lockedPMArray;

        // Bind parameters
        query.addBindValue(routeId);
        query.addBindValue(sourceSignalId);
        query.addBindValue(destSignalId);
        query.addBindValue(direction);
        query.addBindValue(circuitsArray);
        query.addBindValue(overlapArray);
        query.addBindValue(state);
        query.addBindValue(lockedPMArray);
        query.addBindValue(priority);
        query.addBindValue(operatorId.isEmpty() ? "system" : operatorId);

        qDebug() << "  [DB_INSERT] Query prepared and parameters bound";

        //   EXECUTION LOGGING
        qDebug() << " [DB_INSERT] Executing SQL function...";

        if (!query.exec()) {
            QString errorMsg = QString("Query execution failed: %1").arg(query.lastError().text());
            qCritical() << " [DB_INSERT]" << errorMsg;
            qCritical() << " [DB_INSERT] SQL State:" << query.lastError().nativeErrorCode();
            qCritical() << " [DB_INSERT] Driver Text:" << query.lastError().driverText();
            qCritical() << " [DB_INSERT] Database Text:" << query.lastError().databaseText();
            throw std::runtime_error(errorMsg.toStdString());
        }

        qDebug() << "  [DB_INSERT] Query executed successfully";

        if (!query.next()) {
            qCritical() << " [DB_INSERT] Query executed but no result returned";
            throw std::runtime_error("Query executed but no result returned");
        }

        bool success = query.value(0).toBool();
        qDebug() << " [DB_INSERT] Function returned:" << success;

        if (!success) {
            qCritical() << " [DB_INSERT] Database function returned false";
            throw std::runtime_error("Database function returned false");
        }

        //   COMMIT LOGGING
        qDebug() << " [DB_INSERT] Committing transaction...";

        if (!db.commit()) {
            QString errorMsg = QString("Commit failed: %1").arg(db.lastError().text());
            qCritical() << " [DB_INSERT]" << errorMsg;
            throw std::runtime_error(errorMsg.toStdString());
        }

        qDebug() << "  [DB_INSERT] Transaction committed successfully";

        //   VERIFICATION LOGGING
        qDebug() << " [DB_INSERT] Verifying route creation...";

        QSqlQuery verifyQuery(db);
        verifyQuery.prepare("SELECT id, source_signal_id, dest_signal_id, state, created_at FROM railway_control.route_assignments WHERE id = ?");
        verifyQuery.addBindValue(routeId);

        if (!verifyQuery.exec()) {
            qCritical() << " [DB_INSERT] Verification query failed:" << verifyQuery.lastError().text();
            return false;
        }

        if (!verifyQuery.next()) {
            qCritical() << " [DB_INSERT] CRITICAL: Route not found after commit!";
            qCritical() << " [DB_INSERT] This suggests a transaction rollback occurred";
            return false;
        }

        // Log the verified route details
        qDebug() << "  [DB_INSERT] Route verification successful:";
        qDebug() << "   ID:" << verifyQuery.value("id").toString();
        qDebug() << "   Source:" << verifyQuery.value("source_signal_id").toString();
        qDebug() << "   Dest:" << verifyQuery.value("dest_signal_id").toString();
        qDebug() << "   State:" << verifyQuery.value("state").toString();
        qDebug() << "   Created:" << verifyQuery.value("created_at").toDateTime();

        //   SUCCESS LOGGING
        qDebug() << "  [DB_INSERT] ==================== ROUTE INSERTION SUCCESS ====================";
        qDebug() << "  [DB_INSERT] Route" << routeId << "created successfully in" << timer.elapsed() << "ms";

        // Emit signals
        emit routeAssignmentInserted(routeId);
        emit routeAssignmentsChanged();

        return true;

    } catch (const std::exception& e) {
        //   ERROR LOGGING
        qCritical() << " [DB_INSERT] ==================== ROUTE INSERTION FAILED ====================";
        qCritical() << " [DB_INSERT] Exception:" << e.what();
        qCritical() << " [DB_INSERT] Rolling back transaction...";

        if (!db.rollback()) {
            qCritical() << " [DB_INSERT] CRITICAL: Rollback also failed:" << db.lastError().text();
        } else {
            qDebug() << "  [DB_INSERT] Transaction rolled back successfully";
        }
        return false;
    }
}

// 
// ROUTE ASSIGNMENT METHODS IMPLEMENTATION
// 

bool DatabaseManager::updateRouteState(const QString& routeId, const QString& newState, const QString& failureReason) {
    if (!connected) {
        logError("updateRouteState", QSqlError("Not connected to database", "", QSqlError::ConnectionError));
        return false;
    }

    QElapsedTimer timer;
    timer.start();

    qDebug() << " SAFETY: Updating route state:" << routeId << "to state:" << newState;

    // Validate route ID format
    if (routeId.isEmpty()) {
        qWarning() << " Invalid route ID provided for state update";
        emit operationBlocked(routeId, "Invalid route ID");
        return false;
    }

    // Get current route state for logging
    QVariantMap currentRoute = getRouteAssignment(routeId);
    if (currentRoute.isEmpty()) {
        qWarning() << " Route not found:" << routeId;
        emit operationBlocked(routeId, "Route not found");
        return false;
    }

    QString currentState = currentRoute["state"].toString();
    qDebug() << "Route state transition:" << currentState << "→" << newState;

    // Database transaction for route state update
    QSqlQuery query(db);

    if (!db.transaction()) {
        qWarning() << " Failed to start transaction for route state update:" << db.lastError().text();
        return false;
    }

    //   POLICY: Call SQL function instead of direct UPDATE
    query.prepare("SELECT railway_control.update_route_state(?, ?, ?, ?)");
    query.addBindValue(routeId);
    query.addBindValue(newState);
    query.addBindValue("HMI_USER"); // operator_id
    query.addBindValue(failureReason.isEmpty() ? QVariant(QVariant::String) : failureReason);

    bool success = false;
    if (query.exec() && query.next()) {
        success = query.value(0).toBool();
        if (success && db.commit()) {
            // Verify state change
            QSqlQuery verifyQuery(db);
            verifyQuery.prepare("SELECT state FROM railway_control.route_assignments WHERE id = ?");
            verifyQuery.addBindValue(routeId);
            if (verifyQuery.exec() && verifyQuery.next()) {
                QString verifiedState = verifyQuery.value(0).toString();
                qDebug() << "  SAFETY: Route" << routeId << "now has state:" << verifiedState;
            }

            // Emit success signals
            emit routeStateChanged(routeId, newState);
            emit routeAssignmentsChanged();

            qDebug() << "  Route state update completed in" << timer.elapsed() << "ms";
            return true;
        } else {
            qWarning() << " Route state update failed:" << query.lastError().text();
            db.rollback();
            return false;
        }
    } else {
        qWarning() << " Route state query execution failed:" << query.lastError().text();
        db.rollback();
        return false;
    }
}

bool DatabaseManager::updateRouteActivation(const QString& routeId) {
    if (!connected) {
        logError("updateRouteActivation", QSqlError("Not connected to database", "", QSqlError::ConnectionError));
        return false;
    }

    QElapsedTimer timer;
    timer.start();

    qDebug() << " SAFETY: Activating route:" << routeId;

    // Validate route ID format
    if (routeId.isEmpty()) {
        qWarning() << " Invalid route ID provided for activation";
        emit operationBlocked(routeId, "Invalid route ID");
        return false;
    }

    // Get current route state for logging and validation
    QVariantMap currentRoute = getRouteAssignment(routeId);
    if (currentRoute.isEmpty()) {
        qWarning() << " Route not found:" << routeId;
        emit operationBlocked(routeId, "Route not found");
        return false;
    }

    QString currentState = currentRoute["state"].toString();
    qDebug() << "Route activation:" << currentState << "→ ACTIVE";

    // Optional: Additional business logic validation before activation
    // (The SQL function will also validate, but you can add app-specific checks here)
    if (currentState != "RESERVED") {
        qDebug() << " Warning: Activating route from non-RESERVED state:" << currentState;
    }

    // Database transaction for route activation
    QSqlQuery query(db);

    if (!db.transaction()) {
        qWarning() << " Failed to start transaction for route activation:" << db.lastError().text();
        return false;
    }

    //   POLICY: Call SQL function instead of direct UPDATE
    query.prepare("SELECT railway_control.update_route_state(?, ?, ?)");
    query.addBindValue(routeId);
    query.addBindValue("ACTIVE");
    query.addBindValue("HMI_USER"); // operator_id
    // Note: failure_reason is not needed for activation, so we don't pass it

    bool success = false;
    if (query.exec() && query.next()) {
        success = query.value(0).toBool();
        if (success && db.commit()) {
            // Verify activation
            QSqlQuery verifyQuery(db);
            verifyQuery.prepare("SELECT state, activated_at FROM railway_control.route_assignments WHERE id = ?");
            verifyQuery.addBindValue(routeId);
            if (verifyQuery.exec() && verifyQuery.next()) {
                QString verifiedState = verifyQuery.value(0).toString();
                QString activatedAt = verifyQuery.value(1).toString();
                qDebug() << "  SAFETY: Route" << routeId << "activated. State:" << verifiedState << "Time:" << activatedAt;
            }

            // Emit success signals
            emit routeActivated(routeId);
            emit routeStateChanged(routeId, "ACTIVE");
            emit routeAssignmentsChanged();

            qDebug() << "  Route activation completed in" << timer.elapsed() << "ms";
            return true;
        } else {
            qWarning() << " Route activation failed:" << query.lastError().text();
            db.rollback();
            return false;
        }
    } else {
        qWarning() << " Route activation query execution failed:" << query.lastError().text();
        db.rollback();
        return false;
    }
}

bool DatabaseManager::updateRouteRelease(const QString& routeId) {
    if (!connected) {
        logError("updateRouteRelease", QSqlError("Not connected to database", "", QSqlError::ConnectionError));
        return false;
    }

    QElapsedTimer timer;
    timer.start();

    qDebug() << " SAFETY: Releasing route:" << routeId;

    // Validate route ID format
    if (routeId.isEmpty()) {
        qWarning() << " Invalid route ID provided for release";
        emit operationBlocked(routeId, "Invalid route ID");
        return false;
    }

    // Get current route state for logging and validation
    QVariantMap currentRoute = getRouteAssignment(routeId);
    if (currentRoute.isEmpty()) {
        qWarning() << " Route not found:" << routeId;
        emit operationBlocked(routeId, "Route not found");
        return false;
    }

    QString currentState = currentRoute["state"].toString();
    qDebug() << "Route release:" << currentState << "→ RELEASED";

    // Optional: Additional business logic validation before release
    // (The SQL function will also validate, but you can add app-specific checks here)
    if (currentState != "ACTIVE" && currentState != "PARTIALLY_RELEASED") {
        qDebug() << " Warning: Releasing route from non-standard state:" << currentState;
    }

    // Database transaction for route release
    QSqlQuery query(db);

    if (!db.transaction()) {
        qWarning() << " Failed to start transaction for route release:" << db.lastError().text();
        return false;
    }

    //   POLICY: Call SQL function instead of direct UPDATE
    query.prepare("SELECT railway_control.update_route_state(?, ?, ?)");
    query.addBindValue(routeId);
    query.addBindValue("RELEASED");
    query.addBindValue("HMI_USER"); // operator_id
    // Note: failure_reason is not needed for normal release, so we don't pass it

    bool success = false;
    if (query.exec() && query.next()) {
        success = query.value(0).toBool();
        if (success && db.commit()) {
            // Verify release
            QSqlQuery verifyQuery(db);
            verifyQuery.prepare("SELECT state, released_at FROM railway_control.route_assignments WHERE id = ?");
            verifyQuery.addBindValue(routeId);
            if (verifyQuery.exec() && verifyQuery.next()) {
                QString verifiedState = verifyQuery.value(0).toString();
                QString releasedAt = verifyQuery.value(1).toString();
                qDebug() << "  SAFETY: Route" << routeId << "released. State:" << verifiedState << "Time:" << releasedAt;
            }

            // Emit success signals
            emit routeReleased(routeId);
            emit routeStateChanged(routeId, "RELEASED");
            emit routeAssignmentsChanged();

            qDebug() << "  Route release completed in" << timer.elapsed() << "ms";
            return true;
        } else {
            qWarning() << " Route release failed:" << query.lastError().text();
            db.rollback();
            return false;
        }
    } else {
        qWarning() << " Route release query execution failed:" << query.lastError().text();
        db.rollback();
        return false;
    }
}

bool DatabaseManager::updateRouteFailure(const QString& routeId, const QString& failureReason) {
    if (!connected) {
        logError("updateRouteFailure", QSqlError("Not connected to database", "", QSqlError::ConnectionError));
        return false;
    }

    QElapsedTimer timer;
    timer.start();

    qDebug() << " SAFETY: Marking route as FAILED:" << routeId << "Reason:" << failureReason;

    // Validate route ID format
    if (routeId.isEmpty()) {
        qWarning() << " Invalid route ID provided for failure update";
        emit operationBlocked(routeId, "Invalid route ID");
        return false;
    }

    // Validate failure reason is provided
    if (failureReason.isEmpty()) {
        qWarning() << " Failure reason must be provided for route failure";
        emit operationBlocked(routeId, "Failure reason required");
        return false;
    }

    // Get current route state for logging and validation
    QVariantMap currentRoute = getRouteAssignment(routeId);
    if (currentRoute.isEmpty()) {
        qWarning() << " Route not found:" << routeId;
        emit operationBlocked(routeId, "Route not found");
        return false;
    }

    QString currentState = currentRoute["state"].toString();
    qDebug() << "Route failure:" << currentState << "→ FAILED (" << failureReason << ")";

    // Optional: Log warning for certain state transitions
    if (currentState == "ACTIVE") {
        qWarning() << " CRITICAL: Active route being marked as failed - this may affect traffic!";
    }

    // Database transaction for route failure update
    QSqlQuery query(db);

    if (!db.transaction()) {
        qWarning() << " Failed to start transaction for route failure:" << db.lastError().text();
        return false;
    }

    //   POLICY: Call SQL function instead of direct UPDATE
    query.prepare("SELECT railway_control.update_route_state(?, ?, ?, ?)");
    query.addBindValue(routeId);
    query.addBindValue("FAILED");
    query.addBindValue("HMI_USER"); // operator_id
    query.addBindValue(failureReason); // failure_reason_param

    bool success = false;
    if (query.exec() && query.next()) {
        success = query.value(0).toBool();
        if (success && db.commit()) {
            // Verify failure state and reason
            QSqlQuery verifyQuery(db);
            verifyQuery.prepare("SELECT state, failure_reason, updated_at FROM railway_control.route_assignments WHERE id = ?");
            verifyQuery.addBindValue(routeId);
            if (verifyQuery.exec() && verifyQuery.next()) {
                QString verifiedState = verifyQuery.value(0).toString();
                QString verifiedReason = verifyQuery.value(1).toString();
                QString updatedAt = verifyQuery.value(2).toString();
                qDebug() << "  SAFETY: Route" << routeId << "marked as failed.";
                qDebug() << "   State:" << verifiedState << "Reason:" << verifiedReason << "Time:" << updatedAt;
            }

            // Emit success signals
            emit routeFailed(routeId, failureReason);
            emit routeStateChanged(routeId, "FAILED");
            emit routeAssignmentsChanged();

            qDebug() << "  Route failure update completed in" << timer.elapsed() << "ms";
            return true;
        } else {
            qWarning() << " Route failure update failed:" << query.lastError().text();
            db.rollback();
            return false;
        }
    } else {
        qWarning() << " Route failure query execution failed:" << query.lastError().text();
        db.rollback();
        return false;
    }
}

bool DatabaseManager::updateRoutePerformanceMetrics(const QString& routeId, const QVariantMap& metrics) {
    if (!connected) {
        logError("updateRoutePerformanceMetrics", QSqlError("Not connected to database", "", QSqlError::ConnectionError));
        return false;
    }

    QElapsedTimer timer;
    timer.start();

    qDebug() << " Updating performance metrics for route:" << routeId;

    // Validate route ID format
    if (routeId.isEmpty()) {
        qWarning() << " Invalid route ID provided for performance metrics update";
        return false;
    }

    // Validate metrics are provided
    if (metrics.isEmpty()) {
        qWarning() << " No performance metrics provided for update";
        return false;
    }

    // Convert metrics to JSON
    QJsonDocument jsonDoc = QJsonDocument::fromVariant(metrics);
    QString jsonString = jsonDoc.toJson(QJsonDocument::Compact);

    qDebug() << " Performance metrics:" << jsonString;

    // Database transaction for performance metrics update
    QSqlQuery query(db);

    if (!db.transaction()) {
        qWarning() << " Failed to start transaction for performance metrics:" << db.lastError().text();
        return false;
    }

    //   POLICY: Call SQL function instead of direct UPDATE
    query.prepare("SELECT railway_control.update_route_performance_metrics(?, ?::jsonb, ?)");
    query.addBindValue(routeId);
    query.addBindValue(jsonString);
    query.addBindValue("HMI_USER"); // operator_id

    bool success = false;
    if (query.exec() && query.next()) {
        success = query.value(0).toBool();
        if (success && db.commit()) {
            // Optional: Verify metrics were updated
            QSqlQuery verifyQuery(db);
            verifyQuery.prepare("SELECT performance_metrics FROM railway_control.route_assignments WHERE id = ?");
            verifyQuery.addBindValue(routeId);
            if (verifyQuery.exec() && verifyQuery.next()) {
                QString storedMetrics = verifyQuery.value(0).toString();
                qDebug() << "  Performance metrics updated for route" << routeId;
                qDebug() << " Stored metrics:" << storedMetrics.left(100) << "..."; // Truncate for logging
            }

            qDebug() << "  Performance metrics update completed in" << timer.elapsed() << "ms";
            return true;
        } else {
            qWarning() << " Performance metrics update failed:" << query.lastError().text();
            db.rollback();
            return false;
        }
    } else {
        qWarning() << " Performance metrics query execution failed:" << query.lastError().text();
        db.rollback();
        return false;
    }
}

QVariantMap DatabaseManager::getRouteAssignment(const QString& routeId) {
    QVariantMap route;
    if (!connected) return route;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT id, source_signal_id, dest_signal_id, direction,
               assigned_circuits, overlap_circuits, state,
               created_at, activated_at, released_at,
               locked_point_machines, priority, operator_id,
               failure_reason, performance_metrics
        FROM railway_control.route_assignments
        WHERE id = ?
    )");
    query.addBindValue(routeId);

    if (query.exec() && query.next()) {
        route["id"] = query.value("id").toString();
        route["sourceSignalId"] = query.value("source_signal_id").toString();
        route["destSignalId"] = query.value("dest_signal_id").toString();
        route["direction"] = query.value("direction").toString();
        route["assignedCircuits"] = query.value("assigned_circuits").toString();
        route["overlapCircuits"] = query.value("overlap_circuits").toString();
        route["state"] = query.value("state").toString();
        route["createdAt"] = query.value("created_at").toDateTime();
        route["activatedAt"] = query.value("activated_at").toDateTime();
        route["releasedAt"] = query.value("released_at").toDateTime();
        route["lockedPointMachines"] = query.value("locked_point_machines").toString();
        route["priority"] = query.value("priority").toInt();
        route["operatorId"] = query.value("operator_id").toString();
        route["failureReason"] = query.value("failure_reason").toString();
        route["performanceMetrics"] = query.value("performance_metrics").toString();
    } else if (!query.exec()) {
        logError("getRouteAssignment", query.lastError());
    }

    return route;
}

QVariantList DatabaseManager::getActiveRoutes() {
    return getRoutesByState("ACTIVE");
}

QVariantList DatabaseManager::getRoutesByState(const QString& state) {
    QVariantList routes;
    if (!connected) return routes;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT id, source_signal_id, dest_signal_id, direction,
               assigned_circuits, overlap_circuits, state,
               created_at, activated_at, released_at,
               locked_point_machines, priority, operator_id
        FROM railway_control.route_assignments
        WHERE state = ?
        ORDER BY created_at DESC
    )");
    query.addBindValue(state);

    if (query.exec()) {
        while (query.next()) {
            QVariantMap route;
            route["id"] = query.value("id").toString();
            route["sourceSignalId"] = query.value("source_signal_id").toString();
            route["destSignalId"] = query.value("dest_signal_id").toString();
            route["direction"] = query.value("direction").toString();
            route["assignedCircuits"] = query.value("assigned_circuits").toString();
            route["overlapCircuits"] = query.value("overlap_circuits").toString();
            route["state"] = query.value("state").toString();
            route["createdAt"] = query.value("created_at").toDateTime();
            route["activatedAt"] = query.value("activated_at").toDateTime();
            route["releasedAt"] = query.value("released_at").toDateTime();
            route["lockedPointMachines"] = query.value("locked_point_machines").toString();
            route["priority"] = query.value("priority").toInt();
            route["operatorId"] = query.value("operator_id").toString();
            routes.append(route);
        }
    } else {
        logError("getRoutesByState", query.lastError());
    }

    return routes;
}

QVariantList DatabaseManager::getRoutesBySignal(const QString& signalId) {
    QVariantList routes;
    if (!connected) return routes;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT id, source_signal_id, dest_signal_id, direction,
               assigned_circuits, overlap_circuits, state,
               created_at, activated_at, released_at,
               locked_point_machines, priority, operator_id
        FROM railway_control.route_assignments
        WHERE source_signal_id = ? OR dest_signal_id = ?
        ORDER BY created_at DESC
    )");
    query.addBindValue(signalId);
    query.addBindValue(signalId);

    if (query.exec()) {
        while (query.next()) {
            QVariantMap route;
            route["id"] = query.value("id").toString();
            route["sourceSignalId"] = query.value("source_signal_id").toString();
            route["destSignalId"] = query.value("dest_signal_id").toString();
            route["direction"] = query.value("direction").toString();
            route["assignedCircuits"] = query.value("assigned_circuits").toString();
            route["overlapCircuits"] = query.value("overlap_circuits").toString();
            route["state"] = query.value("state").toString();
            route["createdAt"] = query.value("created_at").toDateTime();
            route["activatedAt"] = query.value("activated_at").toDateTime();
            route["releasedAt"] = query.value("released_at").toDateTime();
            route["lockedPointMachines"] = query.value("locked_point_machines").toString();
            route["priority"] = query.value("priority").toInt();
            route["operatorId"] = query.value("operator_id").toString();
            routes.append(route);
        }
    } else {
        logError("getRoutesBySignal", query.lastError());
    }

    return routes;
}

bool DatabaseManager::deleteRouteAssignment(const QString& routeId, bool forceDelete) {
    if (!connected) {
        logError("deleteRouteAssignment", QSqlError("Not connected to database", "", QSqlError::ConnectionError));
        return false;
    }

    QElapsedTimer timer;
    timer.start();

    qDebug() << " SAFETY: Deleting route assignment:" << routeId << "Force:" << forceDelete;

    // Validate route ID format
    if (routeId.isEmpty()) {
        qWarning() << " Invalid route ID provided for deletion";
        emit operationBlocked(routeId, "Invalid route ID");
        return false;
    }

    // Get current route state for safety validation and logging
    QVariantMap currentRoute = getRouteAssignment(routeId);
    if (currentRoute.isEmpty()) {
        qWarning() << " Route not found for deletion:" << routeId;
        emit operationBlocked(routeId, "Route not found");
        return false;
    }

    QString currentState = currentRoute["state"].toString();
    QString sourceSignal = currentRoute["sourceSignalId"].toString();
    QString destSignal = currentRoute["destSignalId"].toString();

    qDebug() << " Route deletion: State:" << currentState << "Route:" << sourceSignal << "→" << destSignal;

    // Safety warning for active route deletion
    if ((currentState == "ACTIVE" || currentState == "RESERVED") && !forceDelete) {
        qWarning() << " SAFETY: Cannot delete active/reserved route without force flag";
        emit operationBlocked(routeId, "Cannot delete active route - use force delete if necessary");
        return false;
    }

    if (forceDelete && (currentState == "ACTIVE" || currentState == "RESERVED")) {
        qCritical() << " CRITICAL: Force deleting active route - this may affect traffic safety!";
    }

    // Database transaction for route deletion
    QSqlQuery query(db);

    if (!db.transaction()) {
        qWarning() << " Failed to start transaction for route deletion:" << db.lastError().text();
        return false;
    }

    //   POLICY: Call SQL function instead of direct DELETE
    query.prepare("SELECT railway_control.delete_route_assignment(?, ?, ?)");
    query.addBindValue(routeId);
    query.addBindValue("HMI_USER"); // operator_id
    query.addBindValue(forceDelete); // force_delete flag

    bool success = false;
    if (query.exec() && query.next()) {
        success = query.value(0).toBool();
        if (success && db.commit()) {
            // Verify deletion
            QSqlQuery verifyQuery(db);
            verifyQuery.prepare("SELECT COUNT(*) FROM railway_control.route_assignments WHERE id = ?");
            verifyQuery.addBindValue(routeId);
            if (verifyQuery.exec() && verifyQuery.next()) {
                int remainingCount = verifyQuery.value(0).toInt();
                if (remainingCount == 0) {
                    qDebug() << "  SAFETY: Route" << routeId << "successfully deleted";
                } else {
                    qWarning() << " Unexpected: Route still exists after deletion";
                }
            }

            // Emit success signals
            emit routeDeleted(routeId); // You may need to add this signal to header
            emit routeAssignmentsChanged();

            qDebug() << "  Route deletion completed in" << timer.elapsed() << "ms";
            return true;
        } else {
            qWarning() << " Route deletion failed:" << query.lastError().text();
            db.rollback();
            return false;
        }
    } else {
        qWarning() << " Route deletion query execution failed:" << query.lastError().text();
        db.rollback();
        return false;
    }
}

bool DatabaseManager::insertRouteEvent(
    const QString& routeId,
    const QString& eventType,
    const QVariantMap& eventData,
    const QString& operatorId,
    const QString& sourceComponent,
    const QString& correlationId,
    double responseTimeMs,
    bool safetyCritical
    ) {
    if (!connected) {
        logError("insertRouteEvent", QSqlError("Not connected to database", "", QSqlError::ConnectionError));
        return false;
    }

    QElapsedTimer timer;
    timer.start();

    // Log based on criticality
    if (safetyCritical) {
        qDebug() << " SAFETY-CRITICAL EVENT:" << eventType << "for route:" << routeId;
    } else {
        qDebug() << "Logging route event:" << eventType << "for route:" << routeId;
    }

    // Validate required parameters
    if (routeId.isEmpty()) {
        qWarning() << " Invalid route ID provided for event logging";
        return false;
    }

    if (eventType.isEmpty()) {
        qWarning() << " Event type cannot be empty";
        return false;
    }

    // Convert event data to JSON
    QJsonDocument jsonDoc = QJsonDocument::fromVariant(eventData);
    QString jsonString = jsonDoc.toJson(QJsonDocument::Compact);

    // Log event details for debugging
    qDebug() << "Event details:";
    qDebug() << "   Type:" << eventType;
    qDebug() << "   Operator:" << (operatorId.isEmpty() ? "system" : operatorId);
    qDebug() << "   Source:" << (sourceComponent.isEmpty() ? "DatabaseManager" : sourceComponent);
    qDebug() << "   Critical:" << safetyCritical;
    qDebug() << "   Response Time:" << responseTimeMs << "ms";
    qDebug() << "   Data:" << jsonString.left(200) << (jsonString.length() > 200 ? "..." : "");

    // Database transaction for route event insertion
    QSqlQuery query(db);

    if (!db.transaction()) {
        qWarning() << " Failed to start transaction for route event:" << db.lastError().text();
        return false;
    }

    //   POLICY: Call SQL function instead of direct INSERT
    query.prepare("SELECT railway_control.insert_route_event(?, ?, ?::jsonb, ?, ?, ?, ?, ?)");
    query.addBindValue(routeId);
    query.addBindValue(eventType);
    query.addBindValue(jsonString.isEmpty() ? "{}" : jsonString);
    query.addBindValue(operatorId.isEmpty() ? QVariant(QVariant::String) : operatorId);
    query.addBindValue(sourceComponent.isEmpty() ? QVariant(QVariant::String) : sourceComponent);
    query.addBindValue(correlationId.isEmpty() ? QVariant(QVariant::String) : correlationId);
    query.addBindValue(responseTimeMs > 0.0 ? responseTimeMs : QVariant(QVariant::Double));
    query.addBindValue(safetyCritical);

    bool success = false;
    if (query.exec() && query.next()) {
        success = query.value(0).toBool();
        if (success && db.commit()) {
            // Verify event was logged (optional for performance)
            if (safetyCritical) {
                QSqlQuery verifyQuery(db);
                verifyQuery.prepare(R"(
                    SELECT event_timestamp, sequence_number
                    FROM railway_control.route_events
                    WHERE route_id = ? AND event_type = ?
                    ORDER BY event_timestamp DESC LIMIT 1
                )");
                verifyQuery.addBindValue(routeId);
                verifyQuery.addBindValue(eventType);
                if (verifyQuery.exec() && verifyQuery.next()) {
                    QString timestamp = verifyQuery.value(0).toString();
                    qint64 sequenceNum = verifyQuery.value(1).toLongLong();
                    qDebug() << "  SAFETY: Critical event logged at" << timestamp << "sequence:" << sequenceNum;
                }
            }

            // Emit success signal
            emit routeEventLogged(routeId, eventType);

            if (safetyCritical) {
                qDebug() << "  Safety-critical route event logged in" << timer.elapsed() << "ms";
            } else {
                qDebug() << "  Route event logged in" << timer.elapsed() << "ms";
            }
            return true;
        } else {
            qWarning() << " Route event insertion failed:" << query.lastError().text();
            db.rollback();
            return false;
        }
    } else {
        qWarning() << " Route event query execution failed:" << query.lastError().text();
        db.rollback();
        return false;
    }
}

QVariantList DatabaseManager::getRouteEvents(const QString& routeId, int limitHours) {
    QVariantList events;
    if (!connected) return events;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT id, route_id, event_type, event_timestamp, event_data,
               operator_id, source_component, correlation_id,
               response_time_ms, safety_critical
        FROM railway_control.route_events
        WHERE route_id = ?
          AND event_timestamp >= CURRENT_TIMESTAMP - INTERVAL '%1 hours'
        ORDER BY event_timestamp DESC
    )");
    query.addBindValue(routeId);

    // Replace %1 with limitHours in the query string
    QString queryString = query.lastQuery();
    queryString = queryString.arg(limitHours);
    query.prepare(queryString);
    query.addBindValue(routeId);

    if (query.exec()) {
        while (query.next()) {
            QVariantMap event;
            event["id"] = query.value("id").toLongLong();
            event["routeId"] = query.value("route_id").toString();
            event["eventType"] = query.value("event_type").toString();
            event["eventTimestamp"] = query.value("event_timestamp").toDateTime();
            event["eventData"] = query.value("event_data").toString();
            event["operatorId"] = query.value("operator_id").toString();
            event["sourceComponent"] = query.value("source_component").toString();
            event["correlationId"] = query.value("correlation_id").toString();
            event["responseTimeMs"] = query.value("response_time_ms").toDouble();
            event["safetyCritical"] = query.value("safety_critical").toBool();
            events.append(event);
        }
    } else {
        logError("getRouteEvents", query.lastError());
    }

    return events;
}

bool DatabaseManager::insertResourceLock(
    const QString& resourceType,
    const QString& resourceId,
    const QString& routeId,
    const QString& lockType
    ) {
    if (!connected) {
        logError("insertResourceLock", QSqlError("Not connected to database", "", QSqlError::ConnectionError));
        return false;
    }

    QElapsedTimer timer;
    timer.start();

    qDebug() << "SAFETY: Acquiring resource lock";
    qDebug() << "   Resource:" << resourceType << resourceId;
    qDebug() << "   Route:" << routeId;
    qDebug() << "   Lock Type:" << lockType;

    // Validate required parameters
    if (resourceType.isEmpty() || resourceId.isEmpty() || routeId.isEmpty() || lockType.isEmpty()) {
        qWarning() << " Missing required parameters for resource lock";
        emit operationBlocked(resourceId, "Missing required lock parameters");
        return false;
    }

    // Validate resource type
    QStringList validResourceTypes = {"TRACK_CIRCUIT", "POINT_MACHINE", "SIGNAL"};
    if (!validResourceTypes.contains(resourceType)) {
        qWarning() << " Invalid resource type:" << resourceType;
        emit operationBlocked(resourceId, "Invalid resource type");
        return false;
    }

    // Validate lock type
    QStringList validLockTypes = {"ROUTE", "OVERLAP", "EMERGENCY", "MAINTENANCE"};
    if (!validLockTypes.contains(lockType)) {
        qWarning() << " Invalid lock type:" << lockType;
        emit operationBlocked(resourceId, "Invalid lock type");
        return false;
    }

    // Check if route exists before attempting lock
    QVariantMap route = getRouteAssignment(routeId);
    if (route.isEmpty()) {
        qWarning() << " Route not found for resource lock:" << routeId;
        emit operationBlocked(resourceId, "Route not found");
        return false;
    }

    QString routeState = route["state"].toString();
    qDebug() << "Locking resource for route in state:" << routeState;

    // Database transaction for resource lock acquisition
    QSqlQuery query(db);

    if (!db.transaction()) {
        qWarning() << " Failed to start transaction for resource lock:" << db.lastError().text();
        return false;
    }

    //   POLICY: Call SQL function instead of direct INSERT
    query.prepare("SELECT railway_control.acquire_resource_lock(?, ?, ?, ?, ?)");
    query.addBindValue(resourceType);
    query.addBindValue(resourceId);
    query.addBindValue(routeId);
    query.addBindValue(lockType);
    query.addBindValue("HMI_USER"); // operator_id
    // Note: expires_at is NULL for normal locks (no expiration)

    bool success = false;
    if (query.exec() && query.next()) {
        success = query.value(0).toBool();
        if (success && db.commit()) {
            // Verify lock was acquired
            QSqlQuery verifyQuery(db);
            verifyQuery.prepare(R"(
                SELECT id, acquired_at, lock_type
                FROM railway_control.resource_locks
                WHERE resource_type = ? AND resource_id = ? AND route_id = ? AND is_active = TRUE
                ORDER BY acquired_at DESC LIMIT 1
            )");
            verifyQuery.addBindValue(resourceType);
            verifyQuery.addBindValue(resourceId);
            verifyQuery.addBindValue(routeId);

            if (verifyQuery.exec() && verifyQuery.next()) {
                QString lockId = verifyQuery.value(0).toString();
                QString acquiredAt = verifyQuery.value(1).toString();
                QString verifiedLockType = verifyQuery.value(2).toString();
                qDebug() << "  SAFETY: Resource lock acquired";
                qDebug() << "   Lock ID:" << lockId;
                qDebug() << "   Acquired at:" << acquiredAt;
                qDebug() << "   Lock type:" << verifiedLockType;
            }

            // Emit success signal
            emit resourceLockAcquired(routeId, resourceType, resourceId);

            qDebug() << "  Resource lock acquisition completed in" << timer.elapsed() << "ms";
            return true;
        } else {
            qWarning() << " Resource lock acquisition failed:" << query.lastError().text();
            db.rollback();
            return false;
        }
    } else {
        qWarning() << " Resource lock query execution failed:" << query.lastError().text();
        QString errorDetail = query.lastError().text();

        // Enhanced error reporting for common conflicts
        if (errorDetail.contains("already locked")) {
            qWarning() << "Resource conflict: Resource is already locked by another route";
            emit operationBlocked(resourceId, "Resource already locked");
        } else if (errorDetail.contains("not found")) {
            qWarning() << " Resource not found or inactive";
            emit operationBlocked(resourceId, "Resource not found");
        }

        db.rollback();
        return false;
    }
}

bool DatabaseManager::releaseResourceLocks(const QString& routeId) {
    if (!connected) {
        logError("releaseResourceLocks", QSqlError("Not connected to database", "", QSqlError::ConnectionError));
        return false;
    }

    QElapsedTimer timer;
    timer.start();

    qDebug() << " SAFETY: Releasing resource locks for route:" << routeId;

    // Validate route ID format
    if (routeId.isEmpty()) {
        qWarning() << " Invalid route ID provided for lock release";
        emit operationBlocked(routeId, "Invalid route ID");
        return false;
    }

    // Get current route state for validation and logging
    QVariantMap currentRoute = getRouteAssignment(routeId);
    if (currentRoute.isEmpty()) {
        qWarning() << " Route not found for lock release:" << routeId;
        emit operationBlocked(routeId, "Route not found");
        return false;
    }

    QString routeState = currentRoute["state"].toString();
    QString sourceSignal = currentRoute["sourceSignalId"].toString();
    QString destSignal = currentRoute["destSignalId"].toString();

    qDebug() << " Releasing locks for route:" << sourceSignal << "→" << destSignal << "State:" << routeState;

    // Get current locks for logging before release
    QVariantList currentLocks = getResourceLocks(routeId);
    int expectedLockCount = currentLocks.size();

    qDebug() << " Found" << expectedLockCount << "active locks to release";
    for (const auto& lockVar : currentLocks) {
        QVariantMap lock = lockVar.toMap();
        qDebug() << "   Lock:" << lock["resourceType"].toString() << lock["resourceId"].toString()
                 << "(" << lock["lockType"].toString() << ")";
    }

    // Database transaction for resource lock release
    QSqlQuery query(db);

    if (!db.transaction()) {
        qWarning() << " Failed to start transaction for lock release:" << db.lastError().text();
        return false;
    }

    //   POLICY: Call SQL function instead of direct DELETE/UPDATE
    query.prepare("SELECT railway_control.release_resource_locks(?, ?, ?)");
    query.addBindValue(routeId);
    query.addBindValue("HMI_USER"); // operator_id
    query.addBindValue("ROUTE_COMPLETION"); // release_reason

    bool success = false;
    if (query.exec() && query.next()) {
        int locksReleased = query.value(0).toInt();

        if (db.commit()) {
            // Verify locks were released
            QSqlQuery verifyQuery(db);
            verifyQuery.prepare(R"(
                SELECT COUNT(*) as active_locks,
                       COUNT(*) FILTER (WHERE is_active = FALSE) as released_locks
                FROM railway_control.resource_locks
                WHERE route_id = ?
            )");
            verifyQuery.addBindValue(routeId);

            if (verifyQuery.exec() && verifyQuery.next()) {
                int activeLocks = verifyQuery.value(0).toInt();
                int releasedLocks = verifyQuery.value(1).toInt();
                qDebug() << "  SAFETY: Lock release verification:";
                qDebug() << "   Locks released:" << locksReleased;
                qDebug() << "   Active locks remaining:" << activeLocks;
                qDebug() << "   Total released locks:" << releasedLocks;

                success = (locksReleased > 0 || expectedLockCount == 0);
            } else {
                // If verification fails, still consider successful if function returned > 0
                success = (locksReleased >= 0);
            }

            if (success) {
                // Emit success signal
                emit resourceLockReleased(routeId);

                if (locksReleased > 0) {
                    qDebug() << "  Successfully released" << locksReleased << "resource locks in" << timer.elapsed() << "ms";
                } else {
                    qDebug() << "  No active locks found to release for route" << routeId;
                }
                return true;
            } else {
                qWarning() << " Lock release verification failed";
                return false;
            }
        } else {
            qWarning() << " Resource lock release commit failed:" << db.lastError().text();
            db.rollback();
            return false;
        }
    } else {
        qWarning() << " Resource lock release query execution failed:" << query.lastError().text();
        QString errorDetail = query.lastError().text();

        // Enhanced error reporting
        if (errorDetail.contains("not found")) {
            qWarning() << " Route not found for lock release";
            emit operationBlocked(routeId, "Route not found");
        }

        db.rollback();
        return false;
    }
}

QVariantList DatabaseManager::getResourceLocks(const QString& routeId) {
    QVariantList locks;
    if (!connected) return locks;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT id, resource_type, resource_id, route_id, lock_type, acquired_at
        FROM railway_control.resource_locks
        WHERE route_id = ?
        ORDER BY acquired_at DESC
    )");
    query.addBindValue(routeId);

    if (query.exec()) {
        while (query.next()) {
            QVariantMap lock;
            lock["id"] = query.value("id").toInt();
            lock["resourceType"] = query.value("resource_type").toString();
            lock["resourceId"] = query.value("resource_id").toString();
            lock["routeId"] = query.value("route_id").toString();
            lock["lockType"] = query.value("lock_type").toString();
            lock["acquiredAt"] = query.value("acquired_at").toDateTime();
            locks.append(lock);
        }
    } else {
        logError("getResourceLocks", query.lastError());
    }

    return locks;
}

QVariantList DatabaseManager::getConflictingLocks(const QString& resourceId, const QString& resourceType) {
    QVariantList locks;
    if (!connected) return locks;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT id, resource_type, resource_id, route_id, lock_type, acquired_at
        FROM railway_control.resource_locks
        WHERE resource_id = ? AND resource_type = ?
        ORDER BY acquired_at DESC
    )");
    query.addBindValue(resourceId);
    query.addBindValue(resourceType);

    if (query.exec()) {
        while (query.next()) {
            QVariantMap lock;
            lock["id"] = query.value("id").toInt();
            lock["resourceType"] = query.value("resource_type").toString();
            lock["resourceId"] = query.value("resource_id").toString();
            lock["routeId"] = query.value("route_id").toString();
            lock["lockType"] = query.value("lock_type").toString();
            lock["acquiredAt"] = query.value("acquired_at").toDateTime();
            locks.append(lock);
        }
    } else {
        logError("getConflictingLocks", query.lastError());
    }

    return locks;
}

QVariantList DatabaseManager::getTrackCircuitEdges() {
    QVariantList edges;
    if (!connected) return edges;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT id, from_circuit_id, to_circuit_id, side,
               condition_point_machine_id, condition_position,
               weight, is_active
        FROM railway_control.track_circuit_edges
        WHERE is_active = TRUE
        ORDER BY from_circuit_id, to_circuit_id
    )");

    if (query.exec()) {
        while (query.next()) {
            QVariantMap edge;
            edge["id"] = query.value("id").toInt();
            edge["fromCircuitId"] = query.value("from_circuit_id").toString();
            edge["toCircuitId"] = query.value("to_circuit_id").toString();
            edge["side"] = query.value("side").toString();
            edge["conditionPointMachineId"] = query.value("condition_point_machine_id").toString();
            edge["conditionPosition"] = query.value("condition_position").toString();
            edge["weight"] = query.value("weight").toDouble();
            edge["isActive"] = query.value("is_active").toBool();
            edges.append(edge);
        }
    } else {
        logError("getTrackCircuitEdges", query.lastError());
    }

    return edges;
}

QVariantList DatabaseManager::getOutgoingEdges(const QString& circuitId) {
    QVariantList edges;
    if (!connected) return edges;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT id, from_circuit_id, to_circuit_id, side,
               condition_point_machine_id, condition_position,
               weight, is_active
        FROM railway_control.track_circuit_edges
        WHERE from_circuit_id = ? AND is_active = TRUE
        ORDER BY weight, to_circuit_id
    )");
    query.addBindValue(circuitId);

    if (query.exec()) {
        while (query.next()) {
            QVariantMap edge;
            edge["id"] = query.value("id").toInt();
            edge["fromCircuitId"] = query.value("from_circuit_id").toString();
            edge["toCircuitId"] = query.value("to_circuit_id").toString();
            edge["side"] = query.value("side").toString();
            edge["conditionPointMachineId"] = query.value("condition_point_machine_id").toString();
            edge["conditionPosition"] = query.value("condition_position").toString();
            edge["weight"] = query.value("weight").toDouble();
            edge["isActive"] = query.value("is_active").toBool();
            edges.append(edge);
        }
    } else {
        logError("getOutgoingEdges", query.lastError());
    }

    return edges;
}

QVariantList DatabaseManager::getIncomingEdges(const QString& circuitId) {
    QVariantList edges;
    if (!connected) return edges;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT id, from_circuit_id, to_circuit_id, side,
               condition_point_machine_id, condition_position,
               weight, is_active
        FROM railway_control.track_circuit_edges
        WHERE to_circuit_id = ? AND is_active = TRUE
        ORDER BY weight, from_circuit_id
    )");
    query.addBindValue(circuitId);

    if (query.exec()) {
        while (query.next()) {
            QVariantMap edge;
            edge["id"] = query.value("id").toInt();
            edge["fromCircuitId"] = query.value("from_circuit_id").toString();
            edge["toCircuitId"] = query.value("to_circuit_id").toString();
            edge["side"] = query.value("side").toString();
            edge["conditionPointMachineId"] = query.value("condition_point_machine_id").toString();
            edge["conditionPosition"] = query.value("condition_position").toString();
            edge["weight"] = query.value("weight").toDouble();
            edge["isActive"] = query.value("is_active").toBool();
            edges.append(edge);
        }
    } else {
        logError("getIncomingEdges", query.lastError());
    }

    return edges;
}

QVariantMap DatabaseManager::getSignalOverlapDefinition(const QString& signalId) {
    QVariantMap overlap;
    if (!connected) return overlap;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT signal_id, overlap_circuit_ids, release_trigger_circuit_ids,
               overlap_distance_meters, timed_release_seconds
        FROM railway_control.signal_overlap_definitions
        WHERE signal_id = ?
    )");
    query.addBindValue(signalId);

    if (query.exec() && query.next()) {
        overlap["signalId"] = query.value("signal_id").toString();
        overlap["overlapCircuitIds"] = query.value("overlap_circuit_ids").toString();
        overlap["releaseTriggerCircuitIds"] = query.value("release_trigger_circuit_ids").toString();
        overlap["overlapDistanceMeters"] = query.value("overlap_distance_meters").toDouble();
        overlap["timedReleaseSeconds"] = query.value("timed_release_seconds").toInt();
    } else if (!query.exec()) {
        logError("getSignalOverlapDefinition", query.lastError());
    }

    return overlap;
}

QVariantList DatabaseManager::getAllSignalOverlapDefinitions() {
    QVariantList overlaps;
    if (!connected) return overlaps;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT signal_id, overlap_circuit_ids, release_trigger_circuit_ids,
               overlap_distance_meters, timed_release_seconds
        FROM railway_control.signal_overlap_definitions
        ORDER BY signal_id
    )");

    if (query.exec()) {
        while (query.next()) {
            QVariantMap overlap;
            overlap["signalId"] = query.value("signal_id").toString();
            overlap["overlapCircuitIds"] = query.value("overlap_circuit_ids").toString();
            overlap["releaseTriggerCircuitIds"] = query.value("release_trigger_circuit_ids").toString();
            overlap["overlapDistanceMeters"] = query.value("overlap_distance_meters").toDouble();
            overlap["timedReleaseSeconds"] = query.value("timed_release_seconds").toInt();
            overlaps.append(overlap);
        }
    } else {
        logError("getAllSignalOverlapDefinitions", query.lastError());
    }

    return overlaps;
}

QString DatabaseManager::formatStringListForSQL(const QStringList& list) const {
    if (list.isEmpty()) {
        return "";  // Empty array
    }

    QStringList quotedItems;
    for (const QString& item : list) {
        //   SAFETY: Escape single quotes and wrap each item in quotes
        QString escaped = item;
        escaped.replace("'", "''");  // Escape single quotes
        quotedItems.append("'" + escaped + "'");
    }

    return quotedItems.join(",");
}
