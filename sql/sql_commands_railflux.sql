-- ============================================================================
-- SIMPLE CLEANUP APPROACH
-- ============================================================================

-- Drop existing schemas in dependency order (CASCADE removes all dependent objects)
DROP SCHEMA IF EXISTS railway_control CASCADE;
DROP SCHEMA IF EXISTS railway_audit CASCADE;
DROP SCHEMA IF EXISTS railway_config CASCADE;

-- Drop any existing sequences that might persist
DROP SEQUENCE IF EXISTS railway_audit.event_sequence CASCADE;

-- Force drop roles with CASCADE (removes all dependencies)
DROP ROLE IF EXISTS railway_operator;
DROP ROLE IF EXISTS railway_observer;
DROP ROLE IF EXISTS railway_auditor;

-- Create schemas for organization
CREATE SCHEMA railway_control;
CREATE SCHEMA railway_audit;
CREATE SCHEMA railway_config;

-- Set search path
SET search_path TO railway_control, railway_audit, railway_config, public;

-- ============================================================================
-- CONFIGURATION TABLES (With Route Assignment Integration)
-- ============================================================================

CREATE TABLE railway_config.signal_types (
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
);

CREATE TABLE railway_config.signal_aspects (
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
);

CREATE TABLE railway_config.point_positions (
    id SERIAL PRIMARY KEY,
    position_code VARCHAR(20) NOT NULL UNIQUE,
    position_name VARCHAR(50) NOT NULL,
    description TEXT,
    -- Route assignment extensions
    pathfinding_weight NUMERIC DEFAULT 1.0,
    transition_time_ms INTEGER DEFAULT 3000,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

-- ============================================================================
-- CORE RAILWAY INFRASTRUCTURE TABLES (With Route Assignment Integration)
-- ============================================================================

-- Track circuits with route assignment enhancements
CREATE TABLE railway_control.track_circuits (
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
);

-- Track segments with circuit references
CREATE TABLE railway_control.track_segments (
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
);

-- Signals with route assignment anchors and properties
CREATE TABLE railway_control.signals (
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
);

-- Point machines with route assignment integration
CREATE TABLE railway_control.point_machines (
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
);

-- Text labels
CREATE TABLE railway_control.text_labels (
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
);

-- System state
CREATE TABLE railway_control.system_state (
    id SERIAL PRIMARY KEY,
    state_key VARCHAR(100) NOT NULL UNIQUE,
    state_value JSONB NOT NULL,
    description TEXT,
    last_updated TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_by VARCHAR(100)
);

-- Interlocking rules
CREATE TABLE railway_control.interlocking_rules (
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
);

-- Route assignments - main state tracking
CREATE TABLE railway_control.route_assignments (
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
);

-- ============================================================================
-- AUDIT AND EVENT LOGGING SYSTEM
-- ============================================================================

CREATE TABLE railway_audit.event_log (
    id BIGSERIAL PRIMARY KEY,
    event_timestamp TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    event_type VARCHAR(50) NOT NULL,
    entity_type VARCHAR(50) NOT NULL, -- SIGNAL, POINT_MACHINE, TRACK_SEGMENT, TRACK_CIRCUIT
    entity_id VARCHAR(50) NOT NULL,
    entity_name VARCHAR(100),
    event_details JSONB;

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
);

CREATE TABLE railway_audit.system_events (
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
);

-- Create sequence for event ordering
CREATE SEQUENCE railway_audit.event_sequence;

-- ============================================================================
-- INDEXES FOR PERFORMANCE AND SAFETY
-- ============================================================================

-- Track circuits indexes
CREATE INDEX idx_track_circuits_id ON railway_control.track_circuits(circuit_id);
CREATE INDEX idx_track_circuits_occupied ON railway_control.track_circuits(is_occupied) WHERE is_occupied = TRUE;
CREATE INDEX idx_track_circuits_active ON railway_control.track_circuits(is_active) WHERE is_active = TRUE;
CREATE INDEX idx_track_circuits_assigned ON railway_control.track_circuits(is_assigned) WHERE is_assigned = TRUE;
CREATE INDEX idx_track_circuits_overlap ON railway_control.track_circuits(is_overlap) WHERE is_overlap = TRUE;

-- Track segments indexes
CREATE INDEX idx_track_segments_id ON railway_control.track_segments(segment_id);
CREATE INDEX idx_track_segments_circuit ON railway_control.track_segments(circuit_id);
CREATE INDEX idx_track_segments_location ON railway_control.track_segments USING btree(start_row, start_col, end_row, end_col);
CREATE INDEX idx_track_segments_assigned ON railway_control.track_segments(is_assigned) WHERE is_assigned = TRUE;
CREATE INDEX idx_track_segments_overlap ON railway_control.track_segments(is_overlap) WHERE is_overlap = TRUE;

-- Signal indexes (including route assignment)
CREATE INDEX idx_signals_id ON railway_control.signals(signal_id);
CREATE INDEX idx_signals_location ON railway_control.signals USING btree(location_row, location_col);
CREATE INDEX idx_signals_type ON railway_control.signals(signal_type_id);
CREATE INDEX idx_signals_active ON railway_control.signals(is_active) WHERE is_active = TRUE;
CREATE INDEX idx_signals_preceded_by ON railway_control.signals(preceded_by_circuit_id) WHERE preceded_by_circuit_id IS NOT NULL;
CREATE INDEX idx_signals_succeeded_by ON railway_control.signals(succeeded_by_circuit_id) WHERE succeeded_by_circuit_id IS NOT NULL;
CREATE INDEX idx_signals_locked ON railway_control.signals(is_locked) WHERE is_locked = TRUE;

-- Point machine indexes
CREATE INDEX idx_point_machines_id ON railway_control.point_machines(machine_id);
CREATE INDEX idx_point_machines_position ON railway_control.point_machines(current_position_id);
CREATE INDEX idx_point_machines_junction ON railway_control.point_machines USING btree(junction_row, junction_col);
CREATE INDEX idx_point_machines_paired_entity ON railway_control.point_machines(paired_entity) WHERE paired_entity IS NOT NULL;
CREATE INDEX idx_point_machines_host_track_circuit ON railway_control.point_machines(host_track_circuit);

-- Route assignment indexes
CREATE INDEX idx_track_circuit_edges_from ON railway_control.track_circuit_edges(from_circuit_id) WHERE is_active = TRUE;
CREATE INDEX idx_track_circuit_edges_to ON railway_control.track_circuit_edges(to_circuit_id) WHERE is_active = TRUE;
CREATE INDEX idx_track_circuit_edges_condition ON railway_control.track_circuit_edges(condition_point_machine_id) WHERE condition_point_machine_id IS NOT NULL;

CREATE INDEX idx_route_assignments_state ON railway_control.route_assignments(state);
CREATE INDEX idx_route_assignments_active ON railway_control.route_assignments(state) WHERE state IN ('RESERVED', 'ACTIVE');
CREATE INDEX idx_route_assignments_signals ON railway_control.route_assignments(source_signal_id, dest_signal_id);
CREATE INDEX idx_route_assignments_created ON railway_control.route_assignments(created_at);

CREATE UNIQUE INDEX idx_resource_locks_unique_active ON railway_control.resource_locks(resource_type, resource_id) WHERE is_active = TRUE;
CREATE INDEX idx_resource_locks_route_active ON railway_control.resource_locks(route_id, is_active) WHERE is_active = TRUE;
CREATE INDEX idx_resource_locks_route ON railway_control.resource_locks(route_id);
CREATE INDEX idx_resource_locks_expires_active ON railway_control.resource_locks(expires_at, is_active) WHERE expires_at IS NOT NULL AND is_active = TRUE;
CREATE INDEX idx_resource_locks_conflict_check ON railway_control.resource_locks(resource_type, resource_id, lock_type, is_active);
CREATE INDEX idx_resource_locks_released_at ON railway_control.resource_locks(released_at) WHERE released_at IS NOT NULL;
CREATE INDEX idx_resource_locks_released_by ON railway_control.resource_locks(released_by, released_at) WHERE released_by IS NOT NULL;
CREATE INDEX idx_resource_locks_acquired_by ON railway_control.resource_locks(acquired_by, acquired_at);
CREATE INDEX idx_resource_locks_lock_type ON railway_control.resource_locks(lock_type, is_active) WHERE is_active = TRUE;
CREATE INDEX idx_resource_locks_duration_analysis ON railway_control.resource_locks(acquired_at, released_at, lock_type) WHERE released_at IS NOT NULL;

CREATE INDEX idx_route_events_route_time ON railway_control.route_events(route_id, occurred_at);
CREATE INDEX idx_route_events_sequence ON railway_control.route_events(sequence_number);

-- Audit indexes
CREATE INDEX idx_event_log_timestamp ON railway_audit.event_log(event_timestamp);
CREATE INDEX idx_event_log_entity ON railway_audit.event_log(entity_type, entity_id);
CREATE INDEX idx_event_log_operator ON railway_audit.event_log(operator_id);
CREATE INDEX idx_event_log_safety ON railway_audit.event_log(safety_critical) WHERE safety_critical = TRUE;
CREATE INDEX idx_event_log_sequence ON railway_audit.event_log(sequence_number);
CREATE INDEX idx_event_log_date ON railway_audit.event_log(event_date);

-- GIN indexes for array and JSONB columns
CREATE INDEX idx_signals_possible_aspects ON railway_control.signals USING gin(possible_aspects);
CREATE INDEX idx_signals_protected_circuits ON railway_control.signals USING gin(protected_track_circuits);
CREATE INDEX idx_track_circuits_protecting_signals ON railway_control.track_circuits USING gin(protecting_signals);
CREATE INDEX idx_point_machines_safety_interlocks ON railway_control.point_machines USING gin(safety_interlocks);
CREATE INDEX idx_event_log_old_values ON railway_audit.event_log USING gin(old_values);
CREATE INDEX idx_event_log_new_values ON railway_audit.event_log USING gin(new_values);
CREATE INDEX idx_route_assignments_circuits ON railway_control.route_assignments USING gin(assigned_circuits);
CREATE INDEX idx_route_assignments_overlap ON railway_control.route_assignments USING gin(overlap_circuits);

-- ============================================================================
-- FUNCTIONS
-- ============================================================================

-- Basic utility functions
CREATE OR REPLACE FUNCTION railway_audit.set_event_date()
RETURNS TRIGGER AS $$
BEGIN
    NEW.event_date := NEW.event_timestamp::DATE;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION railway_control.update_timestamp()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = CURRENT_TIMESTAMP;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION railway_control.update_signal_change_time()
RETURNS TRIGGER AS $$
BEGIN
    IF OLD.current_aspect_id IS DISTINCT FROM NEW.current_aspect_id THEN
        NEW.last_changed_at = CURRENT_TIMESTAMP;
    END IF;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION railway_config.get_aspect_id(aspect_code_param VARCHAR)
RETURNS INTEGER AS $$
DECLARE
    aspect_id_result INTEGER;
BEGIN
    SELECT id INTO aspect_id_result
    FROM railway_config.signal_aspects
    WHERE aspect_code = aspect_code_param;
    RETURN aspect_id_result;
END;
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION railway_config.get_position_id(position_code_param VARCHAR)
RETURNS INTEGER AS $$
DECLARE
    position_id_result INTEGER;
BEGIN
    SELECT id INTO position_id_result
    FROM railway_config.point_positions
    WHERE position_code = position_code_param;
    RETURN position_id_result;
END;
$$ LANGUAGE plpgsql;

-- Function to safely update signal aspect with validation
CREATE OR REPLACE FUNCTION railway_control.update_signal_aspect(
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

    -- Check if signal is locked by route assignment
    SELECT EXISTS(
        SELECT 1 FROM railway_control.resource_locks rl
        WHERE rl.resource_type = 'SIGNAL'
        AND rl.resource_id = signal_id_param
        AND rl.is_active = TRUE
    ) INTO route_locked;

    IF route_locked THEN
        RAISE EXCEPTION 'Signal % is locked by route assignment', signal_id_param;
    END IF;

    -- Update signal aspect
    UPDATE railway_control.signals
    SET current_aspect_id = aspect_id_val,
        last_changed_by = operator_id_param
    WHERE signal_id = signal_id_param;

    GET DIAGNOSTICS rows_affected = ROW_COUNT;
    RETURN rows_affected > 0;
END;
$$ LANGUAGE plpgsql;


-- Basic point machine position update
CREATE OR REPLACE FUNCTION railway_control.update_point_position(
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
$$ LANGUAGE plpgsql;


-- Enhanced subsidiary signal aspect update
CREATE OR REPLACE FUNCTION railway_control.update_subsidiary_signal_aspect(
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
$$ LANGUAGE plpgsql;

-- Enhanced paired point machine position update
CREATE OR REPLACE FUNCTION railway_control.update_point_position_paired(
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

    -- Check if point machine is locked by route assignment
    SELECT EXISTS(
        SELECT 1 FROM railway_control.resource_locks rl
        WHERE rl.resource_type = 'POINT_MACHINE'
        AND rl.resource_id = machine_id_param
        AND rl.is_active = TRUE
    ) INTO route_locked;

    IF route_locked THEN
        RAISE EXCEPTION 'Point machine % is locked by route assignment', machine_id_param;
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
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION railway_control.is_point_machine_available(
    machine_id_param TEXT
)
RETURNS BOOLEAN AS $$
DECLARE
    is_locked BOOLEAN;
    is_in_transition BOOLEAN;
    route_locking_enabled BOOLEAN;
BEGIN
    SELECT
        pm.is_locked OR EXISTS(
            SELECT 1 FROM railway_control.resource_locks rl
            WHERE rl.resource_type = 'POINT_MACHINE'
            AND rl.resource_id = machine_id_param
            AND rl.is_active = TRUE
        ),
        pm.operating_status = 'IN_TRANSITION',
        pm.route_locking_enabled
    INTO is_locked, is_in_transition, route_locking_enabled
    FROM railway_control.point_machines pm
    WHERE pm.machine_id = machine_id_param;

    RETURN NOT (COALESCE(is_locked, TRUE) OR COALESCE(is_in_transition, TRUE))
           AND COALESCE(route_locking_enabled, TRUE);
END;
$$ LANGUAGE plpgsql;


-- ✅ PRIMARY: Function to update track_segment circuit occupancy
CREATE OR REPLACE FUNCTION railway_control.update_track_circuit_occupancy(
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

    -- Update track_segment circuit occupancy
    UPDATE railway_control.track_circuits
    SET
        is_occupied = is_occupied_param,
        occupied_by = CASE
            WHEN is_occupied_param = TRUE THEN occupied_by_param
            ELSE NULL
        END,
        updated_at = CURRENT_TIMESTAMP
    WHERE circuit_id = circuit_id_param;

    GET DIAGNOSTICS rows_affected = ROW_COUNT;
    RETURN rows_affected > 0;
END;
$$ LANGUAGE plpgsql;

-- Function to update track_segment assignment with audit logging
CREATE OR REPLACE FUNCTION railway_control.update_track_segment_assignment(
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
$$ LANGUAGE plpgsql;

-- Get available circuits for route assignment
CREATE OR REPLACE FUNCTION railway_control.get_available_circuits()
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
$$ LANGUAGE plpgsql;

-- Primary circuit occupancy update function
CREATE OR REPLACE FUNCTION railway_control.update_track_circuit_occupancy(
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
$$ LANGUAGE plpgsql;

-- Legacy track segment occupancy function (maps to circuit)
CREATE OR REPLACE FUNCTION railway_control.update_track_segment_occupancy(
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
$$ LANGUAGE plpgsql;
-- Audit logging function
CREATE OR REPLACE FUNCTION railway_audit.log_changes()
RETURNS TRIGGER AS $$
DECLARE
    entity_name_val VARCHAR(100);
    old_json JSONB;
    new_json JSONB;
    operator_id_val VARCHAR(100);
    operation_source_val VARCHAR(50);
BEGIN
    -- Determine entity name based on table
    CASE TG_TABLE_NAME
        WHEN 'track_segments' THEN
            entity_name_val := COALESCE(NEW.segment_name, OLD.segment_name, NEW.segment_id, OLD.segment_id);
        WHEN 'track_circuits' THEN
            entity_name_val := COALESCE(NEW.circuit_name, OLD.circuit_name, NEW.circuit_id, OLD.circuit_id);
        WHEN 'signals' THEN
            entity_name_val := COALESCE(NEW.signal_name, OLD.signal_name, NEW.signal_id, OLD.signal_id);
        WHEN 'point_machines' THEN
            entity_name_val := COALESCE(NEW.machine_name, OLD.machine_name, NEW.machine_id, OLD.machine_id);
        WHEN 'route_assignments' THEN
            entity_name_val := CONCAT('Route: ', COALESCE(NEW.source_signal_id, OLD.source_signal_id), ' -> ', COALESCE(NEW.dest_signal_id, OLD.dest_signal_id));
        ELSE
            entity_name_val := 'Unknown';
    END CASE;

    -- Convert to JSON for comparison
    IF TG_OP != 'INSERT' THEN
        old_json := to_jsonb(OLD);
    END IF;
    IF TG_OP != 'DELETE' THEN
        new_json := to_jsonb(NEW);
    END IF;

    -- Get context variables with safe defaults
    BEGIN
        operator_id_val := current_setting('railway.operator_id');
    EXCEPTION WHEN OTHERS THEN
        operator_id_val := 'system';
    END;

    BEGIN
        operation_source_val := current_setting('railway.operation_source');
    EXCEPTION WHEN OTHERS THEN
        operation_source_val := 'HMI';
    END;

    -- Insert audit record
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
            WHEN 'route_assignments' THEN true  -- Routes are safety critical
            WHEN 'resource_locks' THEN true     -- Resource locks are safety critical
            ELSE false
        END,
        COALESCE(new_json, old_json),
        nextval('railway_audit.event_sequence')
    );

    RETURN COALESCE(NEW, OLD);
END;
$$ LANGUAGE plpgsql;
-- ============================================================================
-- ROUTE EVENT LOGGING FUNCTION
-- ============================================================================
CREATE OR REPLACE FUNCTION railway_control.insert_route_event(
    route_id_param UUID,
    event_type_param VARCHAR,
    event_data_param JSONB DEFAULT '{}',
    operator_id_param VARCHAR DEFAULT 'system',
    source_component_param VARCHAR DEFAULT 'DatabaseManager',
    correlation_id_param VARCHAR DEFAULT NULL,
    response_time_ms_param NUMERIC DEFAULT NULL,
    safety_critical_param BOOLEAN DEFAULT FALSE
)
RETURNS BOOLEAN AS $$
DECLARE
    rows_affected INTEGER;
    route_exists BOOLEAN;
BEGIN
    -- Validate route exists
    SELECT EXISTS(
        SELECT 1 FROM railway_control.route_assignments
        WHERE id = route_id_param
    ) INTO route_exists;

    IF NOT route_exists THEN
        RAISE EXCEPTION 'Route not found for event logging: %', route_id_param;
    END IF;

    -- ✅ FIXED: Use actual table column names
    INSERT INTO railway_control.route_events (
        route_id,
        event_type,
        event_data,
        triggered_by,        -- ✅ FIXED: Use actual column name
        occurred_at          -- ✅ FIXED: Use actual column name
    ) VALUES (
        route_id_param,
        event_type_param,
        COALESCE(event_data_param, '{}'),
        COALESCE(operator_id_param, 'system'),
        CURRENT_TIMESTAMP
    );

    GET DIAGNOSTICS rows_affected = ROW_COUNT;
    RETURN rows_affected > 0;
END;
$$ LANGUAGE plpgsql;

-- ============================================================================
-- ROUTE ASSIGNMENT CREATION FUNCTION (Simplified)
-- ============================================================================
CREATE OR REPLACE FUNCTION railway_control.insert_route_assignment(
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
BEGIN
    -- Set operator context for audit logging
    PERFORM set_config('railway.operator_id', operator_id_param, true);

    -- Insert route assignment
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

    -- Log route creation event
    IF rows_affected > 0 THEN
        -- ✅ FIXED: Use actual table column names
        INSERT INTO railway_control.route_events (
            route_id,
            event_type,
            event_data,
            triggered_by,        -- ✅ FIXED: Use actual column name
            occurred_at          -- ✅ FIXED: Use actual column name
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
                'operator', operator_id_param
            ),
            operator_id_param,   -- ✅ FIXED: Maps to triggered_by
            CURRENT_TIMESTAMP    -- ✅ FIXED: Maps to occurred_at
        );
    END IF;

    RETURN rows_affected > 0;
END;
$$ LANGUAGE plpgsql;
-- ============================================================================
-- HELPER FUNCTION: VALIDATE STATE TRANSITIONS
-- ============================================================================
CREATE OR REPLACE FUNCTION railway_control.is_valid_route_state_transition(
    current_state VARCHAR,
    new_state VARCHAR
)
RETURNS BOOLEAN AS $$
BEGIN
    -- Define valid state transitions
    RETURN CASE
        WHEN current_state = 'REQUESTED' AND new_state IN ('VALIDATING', 'FAILED') THEN TRUE
        WHEN current_state = 'VALIDATING' AND new_state IN ('RESERVED', 'FAILED') THEN TRUE
        WHEN current_state = 'RESERVED' AND new_state IN ('ACTIVE', 'FAILED', 'EMERGENCY_RELEASED') THEN TRUE
        WHEN current_state = 'ACTIVE' AND new_state IN ('PARTIALLY_RELEASED', 'RELEASED', 'EMERGENCY_RELEASED', 'DEGRADED') THEN TRUE
        WHEN current_state = 'PARTIALLY_RELEASED' AND new_state IN ('RELEASED', 'EMERGENCY_RELEASED') THEN TRUE
        WHEN current_state = 'DEGRADED' AND new_state IN ('ACTIVE', 'FAILED', 'EMERGENCY_RELEASED') THEN TRUE
        -- Allow re-attempts for failed routes
        WHEN current_state = 'FAILED' AND new_state IN ('REQUESTED', 'VALIDATING') THEN TRUE
        -- Emergency release allowed from any state
        WHEN new_state = 'EMERGENCY_RELEASED' THEN TRUE
        ELSE FALSE
    END;
END;
$$ LANGUAGE plpgsql;

-- ============================================================================
-- TRIGGERS
-- ============================================================================

-- Basic update triggers
CREATE TRIGGER trg_event_log_set_date
    BEFORE INSERT OR UPDATE ON railway_audit.event_log
    FOR EACH ROW EXECUTE FUNCTION railway_audit.set_event_date();

CREATE TRIGGER trg_track_circuits_updated_at
    BEFORE UPDATE ON railway_control.track_circuits
    FOR EACH ROW EXECUTE FUNCTION railway_control.update_timestamp();

CREATE TRIGGER trg_track_segments_updated_at
    BEFORE UPDATE ON railway_control.track_segments
    FOR EACH ROW EXECUTE FUNCTION railway_control.update_timestamp();

CREATE TRIGGER trg_signals_updated_at
    BEFORE UPDATE ON railway_control.signals
    FOR EACH ROW EXECUTE FUNCTION railway_control.update_timestamp();

CREATE TRIGGER trg_point_machines_updated_at
    BEFORE UPDATE ON railway_control.point_machines
    FOR EACH ROW EXECUTE FUNCTION railway_control.update_timestamp();

CREATE TRIGGER trg_signals_aspect_changed
    BEFORE UPDATE ON railway_control.signals
    FOR EACH ROW EXECUTE FUNCTION railway_control.update_signal_change_time();

CREATE TRIGGER trg_track_circuit_edges_updated_at
    BEFORE UPDATE ON railway_control.track_circuit_edges
    FOR EACH ROW EXECUTE FUNCTION railway_control.update_timestamp();

CREATE TRIGGER trg_signal_overlap_definitions_updated_at
    BEFORE UPDATE ON railway_control.signal_overlap_definitions
    FOR EACH ROW EXECUTE FUNCTION railway_control.update_timestamp();

CREATE TRIGGER trg_text_labels_updated_at
        BEFORE UPDATE ON railway_control.text_labels
        FOR EACH ROW EXECUTE FUNCTION railway_control.update_timestamp();

-- Audit triggers
CREATE TRIGGER trg_track_circuits_audit
    AFTER INSERT OR UPDATE OR DELETE ON railway_control.track_circuits
    FOR EACH ROW EXECUTE FUNCTION railway_audit.log_changes();

CREATE TRIGGER trg_track_segments_audit
    AFTER INSERT OR UPDATE OR DELETE ON railway_control.track_segments
    FOR EACH ROW EXECUTE FUNCTION railway_audit.log_changes();

CREATE TRIGGER trg_signals_audit
    AFTER INSERT OR UPDATE OR DELETE ON railway_control.signals
    FOR EACH ROW EXECUTE FUNCTION railway_audit.log_changes();

CREATE TRIGGER trg_point_machines_audit
    AFTER INSERT OR UPDATE OR DELETE ON railway_control.point_machines
    FOR EACH ROW EXECUTE FUNCTION railway_audit.log_changes();

-- Notification triggers
CREATE TRIGGER trg_track_circuits_notify
    AFTER INSERT OR UPDATE OR DELETE ON railway_control.track_circuits
    FOR EACH ROW EXECUTE FUNCTION railway_control.notify_railway_changes();

CREATE TRIGGER trg_track_segments_notify
    AFTER INSERT OR UPDATE OR DELETE ON railway_control.track_segments
    FOR EACH ROW EXECUTE FUNCTION railway_control.notify_railway_changes();

CREATE TRIGGER trg_signals_notify
    AFTER INSERT OR UPDATE OR DELETE ON railway_control.signals
    FOR EACH ROW EXECUTE FUNCTION railway_control.notify_railway_changes();

CREATE TRIGGER trg_point_machines_notify
    AFTER INSERT OR UPDATE OR DELETE ON railway_control.point_machines
    FOR EACH ROW EXECUTE FUNCTION railway_control.notify_railway_changes();

CREATE TRIGGER trg_route_assignments_notify
    AFTER INSERT OR UPDATE OR DELETE ON railway_control.route_assignments
    FOR EACH ROW EXECUTE FUNCTION railway_control.notify_route_changes();

-- ============================================================================
-- VIEWS
-- ============================================================================

-- Enhanced track segments with complete route assignment integration
CREATE OR REPLACE VIEW railway_control.v_track_segments_with_occupancy AS
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

    -- Route assignment status
    rl.is_active as is_route_locked,
    rl.lock_type as route_lock_type,
    rl.acquired_at as route_locked_at,
    rl.acquired_by as route_locked_by,
    rl.expires_at as route_lock_expires_at,

    -- Route context
    ra.id as route_id,
    ra.source_signal_id as route_source_signal,
    ra.dest_signal_id as route_dest_signal,
    ra.state as route_state,
    ra.direction as route_direction,
    ra.priority as route_priority,
    ra.created_at as route_created_at,

    -- Simplified availability status
    CASE
        WHEN NOT ts.is_active THEN 'INACTIVE'
        WHEN tc.is_occupied = true THEN 'OCCUPIED'
        WHEN tc.is_assigned = true THEN 'ROUTE_ASSIGNED'
        WHEN tc.is_overlap = true THEN 'OVERLAP_ASSIGNED'
        WHEN ts.is_assigned = true THEN 'ASSIGNED'
        WHEN rl.is_active = true THEN 'ROUTE_LOCKED'
        WHEN tc.circuit_id = 'INVALID' OR tc.circuit_id IS NULL THEN 'NO_CIRCUIT'
        ELSE 'AVAILABLE'
    END as availability_status,

    -- Route assignment eligibility
    CASE
        WHEN tc.circuit_id = 'INVALID' OR tc.circuit_id IS NULL THEN false
        WHEN NOT ts.is_active OR NOT tc.is_active THEN false
        WHEN tc.is_occupied = true OR ts.is_assigned = true OR rl.is_active = true THEN false
        ELSE true
    END as route_assignment_eligible

FROM railway_control.track_segments ts
LEFT JOIN railway_control.track_circuits tc ON ts.circuit_id = tc.circuit_id
LEFT JOIN railway_control.resource_locks rl ON (
    rl.resource_type = 'TRACK_CIRCUIT'
    AND rl.resource_id = tc.circuit_id
    AND rl.is_active = true
)
LEFT JOIN railway_control.route_assignments ra ON (
    rl.route_id = ra.id
    AND ra.state IN ('RESERVED', 'ACTIVE', 'PARTIALLY_RELEASED')
);


-- Enhanced track segment occupancy summary with route assignment metrics
CREATE OR REPLACE VIEW railway_control.v_track_segment_occupancy AS
SELECT
    -- Basic segment metrics
    COUNT(DISTINCT ts.segment_id) as total_segments,
    COUNT(DISTINCT ts.segment_id) FILTER (WHERE tc.is_occupied = true) as occupied_count,
    COUNT(DISTINCT ts.segment_id) FILTER (WHERE ts.is_assigned = true) as assigned_count,
    COUNT(DISTINCT ts.segment_id) FILTER (WHERE tc.is_occupied = true OR ts.is_assigned = true) as unavailable_count,

    -- Route assignment metrics
    COUNT(DISTINCT ts.segment_id) FILTER (WHERE rl.is_active = true) as route_locked_count,
    COUNT(DISTINCT ts.segment_id) FILTER (WHERE tc.is_occupied = true OR ts.is_assigned = true OR rl.is_active = true) as total_unavailable_count,

    -- Utilization percentages
    ROUND(
        (COUNT(DISTINCT ts.segment_id) FILTER (WHERE tc.is_occupied = true OR ts.is_assigned = true OR rl.is_active = true)::NUMERIC /
         COUNT(DISTINCT ts.segment_id)) * 100,
        2
    ) as total_utilization_percentage,

    -- Active routes count
    COUNT(DISTINCT ra.id) as active_routes_count,

    -- Speed and length metrics (from circuit data)
    AVG(tc.length_meters) as avg_circuit_length_meters,
    AVG(tc.max_speed_kmh) as avg_circuit_max_speed_kmh

FROM railway_control.track_segments ts
LEFT JOIN railway_control.track_circuits tc ON ts.circuit_id = tc.circuit_id
LEFT JOIN railway_control.resource_locks rl ON (
    rl.resource_type = 'TRACK_CIRCUIT'
    AND rl.resource_id = tc.circuit_id
    AND rl.is_active = true
)
LEFT JOIN railway_control.route_assignments ra ON (
    rl.route_id = ra.id
    AND ra.state IN ('RESERVED', 'ACTIVE', 'PARTIALLY_RELEASED')
)
WHERE ts.is_active = TRUE;


-- Complete signal information with route assignment
CREATE OR REPLACE VIEW railway_control.v_signals_complete AS
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
LEFT JOIN railway_config.signal_aspects sa_loop ON s.loop_aspect_id = sa_loop.id;

-- Complete point machine information view with route assignment integration
CREATE OR REPLACE VIEW railway_control.v_point_machines_complete AS
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

    -- Resource lock status
    EXISTS(
        SELECT 1 FROM railway_control.resource_locks rl
        WHERE rl.resource_type = 'POINT_MACHINE'
        AND rl.resource_id = pm.machine_id
        AND rl.is_active = TRUE
    ) as is_route_locked,

    -- Active route lock information
    rl.route_id as locked_by_route_id,
    rl.lock_type as route_lock_type,
    rl.acquired_at as route_locked_at,
    rl.acquired_by as route_locked_by,
    rl.expires_at as route_lock_expires_at,

    -- Route assignment context
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

    -- Availability for route assignment
    CASE
        WHEN pm.operating_status = 'FAILED' THEN 'FAILED'
        WHEN pm.operating_status = 'MAINTENANCE' THEN 'MAINTENANCE'
        WHEN pm.operating_status = 'IN_TRANSITION' THEN 'IN_TRANSITION'
        WHEN pm.is_locked OR EXISTS(
            SELECT 1 FROM railway_control.resource_locks rl2
            WHERE rl2.resource_type = 'POINT_MACHINE'
            AND rl2.resource_id = pm.machine_id
            AND rl2.is_active = TRUE
        ) THEN 'LOCKED'
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

-- Active resource locks
LEFT JOIN railway_control.resource_locks rl ON (
    rl.resource_type = 'POINT_MACHINE'
    AND rl.resource_id = pm.machine_id
    AND rl.is_active = TRUE
)

-- Route assignment information
LEFT JOIN railway_control.route_assignments ra ON (
    rl.route_id = ra.id
    AND ra.state IN ('RESERVED', 'ACTIVE', 'PARTIALLY_RELEASED')
);

-- Active routes summary
CREATE OR REPLACE VIEW railway_control.v_active_routes_summary AS
SELECT
    COUNT(*) as total_active_routes,
    COUNT(*) FILTER (WHERE state = 'RESERVED') as reserved_routes,
    COUNT(*) FILTER (WHERE state = 'ACTIVE') as active_routes,
    COUNT(*) FILTER (WHERE state = 'PARTIALLY_RELEASED') as partially_released_routes,
    COUNT(*) FILTER (WHERE overlap_release_due_at IS NOT NULL AND overlap_release_due_at <= CURRENT_TIMESTAMP) as expired_overlaps,
    AVG(EXTRACT(EPOCH FROM (CURRENT_TIMESTAMP - created_at)) * 1000) as avg_route_age_ms
FROM railway_control.route_assignments
WHERE state IN ('RESERVED', 'ACTIVE', 'PARTIALLY_RELEASED');

-- Resource utilization view
CREATE OR REPLACE VIEW railway_control.v_resource_utilization AS
SELECT
    'TRACK_CIRCUIT' as resource_type,
    COUNT(DISTINCT tc.circuit_id) as total_resources,
    COUNT(DISTINCT rl.resource_id) as locked_resources,
    ROUND((COUNT(DISTINCT rl.resource_id)::NUMERIC / COUNT(DISTINCT tc.circuit_id)) * 100, 2) as utilization_percentage
FROM railway_control.track_circuits tc
LEFT JOIN railway_control.resource_locks rl ON rl.resource_type = 'TRACK_CIRCUIT' AND rl.resource_id = tc.circuit_id AND rl.is_active = TRUE
WHERE tc.is_active = TRUE

UNION ALL

SELECT
    'POINT_MACHINE' as resource_type,
    COUNT(DISTINCT pm.machine_id) as total_resources,
    COUNT(DISTINCT rl.resource_id) as locked_resources,
    ROUND((COUNT(DISTINCT rl.resource_id)::NUMERIC / COUNT(DISTINCT pm.machine_id)) * 100, 2) as utilization_percentage
FROM railway_control.point_machines pm
LEFT JOIN railway_control.resource_locks rl ON rl.resource_type = 'POINT_MACHINE' AND rl.resource_id = pm.machine_id AND rl.is_active = TRUE;

-- Recent events view
CREATE VIEW railway_audit.v_recent_events AS
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
ORDER BY el.event_timestamp DESC;

-- ============================================================================
-- INITIAL CONFIGURATION DATA
-- ============================================================================

-- Insert default route configuration
INSERT INTO railway_control.route_configuration (config_key, config_value, config_type, description, updated_by) VALUES
('max_concurrent_routes', '10', 'VITAL', 'Maximum number of concurrent routes allowed', 'system'),
('pathfinding_timeout_ms', '500', 'PERFORMANCE', 'Maximum time allowed for pathfinding algorithm', 'system'),
('overlap_hold_default_seconds', '30', 'VITAL', 'Default overlap hold time in seconds', 'system'),
('route_validation_timeout_ms', '50', 'VITAL', 'Maximum time allowed for route validation', 'system'),
('resource_lock_timeout_minutes', '30', 'OPERATIONAL', 'Default timeout for resource locks', 'system'),
('degraded_mode_max_routes', '5', 'VITAL', 'Maximum routes in degraded mode', 'system'),
('performance_warning_threshold_ms', '45', 'PERFORMANCE', 'Performance warning threshold', 'system'),
('enable_route_visualization', 'true', 'OPERATIONAL', 'Enable route visualization on UI', 'system'),
('enable_performance_monitoring', 'true', 'OPERATIONAL', 'Enable performance metrics collection', 'system'),
('safety_violation_max_rate', '0.05', 'VITAL', 'Maximum allowed safety violation rate (5%)', 'system')
ON CONFLICT (config_key) DO NOTHING;

-- ============================================================================
-- SECURITY: Create roles and permissions
-- ============================================================================

-- Railway Control Operator (full access to operations)
CREATE ROLE railway_operator;
GRANT USAGE ON SCHEMA railway_control TO railway_operator;
GRANT ALL PRIVILEGES ON ALL TABLES IN SCHEMA railway_control TO railway_operator;
GRANT ALL PRIVILEGES ON ALL SEQUENCES IN SCHEMA railway_control TO railway_operator;
GRANT USAGE ON SCHEMA railway_config TO railway_operator;
GRANT SELECT ON ALL TABLES IN SCHEMA railway_config TO railway_operator;

-- Railway Observer (read-only access)
CREATE ROLE railway_observer;
GRANT USAGE ON SCHEMA railway_control TO railway_observer;
GRANT SELECT ON ALL TABLES IN SCHEMA railway_control TO railway_observer;
GRANT USAGE ON SCHEMA railway_config TO railway_observer;
GRANT SELECT ON ALL TABLES IN SCHEMA railway_config TO railway_observer;

-- Railway Auditor (access to audit logs)
CREATE ROLE railway_auditor;
GRANT USAGE ON SCHEMA railway_audit TO railway_auditor;
GRANT SELECT ON ALL TABLES IN SCHEMA railway_audit TO railway_auditor;

-- Schema comments
COMMENT ON SCHEMA railway_control IS 'Main railway control system with integrated route assignment capabilities';
COMMENT ON SCHEMA railway_audit IS 'Audit trail and event logging for compliance';
COMMENT ON SCHEMA railway_config IS 'Configuration and lookup tables';

-- ============================================================================
-- UNIFIED SCHEMA CREATION COMPLETED
-- ============================================================================
-- This unified script creates a complete railway control system database
-- with integrated route assignment capabilities from the ground up.
-- No ALTER statements needed - everything is created in final form.
-- ============================================================================
