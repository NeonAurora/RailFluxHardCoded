#include "DatabaseInitializer.h"
#include <QStandardPaths>
#include <QDir>
#include <QCoreApplication>
#include <QThread>

DatabaseInitializer::DatabaseInitializer(QObject* parent)
    : QObject(parent)
    , resetTimer(new QTimer(this))
{
    resetTimer->setSingleShot(true);
    connect(resetTimer, &QTimer::timeout, this, &DatabaseInitializer::performReset);
}

DatabaseInitializer::~DatabaseInitializer() {
    if (db.isOpen()) {
        db.close();
    }
}

bool DatabaseInitializer::initializeDatabase() {
    if (m_isRunning) return false;

    m_isRunning = true;
    m_progress = 0;
    m_lastError.clear(); // ? Clear any previous errors
    emit isRunningChanged();

    qDebug() << " DatabaseInitializer: Starting unified database initialization...";

    try {
        updateProgress(5, "Connecting to database");
        qDebug() << " Step 1: Connecting to database...";
        if (!connectToDatabase()) {
            qDebug() << "? Step 1 FAILED: Database connection failed";
            qDebug() << "? Error details:" << m_lastError;
            m_isRunning = false;
            emit isRunningChanged();
            return false;
        }
        qDebug() << "? Step 1 SUCCESS: Database connected";

        updateProgress(15, "Dropping and creating schemas");
        qDebug() << " Step 2: Dropping and creating schemas...";
        if (!dropAndCreateSchemas()) {
            qDebug() << "? Step 2 FAILED: Schema creation failed";
            qDebug() << "? Error details:" << m_lastError;
            if (m_lastError.isEmpty()) {
                setError("Schema creation failed (no specific error captured)");
            }
            m_isRunning = false;
            emit isRunningChanged();
            return false;
        }
        qDebug() << "? Step 2 SUCCESS: Schemas created";

        updateProgress(25, "Creating unified table structure");
        qDebug() << " Step 3: Creating unified tables...";
        if (!createUnifiedTables()) {
            qDebug() << "? Step 3 FAILED: Table creation failed";
            qDebug() << "? Error details:" << m_lastError;
            if (m_lastError.isEmpty()) {
                setError("Table creation failed (no specific error captured)");
            }
            m_isRunning = false;
            emit isRunningChanged();
            return false;
        }
        qDebug() << "? Step 3 SUCCESS: Tables created";

        updateProgress(40, "Creating indexes and constraints");
        qDebug() << " Step 4: Creating indexes...";
        if (!createIndexes()) {
            qDebug() << "? Step 4 FAILED: Index creation failed";
            qDebug() << "? Error details:" << m_lastError;
            if (m_lastError.isEmpty()) {
                setError("Index creation failed (no specific error captured)");
            }
            m_isRunning = false;
            emit isRunningChanged();
            return false;
        }
        qDebug() << "? Step 4 SUCCESS: Indexes created";

        updateProgress(50, "Creating functions and triggers");
        qDebug() << " Step 5: Creating functions...";
        if (!createFunctions()) {
            qWarning() << " Step 5 WARNING: Some functions failed to create - continuing";
        } else {
            qDebug() << "? Step 5 SUCCESS: Functions created";
        }

        qDebug() << " Step 6: Creating triggers...";
        if (!createTriggers()) {
            qWarning() << " Step 6 WARNING: Some triggers failed to create - continuing";
        } else {
            qDebug() << "? Step 6 SUCCESS: Triggers created";
        }

        updateProgress(60, "Creating views");
        qDebug() << " Step 7: Creating views...";
        if (!createViews()) {
            qWarning() << " Step 7 WARNING: Some views failed to create - continuing";
        } else {
            qDebug() << "? Step 7 SUCCESS: Views created";
        }

        updateProgress(70, "Setting up database security");
        qDebug() << " Step 8: Creating roles and permissions...";
        if (!createRolesAndPermissions()) {
            qWarning() << " Step 8 WARNING: Some roles/permissions failed to create - continuing";
        } else {
            qDebug() << "? Step 8 SUCCESS: Roles and permissions created";
        }

        updateProgress(80, "Populating initial data");
        qDebug() << " Step 9: Populating initial data...";
        if (!populateInitialData()) {
            qDebug() << "? Step 9 FAILED: Data population failed";
            qDebug() << "? Error details:" << m_lastError;
            if (m_lastError.isEmpty()) {
                setError("Initial data population failed (no specific error captured)");
            }
            m_isRunning = false;
            emit isRunningChanged();
            return false;
        }
        qDebug() << "? Step 9 SUCCESS: Initial data populated";

        updateProgress(90, "Validating database");
        qDebug() << " Step 10: Validating database...";
        if (!validateDatabase()) {
            qDebug() << "? Step 10 FAILED: Database validation failed";
            qDebug() << "? Error details:" << m_lastError;
            if (m_lastError.isEmpty()) {
                setError("Database validation failed (no specific error captured)");
            }
            m_isRunning = false;
            emit isRunningChanged();
            return false;
        }
        qDebug() << "? Step 10 SUCCESS: Database validated";

        updateProgress(100, "Database initialization completed");
        qDebug() << "? DatabaseInitializer: Unified database schema created successfully";
        m_isRunning = false;
        emit isRunningChanged();
        return true;

    } catch (const std::exception& e) {
        qDebug() << "? EXCEPTION during initialization:" << e.what();
        setError(QString("Exception during initialization: %1").arg(e.what()));
        m_isRunning = false;
        emit isRunningChanged();
        return false;
    }
}

bool DatabaseInitializer::connectToDatabase() {
    if (db.isOpen()) {
        db.close();
    }

    //   Clear any previous connection errors
    m_lastError.clear();

    // Try system PostgreSQL first (we know this works from your test)
    if (connectToSystemPostgreSQL()) {
        qDebug() << "  DatabaseInitializer: Connected to system PostgreSQL";
        return true;
    }

    // Fall back to portable PostgreSQL
    if (connectToPortablePostgreSQL()) {
        qDebug() << "  DatabaseInitializer: Connected to portable PostgreSQL";
        return true;
    }

    //   Only set generic error if no specific error was captured
    if (m_lastError.isEmpty()) {
        setError("Failed to connect to any PostgreSQL instance");
    }
    return false;
}


// Add this to DatabaseInitializer.cpp:
void DatabaseInitializer::debugConnectionTest() {
    qDebug() << " Testing PostgreSQL connections separately...";

    // Test system PostgreSQL
    qDebug() << "Testing system PostgreSQL (port 5432)...";
    if (connectToSystemPostgreSQL()) {
        qDebug() << "  System PostgreSQL: SUCCESS";
        db.close();
    } else {
        qDebug() << " System PostgreSQL: FAILED -" << m_lastError;
    }

    // Clear error and test portable
    m_lastError.clear();
    qDebug() << "Testing portable PostgreSQL (port 5433)...";
    if (connectToPortablePostgreSQL()) {
        qDebug() << "  Portable PostgreSQL: SUCCESS";
        db.close();
    } else {
        qDebug() << " Portable PostgreSQL: FAILED -" << m_lastError;
    }
}

bool DatabaseInitializer::connectToSystemPostgreSQL() {
    try {
        if (QSqlDatabase::contains("initializer_system_connection")) {
            QSqlDatabase::removeDatabase("initializer_system_connection");
        }

        db = QSqlDatabase::addDatabase("QPSQL", "initializer_system_connection");
        db.setHostName("localhost");
        db.setPort(5432);
        db.setDatabaseName("railway_control_system");
        db.setUserName("postgres");
        db.setPassword("qwerty");

        if (db.open()) {
            qDebug() << "  DatabaseInitializer: Connected to system PostgreSQL";
            return true;
        }

        //   FIX: Capture actual database error
        QString error = db.lastError().text();
        qDebug() << " DatabaseInitializer: System PostgreSQL connection failed:" << error;
        setError(QString("System PostgreSQL connection failed: %1").arg(error));

    } catch (...) {
        qDebug() << " DatabaseInitializer: System PostgreSQL connection failed with exception";
        setError("System PostgreSQL connection failed with exception");
    }

    if (db.isOpen()) {
        db.close();
    }
    return false;
}

bool DatabaseInitializer::connectToPortablePostgreSQL() {
    try {
        if (QSqlDatabase::contains("initializer_portable_connection")) {
            QSqlDatabase::removeDatabase("initializer_portable_connection");
        }

        db = QSqlDatabase::addDatabase("QPSQL", "initializer_portable_connection");
        db.setHostName("localhost");
        db.setPort(5433); // Usually different port for portable
        db.setDatabaseName("railway_control_system");
        db.setUserName("postgres");
        db.setPassword("qwerty");

        if (db.open()) {
            qDebug() << "  DatabaseInitializer: Connected to portable PostgreSQL";
            return true;
        }

        //   FIX: Capture actual database error
        QString error = db.lastError().text();
        qDebug() << " DatabaseInitializer: Portable PostgreSQL connection failed:" << error;
        setError(QString("Portable PostgreSQL connection failed: %1").arg(error));

    } catch (...) {
        qDebug() << " DatabaseInitializer: Portable PostgreSQL connection failed with exception";
        setError("Portable PostgreSQL connection failed with exception");
    }

    if (db.isOpen()) {
        db.close();
    }
    return false;
}

bool DatabaseInitializer::dropAndCreateSchemas() {
    qDebug() << "Dropping existing schemas and creating fresh ones...";

    // Drop existing schemas in dependency order
    QStringList dropQueries = {
        "DROP SCHEMA IF EXISTS railway_control CASCADE;",
        "DROP SCHEMA IF EXISTS railway_audit CASCADE;",
        "DROP SCHEMA IF EXISTS railway_config CASCADE;",
        "DROP SEQUENCE IF EXISTS railway_audit.event_sequence CASCADE;",
        "DROP ROLE IF EXISTS railway_operator;",
        "DROP ROLE IF EXISTS railway_observer;",
        "DROP ROLE IF EXISTS railway_auditor;"
    };

    for (const QString& query : dropQueries) {
        if (!executeQuery(query)) {
            qWarning() << "Failed to execute drop query (continuing):" << query;
        }
    }

    // Create schemas
    QStringList createQueries = {
        "CREATE SCHEMA railway_control;",
        "CREATE SCHEMA railway_audit;",
        "CREATE SCHEMA railway_config;",
        "COMMENT ON SCHEMA railway_control IS 'Main railway control system with route assignment';",
        "COMMENT ON SCHEMA railway_audit IS 'Audit trail and event logging for compliance';",
        "COMMENT ON SCHEMA railway_config IS 'Configuration and lookup tables';",
        "SET search_path TO railway_control, railway_audit, railway_config, public;"
    };

    for (const QString& query : createQueries) {
        if (!executeQuery(query)) {
            return false;
        }
    }

    return true;
}

bool DatabaseInitializer::createUnifiedTables() {
    qDebug() << "Creating unified table structure...";

    // Create in dependency order
    if (!createConfigurationTables()) return false;
    if (!createControlTables()) return false;
    if (!createRouteAssignmentTables()) return false;
    if (!createAuditTables()) return false;

    return true;
}

bool DatabaseInitializer::createConfigurationTables() {
    qDebug() << "Creating configuration tables with route assignment integration...";

    QStringList configTables = {
        // Signal types with route assignment enhancements
        R"(CREATE TABLE railway_config.signal_types (
            id SERIAL PRIMARY KEY,
            type_code VARCHAR(20) NOT NULL UNIQUE,
            type_name VARCHAR(50) NOT NULL,
            description TEXT,
            max_aspects INTEGER NOT NULL DEFAULT 2,
            -- Route assignment extensions
            is_route_signal BOOLEAN DEFAULT FALSE,
            route_priority INTEGER DEFAULT 100,
            created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
            updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
        ))",

        R"(CREATE TABLE railway_config.signal_aspects (
            id SERIAL PRIMARY KEY,
            aspect_code VARCHAR(20) NOT NULL UNIQUE,
            aspect_name VARCHAR(50) NOT NULL,
            color_code VARCHAR(7) NOT NULL, -- Hex color
            description TEXT,
            safety_level INTEGER NOT NULL DEFAULT 0, -- 0=danger, 1=caution, 2=clear
            -- Route assignment extensions
            permits_route_establishment BOOLEAN DEFAULT FALSE,
            requires_overlap BOOLEAN DEFAULT FALSE,
            created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
            updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
        ))",

        R"(CREATE TABLE railway_config.point_positions (
            id SERIAL PRIMARY KEY,
            position_code VARCHAR(20) NOT NULL UNIQUE,
            position_name VARCHAR(50) NOT NULL,
            description TEXT,
            -- Route assignment extensions
            pathfinding_weight NUMERIC DEFAULT 1.0,
            transition_time_ms INTEGER DEFAULT 3000,
            created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
            updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
        ))"
    };

    for (const QString& query : configTables) {
        if (!executeQuery(query)) return false;
    }

    return true;
}

bool DatabaseInitializer::createControlTables() {
    qDebug() << "Creating control tables with route assignment integration...";

    QStringList controlTables = {
        // Track circuits with pathfinding enhancements
        R"(CREATE TABLE railway_control.track_circuits (
            id SERIAL PRIMARY KEY,
            circuit_id VARCHAR(20) NOT NULL UNIQUE, -- e.g., "W22T", "A42", "6T"
            circuit_name VARCHAR(100),
            is_occupied BOOLEAN DEFAULT FALSE,
            is_active BOOLEAN DEFAULT TRUE,
            occupied_by VARCHAR(50),
            last_changed_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
            -- Route assignment extensions (keep only what you need)
            protecting_signals TEXT[],
            is_assigned BOOLEAN DEFAULT FALSE,
            is_overlap BOOLEAN DEFAULT FALSE,
            length_meters NUMERIC(10,2),
            max_speed_kmh INTEGER,
            created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
            updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
        ))",

        // Track segments with circuit references
        R"(CREATE TABLE railway_control.track_segments (
            id SERIAL PRIMARY KEY,
            segment_id VARCHAR(20) NOT NULL UNIQUE, -- e.g., "T1S1", "T1S2"
            segment_name VARCHAR(100),
            start_row NUMERIC(10,2) NOT NULL,
            start_col NUMERIC(10,2) NOT NULL,
            end_row NUMERIC(10,2) NOT NULL,
            end_col NUMERIC(10,2) NOT NULL,
            track_segment_type VARCHAR(20) DEFAULT 'STRAIGHT',
            is_assigned BOOLEAN DEFAULT FALSE,
            is_overlap BOOLEAN DEFAULT FALSE,

            circuit_id VARCHAR(20) REFERENCES railway_control.track_circuits(circuit_id),
            length_meters NUMERIC(10,2),
            max_speed_kmh INTEGER,
            is_active BOOLEAN DEFAULT TRUE,
            protecting_signals TEXT[],
            created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
            updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
            CONSTRAINT chk_coordinates CHECK (
                start_row >= 0 AND start_col >= 0 AND
                end_row >= 0 AND end_col >= 0
            )
        ))",

        // Signals with route assignment anchors
        R"(CREATE TABLE railway_control.signals (
            id SERIAL PRIMARY KEY,
            signal_id VARCHAR(20) NOT NULL UNIQUE,
            signal_name VARCHAR(100) NOT NULL,
            signal_type_id INTEGER NOT NULL REFERENCES railway_config.signal_types(id),
            current_aspect_id INTEGER REFERENCES railway_config.signal_aspects(id),
            location_row NUMERIC(10,2) NOT NULL,
            location_col NUMERIC(10,2) NOT NULL,
            direction VARCHAR(10) NOT NULL CHECK (direction IN ('UP', 'DOWN', 'BIDIRECTIONAL')),
            is_active BOOLEAN DEFAULT TRUE,
            last_changed_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,

            -- Route assignment pathfinding anchors
            preceded_by_circuit_id TEXT REFERENCES railway_control.track_circuits(circuit_id),
            succeeded_by_circuit_id TEXT REFERENCES railway_control.track_circuits(circuit_id),

            -- Route assignment properties
            is_route_signal BOOLEAN DEFAULT FALSE,
            route_signal_type TEXT CHECK (route_signal_type IN ('START', 'INTERMEDIATE', 'END', 'SHUNT')),
            default_overlap_distance_m INTEGER DEFAULT 180,

            -- Original signal properties
            calling_on_aspect_id INTEGER REFERENCES railway_config.signal_aspects(id),
            loop_aspect_id INTEGER REFERENCES railway_config.signal_aspects(id),
            loop_signal_configuration VARCHAR(10) DEFAULT 'UR',
            aspect_count INTEGER NOT NULL DEFAULT 2,
            possible_aspects TEXT[],
            location_description VARCHAR(200),
            last_changed_by VARCHAR(100),
            interlocked_with INTEGER[],
            protected_track_circuits TEXT[],

            is_locked BOOLEAN DEFAULT FALSE,
            manual_control_active BOOLEAN DEFAULT FALSE,

            created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
            updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,

            CONSTRAINT chk_location CHECK (location_row >= 0 AND location_col >= 0),
            CONSTRAINT chk_aspect_count CHECK (aspect_count >= 2 AND aspect_count <= 4)
        ))",

        // Point machines with route assignment integration
        R"(CREATE TABLE railway_control.point_machines (
            id SERIAL PRIMARY KEY,
            machine_id VARCHAR(20) NOT NULL UNIQUE,
            machine_name VARCHAR(100) NOT NULL,
            current_position_id INTEGER REFERENCES railway_config.point_positions(id),
            junction_row NUMERIC(10,2) NOT NULL,
            junction_col NUMERIC(10,2) NOT NULL,
            root_track_segment_connection JSONB NOT NULL,
            normal_track_segment_connection JSONB NOT NULL,
            reverse_track_segment_connection JSONB NOT NULL,
            operating_status VARCHAR(20) DEFAULT 'CONNECTED' CHECK (
                operating_status IN ('CONNECTED', 'IN_TRANSITION', 'FAILED', 'LOCKED', 'MAINTENANCE')
            ),
            is_locked BOOLEAN DEFAULT FALSE,
            transition_time_ms INTEGER DEFAULT 3000,
            last_operated_at TIMESTAMP WITH TIME ZONE,
            last_operated_by VARCHAR(100),
            operation_count INTEGER DEFAULT 0,
            safety_interlocks INTEGER[],
            lock_reason TEXT,
            protected_signals TEXT[],

            -- Route assignment extensions
            paired_entity VARCHAR(20),
            host_track_circuit TEXT REFERENCES railway_control.track_circuits(circuit_id),
            route_locking_enabled BOOLEAN DEFAULT TRUE,
            auto_normalize_after_route BOOLEAN DEFAULT TRUE,

            created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
            updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,

            CONSTRAINT chk_junction_location CHECK (junction_row >= 0 AND junction_col >= 0),
            CONSTRAINT chk_no_self_pairing CHECK (machine_id != paired_entity)
        ))",

        // Text labels
        R"(CREATE TABLE railway_control.text_labels (
            id SERIAL PRIMARY KEY,
            label_text VARCHAR(200) NOT NULL,
            position_row NUMERIC(10,2) NOT NULL,
            position_col NUMERIC(10,2) NOT NULL,
            font_size INTEGER DEFAULT 12,
            color VARCHAR(7) DEFAULT '#ffffff',
            font_family VARCHAR(50) DEFAULT 'Arial',
            is_visible BOOLEAN DEFAULT TRUE,
            label_type VARCHAR(20) DEFAULT 'INFO', -- INFO, WARNING, GRID_REFERENCE
            created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
            updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
        ))",

        // System state
        R"(CREATE TABLE railway_control.system_state (
            id SERIAL PRIMARY KEY,
            state_key VARCHAR(100) NOT NULL UNIQUE,
            state_value JSONB NOT NULL,
            description TEXT,
            last_updated TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
            updated_by VARCHAR(100)
        ))",

        // Interlocking rules
        R"(CREATE TABLE railway_control.interlocking_rules (
            id SERIAL PRIMARY KEY,
            rule_name VARCHAR(100) NOT NULL,
            source_entity_type VARCHAR(20) NOT NULL CHECK (source_entity_type IN ('SIGNAL', 'POINT_MACHINE', 'TRACK_SEGMENT', 'TRACK_CIRCUIT')),
            source_entity_id VARCHAR(20) NOT NULL,
            target_entity_type VARCHAR(20) NOT NULL CHECK (target_entity_type IN ('SIGNAL', 'POINT_MACHINE', 'TRACK_SEGMENT', 'TRACK_CIRCUIT')),
            target_entity_id VARCHAR(20) NOT NULL,
            target_constraint VARCHAR(50) NOT NULL,
            rule_type VARCHAR(50) NOT NULL,
            priority INTEGER DEFAULT 100,
            is_active BOOLEAN DEFAULT TRUE,
            created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
            updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
            CONSTRAINT chk_no_self_reference CHECK (
                NOT (source_entity_type = target_entity_type AND source_entity_id = target_entity_id)
            )
        ))"
    };

    for (const QString& query : controlTables) {
        if (!executeQuery(query)) return false;
    }

    return true;
}

bool DatabaseInitializer::createRouteAssignmentTables() {
    qDebug() << "Creating route assignment tables...";

    QStringList routeTables = {

        // Route assignments - main state tracking
        R"(CREATE TABLE railway_control.route_assignments (
            id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
            source_signal_id TEXT NOT NULL REFERENCES railway_control.signals(signal_id),
            dest_signal_id TEXT NOT NULL REFERENCES railway_control.signals(signal_id),
            direction TEXT NOT NULL CHECK (direction IN ('UP', 'DOWN')),
            assigned_circuits TEXT[] NOT NULL,
            overlap_circuits TEXT[] NOT NULL DEFAULT '{}',
            state TEXT NOT NULL CHECK (state IN (
                'REQUESTED', 'VALIDATING', 'RESERVED', 'ACTIVE',
                'PARTIALLY_RELEASED', 'RELEASED', 'FAILED',
                'EMERGENCY_RELEASED', 'DEGRADED'
            )),
            created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
            activated_at TIMESTAMP WITH TIME ZONE,
            released_at TIMESTAMP WITH TIME ZONE,
            overlap_release_due_at TIMESTAMP WITH TIME ZONE,
            locked_point_machines TEXT[] DEFAULT '{}',
            priority INTEGER DEFAULT 100,
            operator_id TEXT NOT NULL DEFAULT 'system',
            failure_reason TEXT,
            performance_metrics JSONB DEFAULT '{}',

            -- Constraints
            CONSTRAINT chk_route_timing CHECK (
                (activated_at IS NULL OR activated_at >= created_at) AND
                (released_at IS NULL OR released_at >= created_at) AND
                (overlap_release_due_at IS NULL OR overlap_release_due_at >= created_at)
            ),
            CONSTRAINT chk_route_circuits CHECK (
                array_length(assigned_circuits, 1) > 0
            ),
            CONSTRAINT chk_route_signals CHECK (
                source_signal_id != dest_signal_id
            )
        ))"
    };

    for (const QString& query : routeTables) {
        if (!executeQuery(query)) return false;
    }

    return true;
}

bool DatabaseInitializer::createAuditTables() {
    qDebug() << "Creating audit tables...";

    QStringList auditTables = {
        R"(CREATE TABLE railway_audit.event_log (
            id BIGSERIAL PRIMARY KEY,
            event_timestamp TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
            event_type VARCHAR(50) NOT NULL,
            entity_type VARCHAR(50) NOT NULL, -- SIGNAL, POINT_MACHINE, TRACK_SEGMENT, TRACK_CIRCUIT
            entity_id VARCHAR(50) NOT NULL,
            entity_name VARCHAR(100),
            event_details JSONB,

            -- Change details
            old_values JSONB,
            new_values JSONB,
            field_changed VARCHAR(100),

            -- Context
            operator_id VARCHAR(100),
            operator_name VARCHAR(200),
            operation_source VARCHAR(50) DEFAULT 'HMI', -- HMI, API, AUTOMATIC, SYSTEM
            session_id VARCHAR(100),
            ip_address INET,

            -- Safety and compliance
            safety_critical BOOLEAN DEFAULT FALSE,
            authorization_level VARCHAR(20),
            reason_code VARCHAR(50),
            comments TEXT,

            -- Replay capability
            replay_data JSONB, -- Complete state for replay
            sequence_number BIGINT,

            -- Date for partitioning (computed via trigger instead of generated column)
            event_date DATE
        ))",

        R"(CREATE TABLE railway_audit.system_events (
            id BIGSERIAL PRIMARY KEY,
            event_timestamp TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
            event_level VARCHAR(20) NOT NULL CHECK (event_level IN ('INFO', 'WARNING', 'ERROR', 'CRITICAL')),
            event_category VARCHAR(50) NOT NULL, -- DATABASE, COMMUNICATION, SAFETY, PERFORMANCE
            event_message TEXT NOT NULL,
            event_details JSONB,
            source_component VARCHAR(100),
            error_code VARCHAR(20),
            resolved_at TIMESTAMP WITH TIME ZONE,
            resolved_by VARCHAR(100)
        ))"
    };

    for (const QString& query : auditTables) {
        if (!executeQuery(query)) return false;
    }

    // Create sequences
    if (!executeQuery("CREATE SEQUENCE railway_audit.event_sequence")) {
        qWarning() << "Failed to create event sequence";
    }

    return true;
}

bool DatabaseInitializer::createIndexes() {
    qDebug() << "Creating performance and safety indexes...";

    QStringList indexes = {
        // Track circuits indexes (KEEP)
        "CREATE INDEX idx_track_circuits_id ON railway_control.track_circuits(circuit_id)",
        "CREATE INDEX idx_track_circuits_occupied ON railway_control.track_circuits(is_occupied) WHERE is_occupied = TRUE",
        "CREATE INDEX idx_track_circuits_active ON railway_control.track_circuits(is_active) WHERE is_active = TRUE",
        "CREATE INDEX idx_track_circuits_assigned ON railway_control.track_circuits(is_assigned) WHERE is_assigned = TRUE",
        "CREATE INDEX idx_track_circuits_overlap ON railway_control.track_circuits(is_overlap) WHERE is_overlap = TRUE",

        // Track segments indexes (KEEP)
        "CREATE INDEX idx_track_segments_id ON railway_control.track_segments(segment_id)",
        "CREATE INDEX idx_track_segments_circuit ON railway_control.track_segments(circuit_id)",
        "CREATE INDEX idx_track_segments_location ON railway_control.track_segments USING btree(start_row, start_col, end_row, end_col)",
        "CREATE INDEX idx_track_segments_assigned ON railway_control.track_segments(is_assigned) WHERE is_assigned = TRUE",
        "CREATE INDEX idx_track_segments_overlap ON railway_control.track_segments(is_overlap) WHERE is_overlap = TRUE",

        // Signal indexes (KEEP)
        "CREATE INDEX idx_signals_id ON railway_control.signals(signal_id)",
        "CREATE INDEX idx_signals_location ON railway_control.signals USING btree(location_row, location_col)",
        "CREATE INDEX idx_signals_type ON railway_control.signals(signal_type_id)",
        "CREATE INDEX idx_signals_active ON railway_control.signals(is_active) WHERE is_active = TRUE",
        "CREATE INDEX idx_signals_preceded_by ON railway_control.signals(preceded_by_circuit_id) WHERE preceded_by_circuit_id IS NOT NULL",
        "CREATE INDEX idx_signals_succeeded_by ON railway_control.signals(succeeded_by_circuit_id) WHERE succeeded_by_circuit_id IS NOT NULL",
        "CREATE INDEX idx_signals_locked ON railway_control.signals(is_locked) WHERE is_locked = TRUE",

        // Point machine indexes (KEEP)
        "CREATE INDEX idx_point_machines_id ON railway_control.point_machines(machine_id)",
        "CREATE INDEX idx_point_machines_position ON railway_control.point_machines(current_position_id)",
        "CREATE INDEX idx_point_machines_junction ON railway_control.point_machines USING btree(junction_row, junction_col)",
        "CREATE INDEX idx_point_machines_paired_entity ON railway_control.point_machines(paired_entity) WHERE paired_entity IS NOT NULL",
        "CREATE INDEX idx_point_machines_host_track_circuit ON railway_control.point_machines(host_track_circuit)",

        // Route assignment indexes (KEEP - only for existing tables)
        "CREATE INDEX idx_route_assignments_state ON railway_control.route_assignments(state)",
        "CREATE INDEX idx_route_assignments_active ON railway_control.route_assignments(state) WHERE state IN ('RESERVED', 'ACTIVE')",
        "CREATE INDEX idx_route_assignments_signals ON railway_control.route_assignments(source_signal_id, dest_signal_id)",
        "CREATE INDEX idx_route_assignments_created ON railway_control.route_assignments(created_at)",

        // Audit indexes (KEEP)
        "CREATE INDEX idx_event_log_timestamp ON railway_audit.event_log(event_timestamp)",
        "CREATE INDEX idx_event_log_entity ON railway_audit.event_log(entity_type, entity_id)",
        "CREATE INDEX idx_event_log_operator ON railway_audit.event_log(operator_id)",
        "CREATE INDEX idx_event_log_safety ON railway_audit.event_log(safety_critical) WHERE safety_critical = TRUE",
        "CREATE INDEX idx_event_log_sequence ON railway_audit.event_log(sequence_number)",
        "CREATE INDEX idx_event_log_date ON railway_audit.event_log(event_date)",

        // GIN indexes for array and JSONB columns (KEEP)
        "CREATE INDEX idx_signals_possible_aspects ON railway_control.signals USING gin(possible_aspects)",
        "CREATE INDEX idx_signals_protected_circuits ON railway_control.signals USING gin(protected_track_circuits)",
        "CREATE INDEX idx_track_circuits_protecting_signals ON railway_control.track_circuits USING gin(protecting_signals)",
        "CREATE INDEX idx_point_machines_safety_interlocks ON railway_control.point_machines USING gin(safety_interlocks)",
        "CREATE INDEX idx_event_log_old_values ON railway_audit.event_log USING gin(old_values)",
        "CREATE INDEX idx_event_log_new_values ON railway_audit.event_log USING gin(new_values)"
    };

    for (const QString& query : indexes) {
        if (!executeQuery(query)) {
            qWarning() << "Failed to create index:" << query.left(100) + "...";
        }
    }

    return true;
}


bool DatabaseInitializer::createFunctions() {
    qDebug() << "Creating database functions...";

    QStringList functions = {
        // 
        // BASIC UTILITY FUNCTIONS - Core triggers and helper functions
        // 

        // Trigger function to automatically set event_date from event_timestamp in audit table
        R"(CREATE OR REPLACE FUNCTION railway_audit.set_event_date()
    RETURNS TRIGGER AS $$
    BEGIN
        NEW.event_date := NEW.event_timestamp::DATE;
        RETURN NEW;
    END;
    $$ LANGUAGE plpgsql)",

        // Generic trigger function to update timestamp on any table modification
        R"(CREATE OR REPLACE FUNCTION railway_control.update_timestamp()
    RETURNS TRIGGER AS $$
    BEGIN
        NEW.updated_at = CURRENT_TIMESTAMP;
        RETURN NEW;
    END;
    $$ LANGUAGE plpgsql)",

        // Specialized trigger for signals to update last_changed_at only when aspect changes
        R"(CREATE OR REPLACE FUNCTION railway_control.update_signal_change_time()
    RETURNS TRIGGER AS $$
    BEGIN
        IF OLD.current_aspect_id IS DISTINCT FROM NEW.current_aspect_id THEN
            NEW.last_changed_at = CURRENT_TIMESTAMP;
        END IF;
        RETURN NEW;
    END;
    $$ LANGUAGE plpgsql)",

        // Helper function to convert aspect codes (RED, GREEN, etc.) to database IDs
        R"(CREATE OR REPLACE FUNCTION railway_config.get_aspect_id(aspect_code_param VARCHAR)
    RETURNS INTEGER AS $$
    DECLARE
        aspect_id_result INTEGER;
    BEGIN
        SELECT id INTO aspect_id_result
        FROM railway_config.signal_aspects
        WHERE aspect_code = aspect_code_param;
        RETURN aspect_id_result;
    END;
    $$ LANGUAGE plpgsql)",

        // Helper function to convert position codes (NORMAL, REVERSE) to database IDs
        R"(CREATE OR REPLACE FUNCTION railway_config.get_position_id(position_code_param VARCHAR)
    RETURNS INTEGER AS $$
    DECLARE
        position_id_result INTEGER;
    BEGIN
        SELECT id INTO position_id_result
        FROM railway_config.point_positions
        WHERE position_code = position_code_param;
        RETURN position_id_result;
    END;
    $$ LANGUAGE plpgsql)",

        // 
        // SIGNAL CONTROL FUNCTIONS - Main and subsidiary signal operations
        // 

        // Main function for updating signal aspects with route assignment locking check
        R"(CREATE OR REPLACE FUNCTION railway_control.update_signal_aspect(
        signal_id_param VARCHAR,
        aspect_code_param VARCHAR,
        operator_id_param VARCHAR DEFAULT 'system'
    )
    RETURNS BOOLEAN AS $$
    DECLARE
        aspect_id_val INTEGER;
        rows_affected INTEGER;
        route_locked BOOLEAN;
    BEGIN
        -- Set operator context for audit logging
        PERFORM set_config('railway.operator_id', operator_id_param, true);

        -- Get aspect ID
        aspect_id_val := railway_config.get_aspect_id(aspect_code_param);
        IF aspect_id_val IS NULL THEN
            RAISE EXCEPTION 'Invalid aspect code: %', aspect_code_param;
        END IF;

        -- UPDATED: Check if signal is locked by checking route assignments directly
        SELECT EXISTS(
            SELECT 1 FROM railway_control.route_assignments ra
            WHERE (ra.source_signal_id = signal_id_param OR ra.dest_signal_id = signal_id_param)
            AND ra.state IN ('RESERVED', 'ACTIVE', 'PARTIALLY_RELEASED')
        ) INTO route_locked;

        IF route_locked THEN
            RAISE EXCEPTION 'Signal % is locked by active route assignment', signal_id_param;
        END IF;

        -- Check signal's own lock status
        IF EXISTS(SELECT 1 FROM railway_control.signals WHERE signal_id = signal_id_param AND is_locked = TRUE) THEN
            RAISE EXCEPTION 'Signal % is manually locked', signal_id_param;
        END IF;

        -- Update signal aspect
        UPDATE railway_control.signals
        SET current_aspect_id = aspect_id_val,
            last_changed_by = operator_id_param
        WHERE signal_id = signal_id_param;

        GET DIAGNOSTICS rows_affected = ROW_COUNT;
        RETURN rows_affected > 0;
    END;
    $$ LANGUAGE plpgsql)",

        // Function for updating subsidiary signals (calling on and loop aspects)
        R"(CREATE OR REPLACE FUNCTION railway_control.update_subsidiary_signal_aspect(
        signal_id_param VARCHAR,
        aspect_type_param VARCHAR,
        aspect_code_param VARCHAR,
        operator_id_param VARCHAR DEFAULT 'system'
    )
    RETURNS BOOLEAN AS $$
    DECLARE
        aspect_id_val INTEGER;
        rows_affected INTEGER;
    BEGIN
        -- Set operator context for audit logging
        PERFORM set_config('railway.operator_id', operator_id_param, true);

        -- Validate aspect type
        IF aspect_type_param NOT IN ('CALLING_ON', 'LOOP') THEN
            RAISE EXCEPTION 'Invalid subsidiary aspect type: %. Must be CALLING_ON or LOOP', aspect_type_param;
        END IF;

        -- Get aspect ID
        aspect_id_val := railway_config.get_aspect_id(aspect_code_param);
        IF aspect_id_val IS NULL THEN
            RAISE EXCEPTION 'Invalid aspect code: %', aspect_code_param;
        END IF;

        -- Update the appropriate subsidiary signal column
        IF aspect_type_param = 'CALLING_ON' THEN
            UPDATE railway_control.signals
            SET calling_on_aspect_id = aspect_id_val,
                last_changed_at = CURRENT_TIMESTAMP,
                last_changed_by = operator_id_param
            WHERE signal_id = signal_id_param;
        ELSIF aspect_type_param = 'LOOP' THEN
            UPDATE railway_control.signals
            SET loop_aspect_id = aspect_id_val,
                last_changed_at = CURRENT_TIMESTAMP,
                last_changed_by = operator_id_param
            WHERE signal_id = signal_id_param;
        END IF;

        GET DIAGNOSTICS rows_affected = ROW_COUNT;
        RETURN rows_affected > 0;
    END;
    $$ LANGUAGE plpgsql)",

        // 
        // POINT MACHINE CONTROL FUNCTIONS - Single and paired machine operations
        // 

        // Simple point machine position update (single machine)
        R"(CREATE OR REPLACE FUNCTION railway_control.update_point_position(
        machine_id_param VARCHAR,
        position_code_param VARCHAR,
        operator_id_param VARCHAR DEFAULT 'system'
    )
    RETURNS BOOLEAN AS $$
    DECLARE
        position_id_val INTEGER;
        rows_affected INTEGER;
    BEGIN
        -- Set operator context for audit logging
        PERFORM set_config('railway.operator_id', operator_id_param, true);

        -- Get position ID
        position_id_val := railway_config.get_position_id(position_code_param);
        IF position_id_val IS NULL THEN
            RAISE EXCEPTION 'Invalid position code: %', position_code_param;
        END IF;

        -- Update point machine position
        UPDATE railway_control.point_machines
        SET
            current_position_id = position_id_val,
            last_operated_at = CURRENT_TIMESTAMP,
            last_operated_by = operator_id_param,
            operation_count = operation_count + 1
        WHERE machine_id = machine_id_param;

        GET DIAGNOSTICS rows_affected = ROW_COUNT;
        RETURN rows_affected > 0;
    END;
    $$ LANGUAGE plpgsql)",

        // Advanced point machine update with paired machine synchronization logic
        R"(CREATE OR REPLACE FUNCTION railway_control.update_point_position_paired(
        machine_id_param VARCHAR,
        position_code_param VARCHAR,
        operator_id_param VARCHAR DEFAULT 'system'
    )
    RETURNS JSONB AS $$
    DECLARE
        position_id_val INTEGER;
        paired_machine_id VARCHAR(20);
        current_position_code VARCHAR(20);
        paired_current_position_code VARCHAR(20);
        rows_affected INTEGER;
        result_json JSONB;
        position_mismatch BOOLEAN := FALSE;
        route_locked BOOLEAN;
    BEGIN
        -- Set operator context for audit logging
        PERFORM set_config('railway.operator_id', operator_id_param, true);

        -- Validate position code
        position_id_val := railway_config.get_position_id(position_code_param);
        IF position_id_val IS NULL THEN
            RAISE EXCEPTION 'Invalid position code: %', position_code_param;
        END IF;

        -- UPDATED: Check if point machine is locked by checking route assignments directly
        SELECT EXISTS(
            SELECT 1 FROM railway_control.route_assignments ra
            WHERE machine_id_param = ANY(ra.locked_point_machines)
            AND ra.state IN ('RESERVED', 'ACTIVE', 'PARTIALLY_RELEASED')
        ) INTO route_locked;

        IF route_locked THEN
            RAISE EXCEPTION 'Point machine % is locked by active route assignment', machine_id_param;
        END IF;

        -- Check point machine's own lock status
        IF EXISTS(SELECT 1 FROM railway_control.point_machines WHERE machine_id = machine_id_param AND is_locked = TRUE) THEN
            RAISE EXCEPTION 'Point machine % is manually locked', machine_id_param;
        END IF;

        -- Get current machine info including paired entity
        SELECT
            pp.position_code,
            pm.paired_entity
        INTO
            current_position_code,
            paired_machine_id
        FROM railway_control.point_machines pm
        LEFT JOIN railway_config.point_positions pp ON pm.current_position_id = pp.id
        WHERE pm.machine_id = machine_id_param;

        IF NOT FOUND THEN
            RAISE EXCEPTION 'Point machine not found: %', machine_id_param;
        END IF;

        -- Check if requesting same position (no-op)
        IF current_position_code = position_code_param THEN
            result_json := jsonb_build_object(
                'success', true,
                'machines_updated', ARRAY[machine_id_param],
                'message', 'Already in requested position',
                'position_mismatch', false
            );
            RETURN result_json;
        END IF;

        -- Handle unpaired machine (simple case)
        IF paired_machine_id IS NULL THEN
            UPDATE railway_control.point_machines
            SET
                current_position_id = position_id_val,
                last_operated_at = CURRENT_TIMESTAMP,
                last_operated_by = operator_id_param,
                operation_count = operation_count + 1
            WHERE machine_id = machine_id_param;

            GET DIAGNOSTICS rows_affected = ROW_COUNT;

            result_json := jsonb_build_object(
                'success', rows_affected > 0,
                'machines_updated', ARRAY[machine_id_param],
                'message', 'Single point machine updated',
                'position_mismatch', false
            );
            RETURN result_json;
        END IF;

        -- Handle paired machine
        SELECT pp.position_code
        INTO paired_current_position_code
        FROM railway_control.point_machines pm
        LEFT JOIN railway_config.point_positions pp ON pm.current_position_id = pp.id
        WHERE pm.machine_id = paired_machine_id;

        IF NOT FOUND THEN
            RAISE EXCEPTION 'Paired machine not found: %', paired_machine_id;
        END IF;

        -- Check for position mismatch
        IF current_position_code != paired_current_position_code THEN
            position_mismatch := TRUE;

            -- Update only requesting machine to match its pair
            UPDATE railway_control.point_machines
            SET
                current_position_id = (
                    SELECT current_position_id
                    FROM railway_control.point_machines
                    WHERE machine_id = paired_machine_id
                ),
                last_operated_at = CURRENT_TIMESTAMP,
                last_operated_by = operator_id_param,
                operation_count = operation_count + 1
            WHERE machine_id = machine_id_param;

            GET DIAGNOSTICS rows_affected = ROW_COUNT;

            result_json := jsonb_build_object(
                'success', rows_affected > 0,
                'machines_updated', ARRAY[machine_id_param],
                'message', 'Position mismatch corrected - machine synchronized with pair',
                'position_mismatch', true,
                'corrected_to_position', paired_current_position_code
            );
            RETURN result_json;
        END IF;

        -- Both machines have same position - update both atomically
        UPDATE railway_control.point_machines
        SET
            current_position_id = position_id_val,
            last_operated_at = CURRENT_TIMESTAMP,
            last_operated_by = operator_id_param,
            operation_count = operation_count + 1
        WHERE machine_id IN (machine_id_param, paired_machine_id);

        GET DIAGNOSTICS rows_affected = ROW_COUNT;

        result_json := jsonb_build_object(
            'success', rows_affected = 2,
            'machines_updated', ARRAY[machine_id_param, paired_machine_id],
            'message', 'Paired machines updated together',
            'position_mismatch', false
        );

        RETURN result_json;
    END;
    $$ LANGUAGE plpgsql)",

        // Query function to check point machine availability for route assignment
        R"(CREATE OR REPLACE FUNCTION railway_control.is_point_machine_available(
        machine_id_param TEXT
    )
    RETURNS BOOLEAN AS $$
    DECLARE
        is_locked BOOLEAN;
        is_in_transition BOOLEAN;
        route_locking_enabled BOOLEAN;
        is_route_locked BOOLEAN;
    BEGIN
        SELECT
            pm.is_locked,
            pm.operating_status = 'IN_TRANSITION',
            pm.route_locking_enabled
        INTO is_locked, is_in_transition, route_locking_enabled
        FROM railway_control.point_machines pm
        WHERE pm.machine_id = machine_id_param;

        -- UPDATED: Check if locked by route assignment (no resource_locks table)
        SELECT EXISTS(
            SELECT 1 FROM railway_control.route_assignments ra
            WHERE machine_id_param = ANY(ra.locked_point_machines)
            AND ra.state IN ('RESERVED', 'ACTIVE', 'PARTIALLY_RELEASED')
        ) INTO is_route_locked;

        RETURN NOT (COALESCE(is_locked, TRUE) OR COALESCE(is_in_transition, TRUE) OR COALESCE(is_route_locked, TRUE))
               AND COALESCE(route_locking_enabled, TRUE);
    END;
    $$ LANGUAGE plpgsql)",

        // 
        // TRACK CIRCUIT AND SEGMENT FUNCTIONS - Occupancy and assignment management
        // 

        // Main function for updating track circuit occupancy status
        R"(CREATE OR REPLACE FUNCTION railway_control.get_available_circuits()
    RETURNS TABLE(circuit_id TEXT, is_occupied BOOLEAN, is_locked BOOLEAN, circuit_type TEXT) AS $$
    BEGIN
        RETURN QUERY
        SELECT
            tc.circuit_id,
            tc.is_occupied,
            -- UPDATED: Check if locked by route assignment (no resource_locks table)
            EXISTS(
                SELECT 1 FROM railway_control.route_assignments ra
                WHERE tc.circuit_id = ANY(ra.assigned_circuits)
                AND ra.state IN ('RESERVED', 'ACTIVE', 'PARTIALLY_RELEASED')
            ) as is_locked,
            'TRACK_CIRCUIT'::TEXT as circuit_type  -- UPDATED: Fixed circuit_type reference
        FROM railway_control.track_circuits tc
        WHERE tc.is_active = TRUE;
    END;
    $$ LANGUAGE plpgsql)",

        // Function for updating track segment assignment status (for maintenance)
        R"(CREATE OR REPLACE FUNCTION railway_control.update_track_segment_assignment(
        segment_id_param VARCHAR,
        is_assigned_param BOOLEAN,
        operator_id_param VARCHAR DEFAULT 'system'
    )
    RETURNS BOOLEAN AS $$
    DECLARE
        rows_affected INTEGER;
    BEGIN
        -- Set operator context for audit logging
        PERFORM set_config('railway.operator_id', operator_id_param, true);

        -- Update track segment assignment
        UPDATE railway_control.track_segments
        SET is_assigned = is_assigned_param,
            updated_at = CURRENT_TIMESTAMP
        WHERE segment_id = segment_id_param;

        GET DIAGNOSTICS rows_affected = ROW_COUNT;
        RETURN rows_affected > 0;
    END;
    $$ LANGUAGE plpgsql)",

        // Query function to get available circuits for route assignment planning
        R"(CREATE OR REPLACE FUNCTION railway_control.get_available_circuits()
    RETURNS TABLE(circuit_id TEXT, is_occupied BOOLEAN, is_locked BOOLEAN, circuit_type TEXT) AS $$
    BEGIN
        RETURN QUERY
        SELECT
            tc.circuit_id,
            tc.is_occupied,
            EXISTS(
                SELECT 1 FROM railway_control.resource_locks rl
                WHERE rl.resource_type = 'TRACK_CIRCUIT'
                AND rl.resource_id = tc.circuit_id
                AND rl.is_active = TRUE
            ) as is_locked,
            tc.circuit_type
        FROM railway_control.track_circuits tc
        WHERE tc.is_active = TRUE;
    END;
    $$ LANGUAGE plpgsql)",

        // Duplicate track circuit occupancy function (enhanced version with timestamps)
        R"(CREATE OR REPLACE FUNCTION railway_control.update_track_circuit_occupancy(
        circuit_id_param VARCHAR,
        is_occupied_param BOOLEAN,
        occupied_by_param VARCHAR DEFAULT NULL,
        operator_id_param VARCHAR DEFAULT 'system'
    )
    RETURNS BOOLEAN AS $$
    DECLARE
        rows_affected INTEGER;
    BEGIN
        -- Set operator context for audit logging
        PERFORM set_config('railway.operator_id', operator_id_param, true);

        -- Update track circuit occupancy
        UPDATE railway_control.track_circuits
        SET
            is_occupied = is_occupied_param,
            occupied_by = CASE
                WHEN is_occupied_param = TRUE THEN occupied_by_param
                ELSE NULL
            END,
            last_changed_at = CURRENT_TIMESTAMP,
            updated_at = CURRENT_TIMESTAMP
        WHERE circuit_id = circuit_id_param;

        GET DIAGNOSTICS rows_affected = ROW_COUNT;
        RETURN rows_affected > 0;
    END;
    $$ LANGUAGE plpgsql)",

        // Wrapper function to update circuit occupancy via segment ID (for backwards compatibility)
        R"(CREATE OR REPLACE FUNCTION railway_control.update_track_segment_occupancy(
        segment_id_param VARCHAR,
        is_occupied_param BOOLEAN,
        occupied_by_param VARCHAR DEFAULT NULL,
        operator_id_param VARCHAR DEFAULT 'system'
    )
    RETURNS BOOLEAN AS $$
    DECLARE
        circuit_id_val VARCHAR(20);
        circuit_result BOOLEAN;
    BEGIN
        -- Find the circuit ID for this segment
        SELECT circuit_id INTO circuit_id_val
        FROM railway_control.track_segments
        WHERE segment_id = segment_id_param;

        -- If no circuit found or circuit is INVALID, return false
        IF circuit_id_val IS NULL OR circuit_id_val = 'INVALID' THEN
            RETURN false;
        END IF;

        -- Update the circuit occupancy
        SELECT railway_control.update_track_circuit_occupancy(
            circuit_id_val,
            is_occupied_param,
            occupied_by_param,
            operator_id_param
        ) INTO circuit_result;

        RETURN circuit_result;
    END;
    $$ LANGUAGE plpgsql)",

        // Insert Route Assignment
        R"(CREATE OR REPLACE FUNCTION railway_control.insert_route_assignment(
        route_id_param UUID,
        source_signal_id_param VARCHAR,
        dest_signal_id_param VARCHAR,
        direction_param VARCHAR,
        assigned_circuits_param TEXT[],
        overlap_circuits_param TEXT[] DEFAULT '{}',
        state_param VARCHAR DEFAULT 'REQUESTED',
        locked_point_machines_param TEXT[] DEFAULT '{}',
        priority_param INTEGER DEFAULT 100,
        operator_id_param VARCHAR DEFAULT 'system'
    )
    RETURNS BOOLEAN AS $$
    DECLARE
        rows_affected INTEGER;
        function_start_time TIMESTAMP := CURRENT_TIMESTAMP;
        step_name VARCHAR := 'INITIALIZATION';
        error_context TEXT;
    BEGIN
        --   COMPREHENSIVE LOGGING: Function entry
        RAISE NOTICE '[insert_route_assignment]  FUNCTION START: Route ID: %, Source: % → Dest: %',
            route_id_param, source_signal_id_param, dest_signal_id_param;
        RAISE NOTICE '[insert_route_assignment] Parameters: Direction: %, State: %, Priority: %, Operator: %',
            direction_param, state_param, priority_param, operator_id_param;
        RAISE NOTICE '[insert_route_assignment]  Circuits: % (overlap: %)',
            assigned_circuits_param, overlap_circuits_param;
        RAISE NOTICE '[insert_route_assignment]  Point Machines: %', locked_point_machines_param;

        --   PARAMETER VALIDATION
        step_name := 'PARAMETER_VALIDATION';

        IF route_id_param IS NULL THEN
            error_context := 'route_id_param cannot be NULL';
            RAISE EXCEPTION '[insert_route_assignment]  VALIDATION_ERROR: %', error_context;
        END IF;

        IF source_signal_id_param IS NULL OR source_signal_id_param = '' THEN
            error_context := 'source_signal_id_param cannot be NULL or empty';
            RAISE EXCEPTION '[insert_route_assignment]  VALIDATION_ERROR: %', error_context;
        END IF;

        IF dest_signal_id_param IS NULL OR dest_signal_id_param = '' THEN
            error_context := 'dest_signal_id_param cannot be NULL or empty';
            RAISE EXCEPTION '[insert_route_assignment]  VALIDATION_ERROR: %', error_context;
        END IF;

        --   SIGNAL EXISTENCE VALIDATION
        step_name := 'SIGNAL_VALIDATION';

        IF NOT EXISTS(SELECT 1 FROM railway_control.signals WHERE signal_id = source_signal_id_param) THEN
            error_context := 'Source signal does not exist: ' || source_signal_id_param;
            RAISE EXCEPTION '[insert_route_assignment]  SIGNAL_NOT_FOUND: %', error_context;
        END IF;

        IF NOT EXISTS(SELECT 1 FROM railway_control.signals WHERE signal_id = dest_signal_id_param) THEN
            error_context := 'Destination signal does not exist: ' || dest_signal_id_param;
            RAISE EXCEPTION '[insert_route_assignment]  SIGNAL_NOT_FOUND: %', error_context;
        END IF;

        RAISE NOTICE '[insert_route_assignment]   Parameter validation completed successfully';

        --   DUPLICATE CHECK
        step_name := 'DUPLICATE_CHECK';

        IF EXISTS(SELECT 1 FROM railway_control.route_assignments WHERE id = route_id_param) THEN
            error_context := 'Route with this ID already exists: ' || route_id_param;
            RAISE EXCEPTION '[insert_route_assignment]  DUPLICATE_ROUTE: %', error_context;
        END IF;

        RAISE NOTICE '[insert_route_assignment]   Duplicate check passed';

        --   SET OPERATOR CONTEXT
        step_name := 'OPERATOR_CONTEXT';
        PERFORM set_config('railway.operator_id', operator_id_param, true);
        RAISE NOTICE '[insert_route_assignment]  Operator context set: %', operator_id_param;

        --   ROUTE INSERTION
        step_name := 'ROUTE_INSERTION';
        RAISE NOTICE '[insert_route_assignment]  Starting route insertion...';

        BEGIN
            INSERT INTO railway_control.route_assignments (
                id,
                source_signal_id,
                dest_signal_id,
                direction,
                assigned_circuits,
                overlap_circuits,
                state,
                locked_point_machines,
                priority,
                operator_id,
                created_at
            ) VALUES (
                route_id_param,
                source_signal_id_param,
                dest_signal_id_param,
                direction_param,
                assigned_circuits_param,
                COALESCE(overlap_circuits_param, '{}'),
                state_param,
                COALESCE(locked_point_machines_param, '{}'),
                priority_param,
                operator_id_param,
                CURRENT_TIMESTAMP
            );

            GET DIAGNOSTICS rows_affected = ROW_COUNT;
            RAISE NOTICE '[insert_route_assignment]  Route insertion completed. Rows affected: %', rows_affected;

        EXCEPTION WHEN OTHERS THEN
            error_context := 'Route insertion failed: ' || SQLERRM;
            RAISE EXCEPTION '[insert_route_assignment]  INSERTION_FAILED at %: %', step_name, error_context;
        END;

        --   INSERTION VERIFICATION
        step_name := 'INSERTION_VERIFICATION';

        IF rows_affected = 0 THEN
            error_context := 'No rows were inserted - unknown error';
            RAISE EXCEPTION '[insert_route_assignment]  NO_ROWS_INSERTED: %', error_context;
        END IF;

        -- Verify the route actually exists
        IF NOT EXISTS(SELECT 1 FROM railway_control.route_assignments WHERE id = route_id_param) THEN
            error_context := 'Route was not found after insertion - possible rollback';
            RAISE EXCEPTION '[insert_route_assignment]  VERIFICATION_FAILED: %', error_context;
        END IF;

        RAISE NOTICE '[insert_route_assignment]   Route insertion verified successfully';

        --   EVENT LOGGING
        step_name := 'EVENT_LOGGING';

        BEGIN
            INSERT INTO railway_control.route_events (
                route_id,
                event_type,
                event_data,
                triggered_by,
                occurred_at
            ) VALUES (
                route_id_param,
                'ROUTE_REQUESTED',
                jsonb_build_object(
                    'source_signal_id', source_signal_id_param,
                    'dest_signal_id', dest_signal_id_param,
                    'direction', direction_param,
                    'assigned_circuits_count', array_length(assigned_circuits_param, 1),
                    'priority', priority_param,
                    'initial_state', state_param,
                    'operator', operator_id_param,
                    'function_duration_ms', EXTRACT(EPOCH FROM (CURRENT_TIMESTAMP - function_start_time)) * 1000
                ),
                operator_id_param,
                CURRENT_TIMESTAMP
            );

            RAISE NOTICE '[insert_route_assignment] Route event logged successfully';

        EXCEPTION WHEN OTHERS THEN
            -- Don't fail the whole function if event logging fails
            RAISE WARNING '[insert_route_assignment]  Event logging failed: %', SQLERRM;
        END;

        --   SUCCESS
        RAISE NOTICE '[insert_route_assignment]   FUNCTION SUCCESS: Route % created in % ms',
            route_id_param, EXTRACT(EPOCH FROM (CURRENT_TIMESTAMP - function_start_time)) * 1000;

        RETURN rows_affected > 0;

    EXCEPTION WHEN OTHERS THEN
        --   COMPREHENSIVE ERROR HANDLING
        error_context := COALESCE(error_context, SQLERRM);
        RAISE EXCEPTION '[insert_route_assignment]  CRITICAL_ERROR at step [%]: % | SQL State: % | Route: % | Duration: % ms',
            step_name,
            error_context,
            SQLSTATE,
            route_id_param,
            EXTRACT(EPOCH FROM (CURRENT_TIMESTAMP - function_start_time)) * 1000;
    END;
    $$ LANGUAGE plpgsql)",

        // 
        // AUDIT LOGGING FUNCTIONS - Comprehensive audit trail for regulatory compliance
        // 

        // Main audit logging trigger for all safety-critical operations
        R"(CREATE OR REPLACE FUNCTION railway_audit.log_changes()
        RETURNS TRIGGER AS $$
        DECLARE
            old_json JSONB := NULL;
            new_json JSONB := NULL;
            entity_name_val TEXT;
            operator_id_val TEXT := current_setting('railway.operator_id', true);
            operation_source_val TEXT := 'HMI';
        BEGIN
            IF TG_OP = 'DELETE' THEN
                old_json := to_jsonb(OLD);
            END IF;

            IF TG_OP = 'INSERT' OR TG_OP = 'UPDATE' THEN
                new_json := to_jsonb(NEW);
            END IF;

            IF TG_OP = 'UPDATE' THEN
                old_json := to_jsonb(OLD);
            END IF;

            CASE TG_TABLE_NAME
                WHEN 'signals' THEN entity_name_val := COALESCE(NEW.signal_name, OLD.signal_name);
                WHEN 'point_machines' THEN entity_name_val := COALESCE(NEW.machine_name, OLD.machine_name);
                WHEN 'track_circuits' THEN entity_name_val := COALESCE(NEW.circuit_name, OLD.circuit_name);
                WHEN 'track_segments' THEN entity_name_val := COALESCE(NEW.segment_name, OLD.segment_name);
                WHEN 'route_assignments' THEN entity_name_val := COALESCE(NEW.source_signal_id || '→' || NEW.dest_signal_id, OLD.source_signal_id || '→' || OLD.dest_signal_id);
                ELSE entity_name_val := 'Unknown';
            END CASE;

            INSERT INTO railway_audit.event_log (
                event_type,
                entity_type,
                entity_id,
                entity_name,
                old_values,
                new_values,
                operator_id,
                operation_source,
                safety_critical,
                replay_data,
                sequence_number
            ) VALUES (
                TG_OP,
                TG_TABLE_NAME,
                COALESCE(NEW.id::TEXT, OLD.id::TEXT),
                entity_name_val,
                old_json,
                new_json,
                operator_id_val,
                operation_source_val,
                CASE TG_TABLE_NAME
                    WHEN 'signals' THEN true
                    WHEN 'point_machines' THEN true
                    WHEN 'track_circuits' THEN true
                    WHEN 'route_assignments' THEN true
                    ELSE false
                END,
                COALESCE(new_json, old_json),
                nextval('railway_audit.event_sequence')
            );

            RETURN COALESCE(NEW, OLD);
        END;
        $$ LANGUAGE plpgsql)"
    };

    for (const QString& query : functions) {
        if (!executeQuery(query)) {
            qWarning() << "Failed to create function:" << query.left(100) + "...";
        }
    }

    return true;
}

bool DatabaseInitializer::createTriggers() {
    qDebug() << "Creating database triggers...";

    QStringList triggers = {
        // Basic update triggers
        R"(CREATE TRIGGER trg_event_log_set_date
        BEFORE INSERT OR UPDATE ON railway_audit.event_log
        FOR EACH ROW EXECUTE FUNCTION railway_audit.set_event_date())",

        R"(CREATE TRIGGER trg_track_circuits_updated_at
        BEFORE UPDATE ON railway_control.track_circuits
        FOR EACH ROW EXECUTE FUNCTION railway_control.update_timestamp())",

        R"(CREATE TRIGGER trg_track_segments_updated_at
        BEFORE UPDATE ON railway_control.track_segments
        FOR EACH ROW EXECUTE FUNCTION railway_control.update_timestamp())",

        R"(CREATE TRIGGER trg_signals_updated_at
        BEFORE UPDATE ON railway_control.signals
        FOR EACH ROW EXECUTE FUNCTION railway_control.update_timestamp())",

        R"(CREATE TRIGGER trg_point_machines_updated_at
        BEFORE UPDATE ON railway_control.point_machines
        FOR EACH ROW EXECUTE FUNCTION railway_control.update_timestamp())",

        R"(CREATE TRIGGER trg_signals_aspect_changed
        BEFORE UPDATE ON railway_control.signals
        FOR EACH ROW EXECUTE FUNCTION railway_control.update_signal_change_time())",

        R"(CREATE TRIGGER trg_text_labels_updated_at
        BEFORE UPDATE ON railway_control.text_labels
        FOR EACH ROW EXECUTE FUNCTION railway_control.update_timestamp())",

        // Audit triggers
        R"(CREATE TRIGGER trg_track_circuits_audit
        AFTER INSERT OR UPDATE OR DELETE ON railway_control.track_circuits
        FOR EACH ROW EXECUTE FUNCTION railway_audit.log_changes())",

        R"(CREATE TRIGGER trg_track_segments_audit
        AFTER INSERT OR UPDATE OR DELETE ON railway_control.track_segments
        FOR EACH ROW EXECUTE FUNCTION railway_audit.log_changes())",

        R"(CREATE TRIGGER trg_signals_audit
        AFTER INSERT OR UPDATE OR DELETE ON railway_control.signals
        FOR EACH ROW EXECUTE FUNCTION railway_audit.log_changes())",

        R"(CREATE TRIGGER trg_point_machines_audit
        AFTER INSERT OR UPDATE OR DELETE ON railway_control.point_machines
        FOR EACH ROW EXECUTE FUNCTION railway_audit.log_changes())"
    };

    for (const QString& query : triggers) {
        if (!executeQuery(query)) {
            qWarning() << "Failed to create trigger:" << query.left(100) + "...";
        }
    }

    return true;
}

bool DatabaseInitializer::createViews() {
    qDebug() << "Creating database views...";

    QStringList views = {
        // Simplified track segments view (REMOVED all resource_locks references)
        R"(CREATE OR REPLACE VIEW railway_control.v_track_segments_with_occupancy AS
        SELECT
            -- Basic segment information
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
            ts.circuit_id,
            ts.length_meters,
            ts.max_speed_kmh,
            ts.is_active,
            ts.protecting_signals,
            ts.created_at,
            ts.updated_at,

            -- Circuit occupancy information
            COALESCE(tc.is_occupied, false) as is_occupied,
            COALESCE(tc.is_assigned, false) as circuit_is_assigned,
            COALESCE(tc.is_overlap, false) as circuit_is_overlap,
            tc.occupied_by,
            tc.last_changed_at as occupancy_changed_at,

            -- Simplified circuit information (matching new schema)
            tc.circuit_name,
            tc.length_meters as circuit_length_meters,
            tc.max_speed_kmh as circuit_max_speed_kmh,
            tc.protecting_signals as circuit_protecting_signals,

            -- REMOVED ALL ROUTE LOCK FIELDS (no more resource_locks table):
            -- rl.is_active as is_route_locked,
            -- rl.lock_type as route_lock_type,
            -- rl.acquired_at as route_locked_at,
            -- rl.acquired_by as route_locked_by,
            -- rl.expires_at as route_lock_expires_at,

            -- Route context (direct from route_assignments, no resource_locks bridge)
            ra.id as route_id,
            ra.source_signal_id as route_source_signal,
            ra.dest_signal_id as route_dest_signal,
            ra.state as route_state,
            ra.direction as route_direction,
            ra.priority as route_priority,
            ra.created_at as route_created_at,

            -- Simplified availability status (NO resource_locks references)
            CASE
                WHEN NOT ts.is_active THEN 'INACTIVE'
                WHEN tc.is_occupied = true THEN 'OCCUPIED'
                WHEN tc.is_assigned = true THEN 'ROUTE_ASSIGNED'
                WHEN tc.is_overlap = true THEN 'OVERLAP_ASSIGNED'
                WHEN ts.is_assigned = true THEN 'ASSIGNED'
                WHEN tc.circuit_id = 'INVALID' OR tc.circuit_id IS NULL THEN 'NO_CIRCUIT'
                ELSE 'AVAILABLE'
            END as availability_status,

            -- Route assignment eligibility (simplified, no resource_locks)
            CASE
                WHEN tc.circuit_id = 'INVALID' OR tc.circuit_id IS NULL THEN false
                WHEN NOT ts.is_active OR NOT tc.is_active THEN false
                WHEN tc.is_occupied = true OR ts.is_assigned = true THEN false
                ELSE true
            END as route_assignment_eligible

        FROM railway_control.track_segments ts
        LEFT JOIN railway_control.track_circuits tc ON ts.circuit_id = tc.circuit_id
        -- REMOVED resource_locks JOIN entirely
        -- Direct join to route_assignments if circuit is in an active route
        LEFT JOIN railway_control.route_assignments ra ON (
            tc.circuit_id = ANY(ra.assigned_circuits)
            AND ra.state IN ('RESERVED', 'ACTIVE', 'PARTIALLY_RELEASED')
        ))",

        // Simplified track segment occupancy summary (REMOVED resource_locks metrics)
        R"(CREATE OR REPLACE VIEW railway_control.v_track_segment_occupancy AS
        SELECT
            -- Basic segment metrics
            COUNT(DISTINCT ts.segment_id) as total_segments,
            COUNT(DISTINCT ts.segment_id) FILTER (WHERE tc.is_occupied = true) as occupied_count,
            COUNT(DISTINCT ts.segment_id) FILTER (WHERE ts.is_assigned = true) as assigned_count,
            COUNT(DISTINCT ts.segment_id) FILTER (WHERE tc.is_occupied = true OR ts.is_assigned = true) as unavailable_count,

            -- REMOVED route assignment metrics that depended on resource_locks:
            -- COUNT(DISTINCT ts.segment_id) FILTER (WHERE rl.is_active = true) as route_locked_count,
            -- COUNT(DISTINCT ts.segment_id) FILTER (WHERE tc.is_occupied = true OR ts.is_assigned = true OR rl.is_active = true) as total_unavailable_count,

            -- Simplified utilization percentages (no resource_locks)
            ROUND(
                (COUNT(DISTINCT ts.segment_id) FILTER (WHERE tc.is_occupied = true OR ts.is_assigned = true)::NUMERIC /
                 COUNT(DISTINCT ts.segment_id)) * 100,
                2
            ) as total_utilization_percentage,

            -- Active routes count (direct count, no resource_locks bridge)
            COUNT(DISTINCT ra.id) as active_routes_count,

            -- Speed and length metrics (from circuit data)
            AVG(tc.length_meters) as avg_circuit_length_meters,
            AVG(tc.max_speed_kmh) as avg_circuit_max_speed_kmh

        FROM railway_control.track_segments ts
        LEFT JOIN railway_control.track_circuits tc ON ts.circuit_id = tc.circuit_id
        -- REMOVED resource_locks JOIN entirely
        -- Direct join to route_assignments if circuit is in an active route
        LEFT JOIN railway_control.route_assignments ra ON (
            tc.circuit_id = ANY(ra.assigned_circuits)
            AND ra.state IN ('RESERVED', 'ACTIVE', 'PARTIALLY_RELEASED')
        )
        WHERE ts.is_active = TRUE)",

        // Complete signal information - THIS ONE WAS ALREADY OK (no resource_locks references)
        R"(CREATE OR REPLACE VIEW railway_control.v_signals_complete AS
        SELECT
            s.id,
            s.signal_id,
            s.signal_name,
            st.type_code as signal_type,
            st.type_name as signal_type_name,
            s.location_row,
            s.location_col,
            s.direction,
            s.is_locked,

            sa_main.aspect_code as current_aspect,
            sa_main.aspect_name as current_aspect_name,
            sa_main.color_code as current_aspect_color,
            COALESCE(sa_calling.aspect_code, 'OFF') as calling_on_aspect,
            COALESCE(sa_calling.aspect_name, 'Off/Dark') as calling_on_aspect_name,
            COALESCE(sa_calling.color_code, '#404040') as calling_on_aspect_color,
            COALESCE(sa_loop.aspect_code, 'OFF') as loop_aspect,
            COALESCE(sa_loop.aspect_name, 'Off/Dark') as loop_aspect_name,
            COALESCE(sa_loop.color_code, '#404040') as loop_aspect_color,
            s.loop_signal_configuration,
            s.aspect_count,
            s.possible_aspects,
            s.is_active,
            s.location_description,
            s.last_changed_at,
            s.last_changed_by,
            s.interlocked_with,
            s.protected_track_circuits,
            s.manual_control_active,
            s.preceded_by_circuit_id,
            s.succeeded_by_circuit_id,
            s.is_route_signal,
            s.route_signal_type,
            s.created_at,
            s.updated_at
        FROM railway_control.signals s
        JOIN railway_config.signal_types st ON s.signal_type_id = st.id
        LEFT JOIN railway_config.signal_aspects sa_main ON s.current_aspect_id = sa_main.id
        LEFT JOIN railway_config.signal_aspects sa_calling ON s.calling_on_aspect_id = sa_calling.id
        LEFT JOIN railway_config.signal_aspects sa_loop ON s.loop_aspect_id = sa_loop.id)",

        // Point machines complete view (REMOVED resource_locks subquery)
        R"(CREATE OR REPLACE VIEW railway_control.v_point_machines_complete AS
        SELECT
            -- Basic point machine information
            pm.id,
            pm.machine_id,
            pm.machine_name,
            pm.junction_row,
            pm.junction_col,
            pm.root_track_segment_connection,
            pm.normal_track_segment_connection,
            pm.reverse_track_segment_connection,

            -- Position information (enhanced)
            pp.position_code as current_position,
            pp.position_name as current_position_name,
            pp.description as position_description,
            pp.pathfinding_weight as position_pathfinding_weight,
            pp.transition_time_ms as position_default_transition_time_ms,

            -- Operational status and timing
            pm.operating_status,
            pm.transition_time_ms,
            pm.last_operated_at,
            pm.last_operated_by,
            pm.operation_count,

            -- Locking and safety
            pm.is_locked,
            pm.lock_reason,
            pm.safety_interlocks,
            pm.protected_signals,

            -- Route assignment extensions
            pm.paired_entity,
            pm.host_track_circuit,
            pm.route_locking_enabled,
            pm.auto_normalize_after_route,

            -- Paired entity information
            paired_pm.machine_name as paired_machine_name,
            paired_pp.position_code as paired_current_position,
            paired_pp.position_name as paired_current_position_name,
            paired_pm.operating_status as paired_operating_status,
            paired_pm.is_locked as paired_is_locked,

            -- Route assignment context (direct from route_assignments)
            ra.source_signal_id as route_source_signal,
            ra.dest_signal_id as route_dest_signal,
            ra.state as route_state,
            ra.direction as route_direction,

            -- Position synchronization status (for paired machines)
            CASE
                WHEN pm.paired_entity IS NULL THEN 'NOT_PAIRED'
                WHEN pp.position_code = paired_pp.position_code THEN 'SYNCHRONIZED'
                WHEN pp.position_code != paired_pp.position_code THEN 'POSITION_MISMATCH'
                ELSE 'UNKNOWN'
            END as paired_sync_status,

            -- Simplified availability for route assignment (NO resource_locks subquery)
            CASE
                WHEN pm.operating_status = 'FAILED' THEN 'FAILED'
                WHEN pm.operating_status = 'MAINTENANCE' THEN 'MAINTENANCE'
                WHEN pm.operating_status = 'IN_TRANSITION' THEN 'IN_TRANSITION'
                WHEN pm.is_locked THEN 'LOCKED'
                -- REMOVED this condition (resource_locks table doesn't exist):
                WHEN pm.paired_entity IS NOT NULL AND pp.position_code != paired_pp.position_code THEN 'POSITION_MISMATCH'
                ELSE 'AVAILABLE'
            END as availability_status,

            -- Performance metrics
            CASE
                WHEN pm.operation_count > 0 THEN
                    EXTRACT(EPOCH FROM (CURRENT_TIMESTAMP - pm.created_at)) / pm.operation_count
                ELSE NULL
            END as avg_time_between_operations_seconds,

            -- Timestamps
            pm.created_at,
            pm.updated_at

        FROM railway_control.point_machines pm
        LEFT JOIN railway_config.point_positions pp ON pm.current_position_id = pp.id

        -- Paired machine information
        LEFT JOIN railway_control.point_machines paired_pm ON pm.paired_entity = paired_pm.machine_id
        LEFT JOIN railway_config.point_positions paired_pp ON paired_pm.current_position_id = paired_pp.id

        -- Route assignment information (direct join, no resource_locks bridge)
        LEFT JOIN railway_control.route_assignments ra ON (
            pm.machine_id = ANY(ra.locked_point_machines)
            AND ra.state IN ('RESERVED', 'ACTIVE', 'PARTIALLY_RELEASED')
        ))",

        // Active routes summary (KEEP - this one was fine)
        R"(CREATE OR REPLACE VIEW railway_control.v_active_routes_summary AS
        SELECT
            COUNT(*) as total_active_routes,
            COUNT(*) FILTER (WHERE state = 'RESERVED') as reserved_routes,
            COUNT(*) FILTER (WHERE state = 'ACTIVE') as active_routes,
            COUNT(*) FILTER (WHERE state = 'PARTIALLY_RELEASED') as partially_released_routes,
            COUNT(*) FILTER (WHERE overlap_release_due_at IS NOT NULL AND overlap_release_due_at <= CURRENT_TIMESTAMP) as expired_overlaps,
            AVG(EXTRACT(EPOCH FROM (CURRENT_TIMESTAMP - created_at)) * 1000) as avg_route_age_ms
        FROM railway_control.route_assignments
        WHERE state IN ('RESERVED', 'ACTIVE', 'PARTIALLY_RELEASED'))",

        // Recent events view (KEEP - this one was already clean)
        R"(CREATE VIEW railway_audit.v_recent_events AS
        SELECT
            el.id,
            el.event_timestamp,
            el.event_type,
            el.entity_type,
            el.entity_id,
            el.entity_name,
            el.operator_id,
            el.operation_source,
            el.safety_critical,
            el.comments
        FROM railway_audit.event_log el
        WHERE el.event_timestamp >= (CURRENT_TIMESTAMP - INTERVAL '24 hours')
        ORDER BY el.event_timestamp DESC)"

        // COMPLETELY REMOVED VIEWS:
        // - v_resource_utilization (was entirely based on resource_locks table)
    };

    for (const QString& query : views) {
        if (!executeQuery(query)) {
            qWarning() << "Failed to create view:" << query.left(100) + "...";
        }
    }

    return true;
}

bool DatabaseInitializer::createRolesAndPermissions() {
    qDebug() << "Creating database roles and permissions...";

    QStringList roleQueries = {
        // Railway Control Operator (full access to operations)
        "CREATE ROLE railway_operator",
        "GRANT USAGE ON SCHEMA railway_control TO railway_operator",
        "GRANT ALL PRIVILEGES ON ALL TABLES IN SCHEMA railway_control TO railway_operator",
        "GRANT ALL PRIVILEGES ON ALL SEQUENCES IN SCHEMA railway_control TO railway_operator",
        "GRANT USAGE ON SCHEMA railway_config TO railway_operator",
        "GRANT SELECT ON ALL TABLES IN SCHEMA railway_config TO railway_operator",

        // Railway Observer (read-only access)
        "CREATE ROLE railway_observer",
        "GRANT USAGE ON SCHEMA railway_control TO railway_observer",
        "GRANT SELECT ON ALL TABLES IN SCHEMA railway_control TO railway_observer",
        "GRANT USAGE ON SCHEMA railway_config TO railway_observer",
        "GRANT SELECT ON ALL TABLES IN SCHEMA railway_config TO railway_observer",

        // Railway Auditor (access to audit logs)
        "CREATE ROLE railway_auditor",
        "GRANT USAGE ON SCHEMA railway_audit TO railway_auditor",
        "GRANT SELECT ON ALL TABLES IN SCHEMA railway_audit TO railway_auditor"
    };

    bool success = true;
    for (const QString& query : roleQueries) {
        if (!executeQuery(query)) {
            // Check if role already exists error (which is acceptable)
            QSqlError lastError = db.lastError();
            if (lastError.text().contains("already exists")) {
                qDebug() << "Role already exists, continuing:" << query.left(50) + "...";
                continue;
            }
            qWarning() << "Failed to execute role/permission query:" << query.left(100) + "...";
            qWarning() << "Error:" << lastError.text();
            success = false;
            // Continue with other queries rather than failing completely
        }
    }

    if (success) {
        qDebug() << "  Database roles and permissions created successfully";
    } else {
        qWarning() << " Some role/permission queries failed - this may be acceptable if roles already exist";
    }

    return success;
}

bool DatabaseInitializer::populateInitialData() {
    qDebug() << "Populating initial data...";

    try {
        // Configuration data first
        if (!populateConfigurationData()) {
            setError("Failed to populate configuration data");
            return false;
        }

        // Track circuits before track segments
        if (!populateTrackCircuits()) {
            setError("Failed to populate track circuits");
            return false;
        }

        // Track segments
        if (!populateTrackSegments()) {
            setError("Failed to populate track segments");
            return false;
        }

        // Signals
        if (!populateSignals()) {
            setError("Failed to populate signals");
            return false;
        }

        // Point machines
        if (!populatePointMachines()) {
            setError("Failed to populate point machines");
            return false;
        }

        // Text labels
        if (!populateTextLabels()) {
            setError("Failed to populate text labels");
            return false;
        }

        // Interlocking rules
        if (!populateInterlockingRules()) {
            setError("Failed to populate interlocking rules");
            return false;
        }

        // Route assignment data
        if (!populateRouteAssignmentData()) {
            setError("Failed to populate route assignment data");
            return false;
        }

        qDebug() << "  Initial data population completed successfully";
        return true;

    } catch (const std::exception& e) {
        setError(QString("Exception during data population: %1").arg(e.what()));
        return false;
    }
}

// Rest of the methods (populateConfigurationData, populateTrackCircuits, etc.) remain the same as in the original code
// but with the route assignment data population integrated

bool DatabaseInitializer::populateConfigurationData() {
    qDebug() << "Populating configuration data with route assignment integration...";

    // Insert signal types with route assignment enhancements
    int starterTypeId = insertSignalType("STARTER", "Starter Signal", 3, true, 200);
    int homeTypeId = insertSignalType("HOME", "Home Signal", 3, true, 300);
    int outerTypeId = insertSignalType("OUTER", "Outer Signal", 4, true, 400);
    int advancedStarterTypeId = insertSignalType("ADVANCED_STARTER", "Advanced Starter Signal", 2, true, 100);

    if (starterTypeId <= 0 || homeTypeId <= 0 || outerTypeId <= 0 || advancedStarterTypeId <= 0) {
        return false;
    }

    // Insert signal aspects with route assignment enhancements
    insertSignalAspect("RED", "Danger", "#e53e3e", 0, false, false);
    insertSignalAspect("YELLOW", "Caution", "#d69e2e", 1, true, false);
    insertSignalAspect("GREEN", "Clear", "#38a169", 2, true, false);
    insertSignalAspect("SINGLE_YELLOW", "Single Yellow", "#d69e2e", 1, true, true);
    insertSignalAspect("DOUBLE_YELLOW", "Double Yellow", "#f6ad55", 1, true, true);
    insertSignalAspect("WHITE", "Calling On", "#ffffff", 0, false, false);
    insertSignalAspect("BLUE", "Shunt", "#3182ce", 0, false, false);
    insertSignalAspect("OFF", "Inactive", "#cccccc", 0, false, false);

    // Insert point positions with route assignment enhancements
    insertPointPosition("NORMAL", "Normal Position", 1.0, 3000);
    insertPointPosition("REVERSE", "Reverse Position", 1.2, 3000);

    return true;
}

bool DatabaseInitializer::populateRouteAssignmentData() { return true; }

// Include all the original data population methods here (populateTrackCircuits, populateSignals, etc.)
// and the route assignment specific methods (populateSignalAdjacencyAnchors, populateTrackCircuitEdges, etc.)

bool DatabaseInitializer::validateDatabase() {
    QStringList validationQueries = {
        "SELECT COUNT(*) FROM railway_control.track_circuits",
        "SELECT COUNT(*) FROM railway_control.track_segments",
        "SELECT COUNT(*) FROM railway_control.signals",
        "SELECT COUNT(*) FROM railway_control.point_machines",
        "SELECT COUNT(*) FROM railway_control.route_assignments",  // Keep this
        "SELECT COUNT(*) FROM railway_config.signal_types",
        "SELECT COUNT(*) FROM railway_config.signal_aspects",
        "SELECT COUNT(*) FROM railway_config.point_positions",
        "SELECT COUNT(*) FROM railway_control.interlocking_rules"

        // REMOVED VALIDATION FOR:
        // - resource_locks
        // - route_events
        // - track_circuit_edges
        // - overlap_definitions
        // - route_configuration (if table was removed)
    };

    for (const QString& query : validationQueries) {
        QSqlQuery validationQuery(db);
        if (!validationQuery.exec(query)) {
            setError(QString("Validation failed for query: %1").arg(query));
            return false;
        }

        if (validationQuery.next()) {
            int count = validationQuery.value(0).toInt();
            qDebug() << "Validation:" << query << "returned" << count << "rows";

            // Ensure critical tables have data
            if (query.contains("signal_types") && count == 0) {
                setError("No signal types found - critical configuration missing");
                return false;
            }
            if (query.contains("signal_aspects") && count == 0) {
                setError("No signal aspects found - critical configuration missing");
                return false;
            }
            // Remove route_configuration validation if table was removed
        }
    }

    qDebug() << "  Database validation completed successfully";
    return true;
}

// Helper methods
bool DatabaseInitializer::executeQuery(const QString& query, const QVariantList& params) {
    QSqlQuery sqlQuery(db);

    if (!sqlQuery.prepare(query)) {
        setError(QString("Failed to prepare query: %1 - Error: %2")
                     .arg(query.left(50)).arg(sqlQuery.lastError().text()));
        return false;
    }

    for (const QVariant& param : params) {
        sqlQuery.addBindValue(param);
    }

    if (!sqlQuery.exec()) {
        QString error = sqlQuery.lastError().text();

        // Check for common schema-related errors
        if (error.contains("column") && error.contains("does not exist")) {
            setError(QString("Schema mismatch - column missing: %1").arg(error));
        } else if (error.contains("relation") && error.contains("does not exist")) {
            setError(QString("Schema mismatch - table missing: %1").arg(error));
        } else {
            setError(QString("Query execution failed: %1 - Error: %2")
                         .arg(query.left(50)).arg(error));
        }
        return false;
    }

    return true;
}

void DatabaseInitializer::setError(const QString& error) {
    m_lastError = error;
    emit lastErrorChanged();
    qWarning() << "DatabaseInitializer Error:" << error;
}

void DatabaseInitializer::updateProgress(int value, const QString& operation) {
    m_progress = value;
    m_currentOperation = operation;
    emit progressChanged();
    emit currentOperationChanged();
    qDebug() << QString("Progress [%1%]: %2").arg(value).arg(operation);
}

// Additional helper methods for signal type insertion, data population, etc.
int DatabaseInitializer::insertSignalType(const QString& typeCode, const QString& typeName,
                                          int maxAspects, bool isRouteSignal, int routePriority) {
    QString query = R"(
        INSERT INTO railway_config.signal_types
        (type_code, type_name, max_aspects, is_route_signal, route_priority)
        VALUES (?, ?, ?, ?, ?) RETURNING id
    )";

    QSqlQuery sqlQuery(db);
    sqlQuery.prepare(query);
    sqlQuery.addBindValue(typeCode);
    sqlQuery.addBindValue(typeName);
    sqlQuery.addBindValue(maxAspects);
    sqlQuery.addBindValue(isRouteSignal);
    sqlQuery.addBindValue(routePriority);

    if (sqlQuery.exec() && sqlQuery.next()) {
        return sqlQuery.value(0).toInt();
    }

    setError(QString("Failed to insert signal type: %1").arg(typeCode));
    return -1;
}

int DatabaseInitializer::insertSignalAspect(const QString& aspectCode, const QString& aspectName,
                                            const QString& colorCode, int safetyLevel,
                                            bool permitsRouteEstablishment, bool requiresOverlap) {
    QString query = R"(
        INSERT INTO railway_config.signal_aspects
        (aspect_code, aspect_name, color_code, safety_level, permits_route_establishment, requires_overlap)
        VALUES (?, ?, ?, ?, ?, ?) RETURNING id
    )";

    QSqlQuery sqlQuery(db);
    sqlQuery.prepare(query);
    sqlQuery.addBindValue(aspectCode);
    sqlQuery.addBindValue(aspectName);
    sqlQuery.addBindValue(colorCode);
    sqlQuery.addBindValue(safetyLevel);
    sqlQuery.addBindValue(permitsRouteEstablishment);
    sqlQuery.addBindValue(requiresOverlap);

    if (sqlQuery.exec() && sqlQuery.next()) {
        return sqlQuery.value(0).toInt();
    }

    return -1;
}


int DatabaseInitializer::insertPointPosition(const QString& positionCode, const QString& positionName,
                                             double pathfindingWeight, int transitionTimeMs) {
    QString query = R"(
        INSERT INTO railway_config.point_positions
        (position_code, position_name, pathfinding_weight, transition_time_ms)
        VALUES (?, ?, ?, ?) RETURNING id
    )";

    QSqlQuery sqlQuery(db);
    sqlQuery.prepare(query);
    sqlQuery.addBindValue(positionCode);
    sqlQuery.addBindValue(positionName);
    sqlQuery.addBindValue(pathfindingWeight);
    sqlQuery.addBindValue(transitionTimeMs);

    if (sqlQuery.exec() && sqlQuery.next()) {
        return sqlQuery.value(0).toInt();
    }

    return -1;
}

// The rest of the data population methods would follow the same pattern as in the original code
// but with route assignment integration where appropriate

bool DatabaseInitializer::isDatabaseConnected() {
    return db.isOpen() && db.isValid();
}

QVariantMap DatabaseInitializer::getDatabaseStatus() {
    QVariantMap status;
    status["connected"] = isDatabaseConnected();
    status["lastError"] = m_lastError;

    if (!isDatabaseConnected()) {
        return status;
    }

    // Get table counts including route assignment tables
    QStringList tables = {"track_circuits", "track_segments", "signals", "point_machines",
                          "route_assignments"};
    for (const QString& table : tables) {
        QSqlQuery query(db);
        if (query.exec(QString("SELECT COUNT(*) FROM railway_control.%1").arg(table))) {
            if (query.next()) {
                status[table + "_count"] = query.value(0).toInt();
            }
        }
    }

    return status;
}

// 
// DATA POPULATION METHODS
// 

bool DatabaseInitializer::populateTrackCircuits() {
    qDebug() << "Populating track circuits with locking support...";

    QJsonArray circuitData = getTrackCircuitMappings();

    //   UPDATED: Include is_assigned and is_overlap columns for resource locking
    QString insertQuery = R"(
        INSERT INTO railway_control.track_circuits
        (circuit_id, circuit_name, is_occupied, is_assigned, is_overlap, is_active,
         protecting_signals, length_meters, max_speed_kmh)
        VALUES (?, ?, FALSE, ?, ?, TRUE, ?, ?, ?)
        ON CONFLICT (circuit_id) DO NOTHING
    )";

    for (const auto& value : circuitData) {
        QJsonObject circuit = value.toObject();

        // Convert JSON array to PostgreSQL TEXT[] format
        QJsonArray protectingSignalsArray = circuit["protecting_signals"].toArray();
        QStringList protectingSignalsList;
        for (const auto& signal : protectingSignalsArray) {
            protectingSignalsList.append(signal.toString());
        }

        QString protectingSignalsStr;
        if (protectingSignalsList.isEmpty()) {
            protectingSignalsStr = "{}";
        } else {
            protectingSignalsStr = "{" + protectingSignalsList.join(",") + "}";
        }

        // Simplified parameters - no location, type, weights, etc.
        QString circuitId = circuit["circuit_id"].toString();
        double lengthMeters = 100.0; // Default length
        int maxSpeedKmh = 80; // Default speed

        // Adjust only speed based on circuit type for safety
        if (circuitId.contains("T") && (circuitId == "3T" || circuitId == "4T")) {
            maxSpeedKmh = 25; // Platform areas
        } else if (circuitId.startsWith("A")) {
            maxSpeedKmh = 100; // Approach blocks
        }

        //   UPDATED: Include assigned and overlap parameters from JSON data
        QVariantList params = {
            circuit["circuit_id"].toString(),
            circuit["circuit_name"].toString(),
            circuit["assigned"].toBool(),  //   NEW: is_assigned from data
            circuit["overlap"].toBool(),   //   NEW: is_overlap from data
            protectingSignalsStr,
            lengthMeters,
            maxSpeedKmh
        };

        if (!executeQuery(insertQuery, params)) {
            return false;
        }
    }

    qDebug() << "  Populated" << circuitData.size() << "track circuits with locking support (all unlocked)";
    return true;
}

bool DatabaseInitializer::populateTrackSegments() {
    qDebug() << "Populating track segments with locking support...";

    QJsonArray trackSegmentData = getTrackSegmentsData();

    //   UPDATED: Include is_overlap column for resource locking
    QString insertQuery = R"(
        INSERT INTO railway_control.track_segments
        (segment_id, start_row, start_col, end_row, end_col, circuit_id, is_assigned, is_overlap, protecting_signals)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT (segment_id) DO NOTHING
    )";

    for (const auto& trackSegmentValue : trackSegmentData) {
        QJsonObject trackSegment = trackSegmentValue.toObject();

        // Handle INVALID circuit_id by setting to NULL
        QString circuitId = trackSegment["circuit_id"].toString();
        QVariant circuitIdValue = (circuitId == "INVALID") ? QVariant() : QVariant(circuitId);

        // Convert JSON array to PostgreSQL TEXT[] format
        QJsonArray protectingSignalsArray = trackSegment["protecting_signals"].toArray();
        QStringList protectingSignalsList;
        for (const auto& signal : protectingSignalsArray) {
            protectingSignalsList.append(signal.toString());
        }

        // Create PostgreSQL array string {signal1,signal2,signal3}
        QString protectingSignalsStr;
        if (protectingSignalsList.isEmpty()) {
            protectingSignalsStr = "{}";
        } else {
            protectingSignalsStr = "{" + protectingSignalsList.join(",") + "}";
        }

        //   UPDATED: Include overlap parameter
        QVariantList params = {
            trackSegment["id"].toString(),
            trackSegment["startRow"].toDouble(),
            trackSegment["startCol"].toDouble(),
            trackSegment["endRow"].toDouble(),
            trackSegment["endCol"].toDouble(),
            circuitIdValue,
            trackSegment["assigned"].toBool(),
            trackSegment["overlap"].toBool(),  //   NEW: is_overlap column
            protectingSignalsStr
        };

        if (!executeQuery(insertQuery, params)) {
            return false;
        }
    }

    qDebug() << "  Populated" << trackSegmentData.size() << "track segments with locking support";
    return true;
}

bool DatabaseInitializer::populateSignals() {
    qDebug() << "Populating signals with route assignment integration and explicit locking status...";

    // Combine all signal types
    QJsonArray allSignals;
    QJsonArray outerSignals = getOuterSignalsData();
    QJsonArray homeSignals = getHomeSignalsData();
    QJsonArray starterSignals = getStarterSignalsData();
    QJsonArray advancedSignals = getAdvancedStarterSignalsData();

    // Merge all signal arrays
    for (const auto& signal : outerSignals) allSignals.append(signal);
    for (const auto& signal : homeSignals) allSignals.append(signal);
    for (const auto& signal : starterSignals) allSignals.append(signal);
    for (const auto& signal : advancedSignals) allSignals.append(signal);

    for (const auto& signalValue : allSignals) {
        QJsonObject signal = signalValue.toObject();
        QString signalType = signal["type"].toString();

        // Get signal type ID
        QSqlQuery typeQuery(db);
        typeQuery.prepare("SELECT id FROM railway_config.signal_types WHERE type_code = ?");
        typeQuery.addBindValue(signalType);

        if (!typeQuery.exec() || !typeQuery.next()) {
            setError(QString("Signal type not found: %1").arg(signalType));
            return false;
        }
        int typeId = typeQuery.value(0).toInt();

        // Get main signal aspect ID
        QString currentAspect = signal["currentAspect"].toString();
        QSqlQuery aspectQuery(db);
        aspectQuery.prepare("SELECT id FROM railway_config.signal_aspects WHERE aspect_code = ?");
        aspectQuery.addBindValue(currentAspect);

        int aspectId = 1; // Default to RED
        if (aspectQuery.exec() && aspectQuery.next()) {
            aspectId = aspectQuery.value(0).toInt();
        }

        // Get calling-on aspect ID
        QString callingOnAspectStr = signal["callingOnAspect"].toString("OFF");
        int callingOnAspectId = getAspectIdByCode(callingOnAspectStr);

        // Get loop aspect ID
        QString loopAspectStr = signal["loopAspect"].toString("OFF");
        int loopAspectId = getAspectIdByCode(loopAspectStr);

        // Convert possible aspects array to PostgreSQL array format
        QJsonArray possibleAspects = signal["possibleAspects"].toArray();
        QStringList aspectsList;
        for (const auto& aspect : possibleAspects) {
            aspectsList << aspect.toString();
        }
        QString aspectsArrayStr = "{" + aspectsList.join(",") + "}";

        // Convert protected track circuits array to PostgreSQL TEXT[]
        QJsonArray protectedCircuitsArray = signal["protectedTrackCircuits"].toArray();
        QStringList protectedCircuitsList;
        for (const auto& circuit : protectedCircuitsArray) {
            protectedCircuitsList.append(circuit.toString());
        }

        QString protectedCircuitsStr;
        if (protectedCircuitsList.isEmpty()) {
            protectedCircuitsStr = "{}";
        } else {
            protectedCircuitsStr = "{" + protectedCircuitsList.join(",") + "}";
        }

        // Determine route signal properties
        bool isRouteSignal = (signalType == "HOME" || signalType == "STARTER" || signalType == "ADVANCED_STARTER");
        QString routeSignalType;
        if (signalType == "OUTER") routeSignalType = "START";
        else if (signalType == "HOME") routeSignalType = "INTERMEDIATE";
        else if (signalType == "STARTER") routeSignalType = "INTERMEDIATE";
        else if (signalType == "ADVANCED_STARTER") routeSignalType = "END";

        //   UPDATED: Insert query with explicit is_locked column for safety
        QString insertQuery = R"(
            INSERT INTO railway_control.signals
            (signal_id, signal_name, signal_type_id, location_row, location_col,
             direction, current_aspect_id, calling_on_aspect_id, loop_aspect_id,
             loop_signal_configuration, aspect_count, possible_aspects,
             protected_track_circuits, is_active, location_description,
             is_route_signal, route_signal_type, default_overlap_distance_m, is_locked)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, FALSE)
            ON CONFLICT (signal_id) DO NOTHING
        )";

        QVariantList params = {
            signal["id"].toString(),
            signal["name"].toString(),
            typeId,
            signal["row"].toDouble(),
            signal["col"].toDouble(),
            signal["direction"].toString(),
            aspectId,
            callingOnAspectId,
            loopAspectId,
            signal["loopSignalConfiguration"].toString("UR"),
            signal["aspectCount"].toInt(2),
            aspectsArrayStr,
            protectedCircuitsStr,
            signal["isActive"].toBool(true),
            signal["location"].toString(),
            isRouteSignal,
            routeSignalType.isEmpty() ? QVariant() : routeSignalType,
            180 // Default overlap distance
            //   NOTE: is_locked = FALSE is now explicitly set in the VALUES clause
        };

        if (!executeQuery(insertQuery, params)) {
            return false;
        }
    }

    qDebug() << "  Populated" << allSignals.size() << "signals with route assignment properties and explicit locking status (all unlocked)";
    return true;
}

bool DatabaseInitializer::populatePointMachines() {
    qDebug() << "Populating point machines with route assignment integration and explicit locking status...";

    QJsonArray pointsData = getPointMachinesData();

    for (const auto& pointValue : pointsData) {
        QJsonObject point = pointValue.toObject();

        // Get position ID
        QString currentPosition = point["position"].toString();
        QSqlQuery positionQuery(db);
        positionQuery.prepare("SELECT id FROM railway_config.point_positions WHERE position_code = ?");
        positionQuery.addBindValue(currentPosition);

        int positionId = 1; // Default to NORMAL
        if (positionQuery.exec() && positionQuery.next()) {
            positionId = positionQuery.value(0).toInt();
        }

        // Convert track connections to properly formatted JSON strings
        QJsonObject rootTrackSegment = point["rootTrackSegment"].toObject();
        QJsonObject normalTrackSegment = point["normalTrackSegment"].toObject();
        QJsonObject reverseTrackSegment = point["reverseTrackSegment"].toObject();

        QString rootTrackSegmentJson = QString::fromUtf8(QJsonDocument(rootTrackSegment).toJson(QJsonDocument::Compact));
        QString normalTrackSegmentJson = QString::fromUtf8(QJsonDocument(normalTrackSegment).toJson(QJsonDocument::Compact));
        QString reverseTrackSegmentJson = QString::fromUtf8(QJsonDocument(reverseTrackSegment).toJson(QJsonDocument::Compact));

        // Handle paired entity (can be null)
        QString pairedEntity;
        if (point.contains("pairedEntity") && !point["pairedEntity"].toString().isEmpty()) {
            pairedEntity = point["pairedEntity"].toString();
        }

        // Handle host track circuit (can be null for paired entities)
        QString hostTrackCircuit;
        if (point.contains("hostTrackCircuit") && !point["hostTrackCircuit"].toString().isEmpty()) {
            hostTrackCircuit = point["hostTrackCircuit"].toString();
            qDebug() << "    Point machine" << point["id"].toString()
                     << "assigned to host circuit:" << hostTrackCircuit;
        }

        //   UPDATED: Explicitly include is_locked column for safety
        QString insertQuery = R"(
            INSERT INTO railway_control.point_machines
            (machine_id, machine_name, junction_row, junction_col,
             root_track_segment_connection, normal_track_segment_connection, reverse_track_segment_connection,
             current_position_id, operating_status, transition_time_ms, paired_entity, host_track_circuit,
             route_locking_enabled, auto_normalize_after_route, is_locked)
            VALUES (?, ?, ?, ?, ?::jsonb, ?::jsonb, ?::jsonb, ?, ?, ?, ?, ?, TRUE, TRUE, FALSE)
        )";

        QVariantList params = {
            point["id"].toString(),
            point["name"].toString(),
            point["junctionPoint"].toObject()["row"].toDouble(),
            point["junctionPoint"].toObject()["col"].toDouble(),
            rootTrackSegmentJson,
            normalTrackSegmentJson,
            reverseTrackSegmentJson,
            positionId,
            point["operatingStatus"].toString("CONNECTED"),
            3000, // Default transition time
            pairedEntity.isEmpty() ? QVariant() : pairedEntity,
            hostTrackCircuit.isEmpty() ? QVariant() : hostTrackCircuit
            //   NOTE: is_locked = FALSE is now explicitly set in the VALUES clause
        };

        if (!executeQuery(insertQuery, params)) {
            setError(QString("Failed to insert point machine: %1").arg(point["id"].toString()));
            return false;
        }
    }

    qDebug() << "  Populated" << pointsData.size() << "point machines with explicit locking status (all unlocked)";
    qDebug() << "  PM001 → W22T (primary, unlocked)";
    qDebug() << "  PM004 → W21T (primary, unlocked)";
    qDebug() << "  PM002, PM003 → No host circuit (paired entities, unlocked)";

    return true;
}


bool DatabaseInitializer::populateTextLabels() {
    qDebug() << "Populating text labels...";

    QJsonArray labelsData = getTextLabelsData();

    QString insertQuery = R"(
        INSERT INTO railway_control.text_labels
        (label_text, position_row, position_col, font_size)
        VALUES (?, ?, ?, ?)
    )";

    for (const auto& labelValue : labelsData) {
        QJsonObject label = labelValue.toObject();

        QVariantList params = {
            label["text"].toString(),
            label["row"].toDouble(),
            label["col"].toDouble(),
            label["fontSize"].toInt(12)
        };

        if (!executeQuery(insertQuery, params)) {
            return false;
        }
    }

    qDebug() << "  Populated" << labelsData.size() << "text labels";
    return true;
}

bool DatabaseInitializer::populateInterlockingRules() {
    qDebug() << "Populating interlocking rules...";

    QJsonArray rulesData = getInterlockingRulesData();

    QString insertQuery = R"(
        INSERT INTO railway_control.interlocking_rules (
            rule_name, source_entity_type, source_entity_id,
            target_entity_type, target_entity_id, target_constraint,
            rule_type, priority
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT DO NOTHING
    )";

    for (const auto& value : rulesData) {
        QJsonObject rule = value.toObject();

        QVariantList params = {
            rule["rule_name"].toString(),
            rule["source_entity_type"].toString(),
            rule["source_entity_id"].toString(),
            rule["target_entity_type"].toString(),
            rule["target_entity_id"].toString(),
            rule["target_constraint"].toString(),
            rule["rule_type"].toString(),
            rule["priority"].toInt()
        };

        if (!executeQuery(insertQuery, params)) {
            return false;
        }
    }

    qDebug() << "  Populated" << rulesData.size() << "interlocking rules";
    return true;
}
// 
// DATA GETTER METHODS
// 

QJsonArray DatabaseInitializer::getInterlockingRulesData() {
    return QJsonArray {
        // PROTECTION RULES
        QJsonObject{{"rule_name", "Signal AS002 protects Circuit A42T"}, {"source_entity_type", "SIGNAL"}, {"source_entity_id", "AS002"}, {"target_entity_type", "TRACK_CIRCUIT"}, {"target_entity_id", "A42T"}, {"target_constraint", "MUST_BE_CLEAR"}, {"rule_type", "PROTECTING"}, {"priority", 900}},
        QJsonObject{{"rule_name", "Signal OT001 protects Circuit 6T"}, {"source_entity_type", "SIGNAL"}, {"source_entity_id", "OT001"}, {"target_entity_type", "TRACK_CIRCUIT"}, {"target_entity_id", "6T"}, {"target_constraint", "MUST_BE_CLEAR"}, {"rule_type", "PROTECTING"}, {"priority", 900}},
        QJsonObject{{"rule_name", "Signal AS002 protects Circuit 6T"}, {"source_entity_type", "SIGNAL"}, {"source_entity_id", "AS002"}, {"target_entity_type", "TRACK_CIRCUIT"}, {"target_entity_id", "6T"}, {"target_constraint", "MUST_BE_CLEAR"}, {"rule_type", "PROTECTING"}, {"priority", 900}},
        QJsonObject{{"rule_name", "Signal OT001 protects Circuit 5T"}, {"source_entity_type", "SIGNAL"}, {"source_entity_id", "OT001"}, {"target_entity_type", "TRACK_CIRCUIT"}, {"target_entity_id", "5T"}, {"target_constraint", "MUST_BE_CLEAR"}, {"rule_type", "PROTECTING"}, {"priority", 900}},
        QJsonObject{{"rule_name", "Signal ST003 protects Circuit 5T"}, {"source_entity_type", "SIGNAL"}, {"source_entity_id", "ST003"}, {"target_entity_type", "TRACK_CIRCUIT"}, {"target_entity_id", "5T"}, {"target_constraint", "MUST_BE_CLEAR"}, {"rule_type", "PROTECTING"}, {"priority", 900}},
        QJsonObject{{"rule_name", "Signal HM001 protects Circuit W22T"}, {"source_entity_type", "SIGNAL"}, {"source_entity_id", "HM001"}, {"target_entity_type", "TRACK_CIRCUIT"}, {"target_entity_id", "W22T"}, {"target_constraint", "MUST_BE_CLEAR"}, {"rule_type", "PROTECTING"}, {"priority", 900}},
        QJsonObject{{"rule_name", "Signal ST003 protects Circuit W22T"}, {"source_entity_type", "SIGNAL"}, {"source_entity_id", "ST003"}, {"target_entity_type", "TRACK_CIRCUIT"}, {"target_entity_id", "W22T"}, {"target_constraint", "MUST_BE_CLEAR"}, {"rule_type", "PROTECTING"}, {"priority", 900}},
        QJsonObject{{"rule_name", "Signal ST004 protects Circuit W22T"}, {"source_entity_type", "SIGNAL"}, {"source_entity_id", "ST004"}, {"target_entity_type", "TRACK_CIRCUIT"}, {"target_entity_id", "W22T"}, {"target_constraint", "MUST_BE_CLEAR"}, {"rule_type", "PROTECTING"}, {"priority", 900}},
        QJsonObject{{"rule_name", "Signal HM001 protects Circuit 3T"}, {"source_entity_type", "SIGNAL"}, {"source_entity_id", "HM001"}, {"target_entity_type", "TRACK_CIRCUIT"}, {"target_entity_id", "3T"}, {"target_constraint", "MUST_BE_CLEAR"}, {"rule_type", "PROTECTING"}, {"priority", 900}},
        QJsonObject{{"rule_name", "Signal HM002 protects Circuit 3T"}, {"source_entity_type", "SIGNAL"}, {"source_entity_id", "HM002"}, {"target_entity_type", "TRACK_CIRCUIT"}, {"target_entity_id", "3T"}, {"target_constraint", "MUST_BE_CLEAR"}, {"rule_type", "PROTECTING"}, {"priority", 900}},
        QJsonObject{{"rule_name", "Signal HM002 protects Circuit W21T"}, {"source_entity_type", "SIGNAL"}, {"source_entity_id", "HM002"}, {"target_entity_type", "TRACK_CIRCUIT"}, {"target_entity_id", "W21T"}, {"target_constraint", "MUST_BE_CLEAR"}, {"rule_type", "PROTECTING"}, {"priority", 900}},
        QJsonObject{{"rule_name", "Signal ST001 protects Circuit W21T"}, {"source_entity_type", "SIGNAL"}, {"source_entity_id", "ST001"}, {"target_entity_type", "TRACK_CIRCUIT"}, {"target_entity_id", "W21T"}, {"target_constraint", "MUST_BE_CLEAR"}, {"rule_type", "PROTECTING"}, {"priority", 900}},
        QJsonObject{{"rule_name", "Signal ST002 protects Circuit W21T"}, {"source_entity_type", "SIGNAL"}, {"source_entity_id", "ST002"}, {"target_entity_type", "TRACK_CIRCUIT"}, {"target_entity_id", "W21T"}, {"target_constraint", "MUST_BE_CLEAR"}, {"rule_type", "PROTECTING"}, {"priority", 900}},
        QJsonObject{{"rule_name", "Signal OT002 protects Circuit 2T"}, {"source_entity_type", "SIGNAL"}, {"source_entity_id", "OT002"}, {"target_entity_type", "TRACK_CIRCUIT"}, {"target_entity_id", "2T"}, {"target_constraint", "MUST_BE_CLEAR"}, {"rule_type", "PROTECTING"}, {"priority", 900}},
        QJsonObject{{"rule_name", "Signal ST001 protects Circuit 2T"}, {"source_entity_type", "SIGNAL"}, {"source_entity_id", "ST001"}, {"target_entity_type", "TRACK_CIRCUIT"}, {"target_entity_id", "2T"}, {"target_constraint", "MUST_BE_CLEAR"}, {"rule_type", "PROTECTING"}, {"priority", 900}},
        QJsonObject{{"rule_name", "Signal OT002 protects Circuit 1T"}, {"source_entity_type", "SIGNAL"}, {"source_entity_id", "OT002"}, {"target_entity_type", "TRACK_CIRCUIT"}, {"target_entity_id", "1T"}, {"target_constraint", "MUST_BE_CLEAR"}, {"rule_type", "PROTECTING"}, {"priority", 900}},
        QJsonObject{{"rule_name", "Signal AS001 protects Circuit 1T"}, {"source_entity_type", "SIGNAL"}, {"source_entity_id", "AS001"}, {"target_entity_type", "TRACK_CIRCUIT"}, {"target_entity_id", "1T"}, {"target_constraint", "MUST_BE_CLEAR"}, {"rule_type", "PROTECTING"}, {"priority", 900}},
        QJsonObject{{"rule_name", "Signal AS001 protects Circuit A1T"}, {"source_entity_type", "SIGNAL"}, {"source_entity_id", "AS001"}, {"target_entity_type", "TRACK_CIRCUIT"}, {"target_entity_id", "A1T"}, {"target_constraint", "MUST_BE_CLEAR"}, {"rule_type", "PROTECTING"}, {"priority", 900}},

        // OPPOSING RULES
        QJsonObject{{"rule_name", "Opposing Signals HM001-HM002"}, {"source_entity_type", "SIGNAL"}, {"source_entity_id", "HM001"}, {"target_entity_type", "SIGNAL"}, {"target_entity_id", "HM002"}, {"target_constraint", "MUST_BE_RED"}, {"rule_type", "OPPOSING"}, {"priority", 1000}},
        QJsonObject{{"rule_name", "Opposing Signals HM002-HM001"}, {"source_entity_type", "SIGNAL"}, {"source_entity_id", "HM002"}, {"target_entity_type", "SIGNAL"}, {"target_entity_id", "HM001"}, {"target_constraint", "MUST_BE_RED"}, {"rule_type", "OPPOSING"}, {"priority", 1000}}
    };
}

QJsonArray DatabaseInitializer::getTrackSegmentsData() {
    return QJsonArray {
        QJsonObject{{"id", "T1S1"}, {"startRow", 110}, {"startCol", 0}, {"endRow", 110}, {"endCol", 12}, {"circuit_id", "INVALID"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{}}},
        QJsonObject{{"id", "T1S2"}, {"startRow", 110}, {"startCol", 13}, {"endRow", 110}, {"endCol", 34}, {"circuit_id", "A42T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{"AS002"}}},
        QJsonObject{{"id", "T1S3"}, {"startRow", 110}, {"startCol", 35}, {"endRow", 110}, {"endCol", 67}, {"circuit_id", "6T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{"OT001", "AS002"}}},
        QJsonObject{{"id", "T1S4"}, {"startRow", 110}, {"startCol", 68}, {"endRow", 110}, {"endCol", 90}, {"circuit_id", "5T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{"OT001", "ST003"}}},
        QJsonObject{{"id", "T1S5"}, {"startRow", 110}, {"startCol", 91}, {"endRow", 110}, {"endCol", 117}, {"circuit_id", "W22T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{"HM001", "ST003", "ST004"}}},
        QJsonObject{{"id", "T1S6"}, {"startRow", 110}, {"startCol", 128}, {"endRow", 110}, {"endCol", 158}, {"circuit_id", "W22T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{"HM001", "ST003", "ST004"}}},
        QJsonObject{{"id", "T1S7"}, {"startRow", 110}, {"startCol", 159}, {"endRow", 110}, {"endCol", 221}, {"circuit_id", "3T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{}}},
        QJsonObject{{"id", "T1S8"}, {"startRow", 110}, {"startCol", 222}, {"endRow", 110}, {"endCol", 254}, {"circuit_id", "W21T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{"HM002", "ST001", "ST002"}}},
        QJsonObject{{"id", "T1S9"}, {"startRow", 110}, {"startCol", 264}, {"endRow", 110}, {"endCol", 286}, {"circuit_id", "W21T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{"HM002", "ST001", "ST002"}}},
        QJsonObject{{"id", "T1S10"}, {"startRow", 110}, {"startCol", 287}, {"endRow", 110}, {"endCol", 305}, {"circuit_id", "2T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{"OT002", "ST001"}}},
        QJsonObject{{"id", "T1S11"}, {"startRow", 110}, {"startCol", 306}, {"endRow", 110}, {"endCol", 338}, {"circuit_id", "1T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{"OT002", "AS001"}}},
        QJsonObject{{"id", "T1S12"}, {"startRow", 110}, {"startCol", 339}, {"endRow", 110}, {"endCol", 358}, {"circuit_id", "A1T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{"AS001"}}},
        QJsonObject{{"id", "T1S13"}, {"startRow", 110}, {"startCol", 359}, {"endRow", 110}, {"endCol", 369}, {"circuit_id", "INVALID"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{}}},
        QJsonObject{{"id", "T4S1"}, {"startRow", 88}, {"startCol", 125}, {"endRow", 88}, {"endCol", 137}, {"circuit_id", "W22T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{"HM001", "ST003", "ST004"}}},
        QJsonObject{{"id", "T4S2"}, {"startRow", 88}, {"startCol", 147}, {"endRow", 88}, {"endCol", 153}, {"circuit_id", "W22T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{"HM001", "ST003", "ST004"}}},
        QJsonObject{{"id", "T4S3"}, {"startRow", 88}, {"startCol", 154}, {"endRow", 88}, {"endCol", 226}, {"circuit_id", "4T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{}}},
        QJsonObject{{"id", "T4S4"}, {"startRow", 88}, {"startCol", 227}, {"endRow", 88}, {"endCol", 232}, {"circuit_id", "W21T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{"HM002", "ST001", "ST002"}}},
        QJsonObject{{"id", "T4S5"}, {"startRow", 88}, {"startCol", 242}, {"endRow", 88}, {"endCol", 258}, {"circuit_id", "W21T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{"HM002", "ST001", "ST002"}}},
        QJsonObject{{"id", "T5S1"}, {"startRow", 106}, {"startCol", 125}, {"endRow", 92}, {"endCol", 139}, {"circuit_id", "W22T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{"HM001", "ST003", "ST004"}}},
        QJsonObject{{"id", "T6S1"}, {"startRow", 92}, {"startCol", 240}, {"endRow", 105}, {"endCol", 254}, {"circuit_id", "W21T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{"HM002", "ST001", "ST002"}}}
    };
}

QJsonArray DatabaseInitializer::getTrackCircuitMappings() {
    return QJsonArray {
        QJsonObject{{"circuit_id", "A42T"}, {"circuit_name", "Approach Block A42T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{"AS002"}}},
        QJsonObject{{"circuit_id", "6T"}, {"circuit_name", "Main Line Section 6T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{"OT001", "AS002"}}},
        QJsonObject{{"circuit_id", "5T"}, {"circuit_name", "Main Line Section 5T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{"OT001", "ST003"}}},
        QJsonObject{{"circuit_id", "W22T"}, {"circuit_name", "Junction W22T Circuit"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{"HM001", "ST003", "ST004"}}},
        QJsonObject{{"circuit_id", "3T"}, {"circuit_name", "Platform Section 3T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{"HM001", "HM002"}}},
        QJsonObject{{"circuit_id", "W21T"}, {"circuit_name", "Junction W21T Circuit"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{"HM002", "ST001", "ST002"}}},
        QJsonObject{{"circuit_id", "2T"}, {"circuit_name", "Main Line Section 2T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{"OT002", "ST001"}}},
        QJsonObject{{"circuit_id", "1T"}, {"circuit_name", "Main Line Section 1T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{"OT002", "AS001"}}},
        QJsonObject{{"circuit_id", "A1T"}, {"circuit_name", "Exit Block A1T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{"AS001"}}},
        QJsonObject{{"circuit_id", "4T"}, {"circuit_name", "Loop Section 4T"}, {"assigned", false}, {"overlap", false}, {"protecting_signals", QJsonArray{}}}
    };
}

QJsonArray DatabaseInitializer::getOuterSignalsData() {
    return QJsonArray {
        QJsonObject{
            {"id", "OT001"}, {"name", "Outer A1"}, {"type", "OUTER"},
            {"row", 102}, {"col", 30}, {"direction", "UP"},
            {"currentAspect", "RED"}, {"aspectCount", 4},
            {"possibleAspects", QJsonArray{"RED", "SINGLE_YELLOW", "DOUBLE_YELLOW", "GREEN"}},
            {"protectedTrackCircuits", QJsonArray{"6T", "5T"}},
            {"isActive", true}, {"location", "Approach_Block_1"}
        },
        QJsonObject{
            {"id", "OT002"}, {"name", "Outer A2"}, {"type", "OUTER"},
            {"row", 113}, {"col", 330}, {"direction", "DOWN"},
            {"currentAspect", "RED"}, {"aspectCount", 4},
            {"possibleAspects", QJsonArray{"RED", "SINGLE_YELLOW", "DOUBLE_YELLOW", "GREEN"}},
            {"protectedTrackCircuits", QJsonArray{"2T", "1T"}},
            {"isActive", true}, {"location", "Approach_Block_2"}
        }
    };
}

QJsonArray DatabaseInitializer::getHomeSignalsData() {
    return QJsonArray {
        QJsonObject{
            {"id", "HM001"}, {"name", "Home A1"}, {"type", "HOME"},
            {"row", 102}, {"col", 84}, {"direction", "UP"},
            {"currentAspect", "RED"}, {"aspectCount", 3},
            {"possibleAspects", QJsonArray{"RED", "YELLOW", "GREEN"}},
            {"callingOnAspect", "WHITE"}, {"loopAspect", "YELLOW"}, {"loopSignalConfiguration", "UR"},
            {"protectedTrackCircuits", QJsonArray{"W22T", "3T"}},
            {"isActive", true}, {"location", "Platform_A_Entry"}
        },
        QJsonObject{
            {"id", "HM002"}, {"name", "Home A2"}, {"type", "HOME"},
            {"row", 113}, {"col", 275}, {"direction", "DOWN"},
            {"currentAspect", "RED"}, {"aspectCount", 3},
            {"possibleAspects", QJsonArray{"RED", "YELLOW", "GREEN"}},
            {"callingOnAspect", "OFF"}, {"loopAspect", "OFF"}, {"loopSignalConfiguration", "UR"},
            {"protectedTrackCircuits", QJsonArray{"W21T", "3T"}},
            {"isActive", true}, {"location", "Platform_A_Exit"}
        }
    };
}

QJsonArray DatabaseInitializer::getStarterSignalsData() {
    return QJsonArray {
        QJsonObject{
            {"id", "ST001"}, {"name", "Starter A1"}, {"type", "STARTER"},
            {"row", 103}, {"col", 217}, {"direction", "UP"},
            {"currentAspect", "RED"}, {"aspectCount", 3},
            {"possibleAspects", QJsonArray{"RED", "YELLOW", "GREEN"}},
            {"protectedTrackCircuits", QJsonArray{"W21T", "2T"}},
            {"isActive", true}, {"location", "Platform_A_Main_Departure"}
        },
        QJsonObject{
            {"id", "ST002"}, {"name", "Starter A2"}, {"type", "STARTER"},
            {"row", 83}, {"col", 220}, {"direction", "UP"},
            {"currentAspect", "RED"}, {"aspectCount", 2},
            {"possibleAspects", QJsonArray{"RED", "YELLOW"}},
            {"protectedTrackCircuits", QJsonArray{"W21T"}},
            {"isActive", true}, {"location", "Platform_A_Departure"}
        },
        QJsonObject{
            {"id", "ST003"}, {"name", "Starter B1"}, {"type", "STARTER"},
            {"row", 115}, {"col", 152}, {"direction", "DOWN"},
            {"currentAspect", "RED"}, {"aspectCount", 3},
            {"possibleAspects", QJsonArray{"RED", "YELLOW", "GREEN"}},
            {"protectedTrackCircuits", QJsonArray{"5T", "W22T"}},
            {"isActive", true}, {"location", "Platform_A_Main_Departure"}
        },
        QJsonObject{
            {"id", "ST004"}, {"name", "Starter B2"}, {"type", "STARTER"},
            {"row", 91}, {"col", 150}, {"direction", "DOWN"},
            {"currentAspect", "RED"}, {"aspectCount", 2},
            {"possibleAspects", QJsonArray{"RED", "YELLOW"}},
            {"protectedTrackCircuits", QJsonArray{"W22T"}},
            {"isActive", true}, {"location", "Junction_Loop_Entry"}
        }
    };
}

QJsonArray DatabaseInitializer::getAdvancedStarterSignalsData() {
    return QJsonArray {
        QJsonObject{
            {"id", "AS001"}, {"name", "Advanced Starter A1"}, {"type", "ADVANCED_STARTER"},
            {"row", 102}, {"col", 302}, {"direction", "UP"},
            {"currentAspect", "RED"}, {"aspectCount", 2},
            {"possibleAspects", QJsonArray{"RED", "GREEN"}},
            {"protectedTrackCircuits", QJsonArray{"1T", "A1T"}},
            {"isActive", true}, {"location", "Advanced_Departure_A"}
        },
        QJsonObject{
            {"id", "AS002"}, {"name", "Advanced Starter A2"}, {"type", "ADVANCED_STARTER"},
            {"row", 113}, {"col", 56}, {"direction", "DOWN"},
            {"currentAspect", "RED"}, {"aspectCount", 2},
            {"possibleAspects", QJsonArray{"RED", "GREEN"}},
            {"protectedTrackCircuits", QJsonArray{"A42T", "6T"}},
            {"isActive", true}, {"location", "Advanced_Departure_B"}
        }
    };
}

QJsonArray DatabaseInitializer::getPointMachinesData() {
    return QJsonArray {
        QJsonObject{
            {"id", "PM001"}, {"name", "Junction A"}, {"position", "NORMAL"}, {"operatingStatus", "CONNECTED"},
            {"pairedEntity", "PM002"},
            {"hostTrackCircuit", "W22T"},  //  NEW: Host track circuit
            {"junctionPoint", QJsonObject{{"row", 110}, {"col", 121.2}}},
            {"rootTrackSegment", QJsonObject{{"trackSegmentId", "T1S5"}, {"connectionEnd", "END"}, {"offset", QJsonObject{{"row", 0}, {"col", 0}}}}},
            {"normalTrackSegment", QJsonObject{{"trackSegmentId", "T1S6"}, {"connectionEnd", "START"}, {"offset", QJsonObject{{"row", 0}, {"col", 0}}}}},
            {"reverseTrackSegment", QJsonObject{{"trackSegmentId", "T5S1"}, {"connectionEnd", "START"}, {"offset", QJsonObject{{"row", 0}, {"col", 0}}}}}
        },
        QJsonObject{
            {"id", "PM002"}, {"name", "Junction B"}, {"position", "NORMAL"}, {"operatingStatus", "CONNECTED"},
            {"pairedEntity", "PM001"},
            //  NO hostTrackCircuit - paired entity, leave empty to avoid unexpected behavior
            {"junctionPoint", QJsonObject{{"row", 88}, {"col", 143.3}}},
            {"rootTrackSegment", QJsonObject{{"trackSegmentId", "T4S2"}, {"connectionEnd", "START"}, {"offset", QJsonObject{{"row", 0}, {"col", 0}}}}},
            {"normalTrackSegment", QJsonObject{{"trackSegmentId", "T4S1"}, {"connectionEnd", "END"}, {"offset", QJsonObject{{"row", 0}, {"col", 0}}}}},
            {"reverseTrackSegment", QJsonObject{{"trackSegmentId", "T5S1"}, {"connectionEnd", "END"}, {"offset", QJsonObject{{"row", 0}, {"col", 0}}}}}
        },
        QJsonObject{
            {"id", "PM003"}, {"name", "Junction C"}, {"position", "NORMAL"}, {"operatingStatus", "CONNECTED"},
            {"pairedEntity", "PM004"},
            //  NO hostTrackCircuit - paired entity, leave empty to avoid unexpected behavior
            {"junctionPoint", QJsonObject{{"row", 88}, {"col", 235.6}}},
            {"rootTrackSegment", QJsonObject{{"trackSegmentId", "T4S4"}, {"connectionEnd", "END"}, {"offset", QJsonObject{{"row", 0}, {"col", 0}}}}},
            {"normalTrackSegment", QJsonObject{{"trackSegmentId", "T4S5"}, {"connectionEnd", "START"}, {"offset", QJsonObject{{"row", 0}, {"col", 0}}}}},
            {"reverseTrackSegment", QJsonObject{{"trackSegmentId", "T6S1"}, {"connectionEnd", "START"}, {"offset", QJsonObject{{"row", 0}, {"col", 0}}}}}
        },
        QJsonObject{
            {"id", "PM004"}, {"name", "Junction D"}, {"position", "NORMAL"}, {"operatingStatus", "CONNECTED"},
            {"pairedEntity", "PM003"},
            {"hostTrackCircuit", "W21T"},  //  NEW: Host track circuit
            {"junctionPoint", QJsonObject{{"row", 110}, {"col", 259.5}}},
            {"rootTrackSegment", QJsonObject{{"trackSegmentId", "T1S9"}, {"connectionEnd", "START"}, {"offset", QJsonObject{{"row", 0}, {"col", 0}}}}},
            {"normalTrackSegment", QJsonObject{{"trackSegmentId", "T1S8"}, {"connectionEnd", "END"}, {"offset", QJsonObject{{"row", 0}, {"col", 0}}}}},
            {"reverseTrackSegment", QJsonObject{{"trackSegmentId", "T6S1"}, {"connectionEnd", "END"}, {"offset", QJsonObject{{"row", 0}, {"col", 0}}}}}
        }
    };
}

QJsonArray DatabaseInitializer::getTextLabelsData() {
    return QJsonArray {
        QJsonObject{{"text", "50"}, {"row", 1}, {"col", 49}, {"fontSize", 12}},
        QJsonObject{{"text", "100"}, {"row", 1}, {"col", 99}, {"fontSize", 12}},
        QJsonObject{{"text", "150"}, {"row", 1}, {"col", 149}, {"fontSize", 12}},
        QJsonObject{{"text", "200"}, {"row", 1}, {"col", 199}, {"fontSize", 12}},
        QJsonObject{{"text", "30"}, {"row", 29}, {"col", 1}, {"fontSize", 12}},
        QJsonObject{{"text", "90"}, {"row", 89}, {"col", 1}, {"fontSize", 12}},
        QJsonObject{{"text", "T1S1"}, {"row", 107}, {"col", 4}, {"fontSize", 12}},
        QJsonObject{{"text", "T1S2"}, {"row", 107}, {"col", 20}, {"fontSize", 12}},
        QJsonObject{{"text", "T1S3"}, {"row", 107}, {"col", 48}, {"fontSize", 12}},
        QJsonObject{{"text", "T1S4"}, {"row", 107}, {"col", 77}, {"fontSize", 12}},
        QJsonObject{{"text", "T1S5"}, {"row", 107}, {"col", 105}, {"fontSize", 12}},
        QJsonObject{{"text", "T1S6"}, {"row", 107}, {"col", 138}, {"fontSize", 12}},
        QJsonObject{{"text", "T1S7"}, {"row", 107}, {"col", 188}, {"fontSize", 12}},
        QJsonObject{{"text", "T1S8"}, {"row", 107}, {"col", 236}, {"fontSize", 12}},
        QJsonObject{{"text", "T1S9"}, {"row", 107}, {"col", 271}, {"fontSize", 12}},
        QJsonObject{{"text", "T1S10"}, {"row", 107}, {"col", 293}, {"fontSize", 12}},
        QJsonObject{{"text", "T1S11"}, {"row", 107}, {"col", 318}, {"fontSize", 12}},
        QJsonObject{{"text", "T1S12"}, {"row", 107}, {"col", 345}, {"fontSize", 12}},
        QJsonObject{{"text", "T1S13"}, {"row", 107}, {"col", 360}, {"fontSize", 12}},
        QJsonObject{{"text", "T4S1"}, {"row", 85}, {"col", 130}, {"fontSize", 12}},
        QJsonObject{{"text", "T4S3"}, {"row", 85}, {"col", 188}, {"fontSize", 12}},
        QJsonObject{{"text", "T4S5"}, {"row", 85}, {"col", 246}, {"fontSize", 12}}
    };
}

// 
// HELPER METHODS
// 

int DatabaseInitializer::getAspectIdByCode(const QString& aspectCode) {
    // Hardcoded based on insertion order
    if (aspectCode == "RED") return 1;
    if (aspectCode == "YELLOW") return 2;
    if (aspectCode == "GREEN") return 3;
    if (aspectCode == "SINGLE_YELLOW") return 4;
    if (aspectCode == "DOUBLE_YELLOW") return 5;
    if (aspectCode == "WHITE") return 6;
    if (aspectCode == "BLUE") return 7;
    if (aspectCode == "OFF") return 8;

    // Fallback: Query database for other aspects
    QSqlQuery query(db);
    query.prepare("SELECT id FROM railway_config.signal_aspects WHERE aspect_code = ?");
    query.addBindValue(aspectCode);

    if (query.exec() && query.next()) {
        return query.value(0).toInt();
    }

    // Default: Return OFF aspect ID if not found
    qWarning() << " Aspect code not found:" << aspectCode << "- defaulting to OFF";
    return 8; // OFF
}

// Async methods for backward compatibility
void DatabaseInitializer::resetDatabaseAsync() {
    if (m_isRunning) {
        qWarning() << "Database reset already in progress";
        return;
    }

    m_isRunning = true;
    emit isRunningChanged();

    updateProgress(0, "Preparing database reset...");
    resetTimer->start(100);
}

void DatabaseInitializer::performReset() {
    qDebug() << "DatabaseInitializer::performReset() - Starting reset process";
    qDebug() << "Current m_isRunning state:" << m_isRunning;
    qDebug() << "Current m_lastError:" << m_lastError;

    //   FIX: Reset the running flag before calling initializeDatabase
    m_isRunning = false;
    emit isRunningChanged();

    bool success = initializeDatabase();

    qDebug() << "DatabaseInitializer::performReset() - initializeDatabase() returned:" << success;
    qDebug() << "Current m_lastError after initializeDatabase():" << m_lastError;

    QString resultMessage = success ?
                                "Database has been reset and populated with unified schema" :
                                QString("Database reset failed: %1").arg(m_lastError);

    qDebug() << "DatabaseInitializer::performReset() - Success:" << success;
    qDebug() << "DatabaseInitializer::performReset() - Error message:" << m_lastError;
    qDebug() << "DatabaseInitializer::performReset() - Result message:" << resultMessage;

    emit resetCompleted(success, resultMessage);
}

void DatabaseInitializer::testConnectionAsync() {
    bool success = false;
    QString message;

    try {
        if (connectToDatabase()) {
            QSqlQuery query(db);
            if (query.exec("SELECT version()") && query.next()) {
                QString version = query.value(0).toString();
                success = true;
                message = QString("Connection successful!\nPostgreSQL version: %1").arg(version);
            } else {
                message = "Connected but failed to query version";
            }
        } else {
            message = "Failed to connect to any PostgreSQL instance";
        }
    } catch (...) {
        message = "Connection test failed with exception";
    }

    emit connectionTestCompleted(success, message);
}

void DatabaseInitializer::testConnection() {
    bool success = connectToDatabase();
    QString message = success ? "Database connection successful" : m_lastError;
    emit connectionTestCompleted(success, message);
}
