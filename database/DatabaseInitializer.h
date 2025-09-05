#pragma once
#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariantMap>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <QFile>
#include <QTextStream>
#include <QTimer>
#include <memory>

class DatabaseInitializer : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isRunning READ isRunning NOTIFY isRunningChanged)
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString currentOperation READ currentOperation NOTIFY currentOperationChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit DatabaseInitializer(QObject* parent = nullptr);
    ~DatabaseInitializer();

    // Properties
    bool isRunning() const { return m_isRunning; }
    int progress() const { return m_progress; }
    QString currentOperation() const { return m_currentOperation; }
    QString lastError() const { return m_lastError; }

    // Main operations
    Q_INVOKABLE bool initializeDatabase();
    Q_INVOKABLE bool isDatabaseConnected();
    Q_INVOKABLE QVariantMap getDatabaseStatus();
    Q_INVOKABLE void testConnection();
    Q_INVOKABLE void debugConnectionTest();

    // Async operations (for backward compatibility)
    Q_INVOKABLE void resetDatabaseAsync();

public slots:
    Q_INVOKABLE void testConnectionAsync();

signals:
    void isRunningChanged();
    void progressChanged();
    void currentOperationChanged();
    void lastErrorChanged();
    void resetCompleted(bool success, const QString& message);
    void connectionTestCompleted(bool success, const QString& message);

private slots:
    void performReset();

private:
    // Properties
    bool m_isRunning = false;
    int m_progress = 0;
    QString m_currentOperation;
    QString m_lastError;

    // Database connection
    QSqlDatabase db;
    QTimer* resetTimer;

    //
    // CORE DATABASE OPERATIONS
    //

    // Connection management
    bool connectToDatabase();
    bool connectToSystemPostgreSQL();
    bool connectToPortablePostgreSQL();

    // Unified schema creation approach
    bool dropAndCreateSchemas();
    bool createUnifiedTables();
    bool validateDatabase();

    //
    // UNIFIED TABLE CREATION METHODS
    //

    // Configuration tables with route assignment integration
    bool createConfigurationTables();

    // Control tables with route assignment integration
    bool createControlTables();

    // Route assignment specific tables
    bool createRouteAssignmentTables();

    // Audit and logging tables
    bool createAuditTables();

    //
    // DATABASE STRUCTURE CREATION
    //

    // Performance and safety indexes
    bool createIndexes();

    // Database functions (utility and route assignment)
    bool createFunctions();

    // Database triggers (audit and notifications)
    bool createTriggers();

    // Database views (with route assignment integration)
    bool createViews();

    // Role-based security
    bool createRolesAndPermissions();

    //
    // DATA POPULATION METHODS
    //

    // Main data population coordinator
    bool populateInitialData();

    // Configuration data
    bool populateConfigurationData();

    // Track infrastructure with route assignment integration
    bool populateTrackCircuits();
    bool populateTrackSegments();

    // Signal data with route assignment properties
    bool populateSignals();

    // Point machine data with route assignment integration
    bool populatePointMachines();

    // Additional infrastructure
    bool populateTextLabels();
    bool populateInterlockingRules();

    //
    // ROUTE ASSIGNMENT DATA POPULATION
    //

    // Route assignment specific data coordinator
    bool populateRouteAssignmentData();

    // Pathfinding infrastructure
    bool populateSignalAdjacencyAnchors();

    //
    // HELPER METHODS
    //

    // Query execution
    bool executeQuery(const QString& query, const QVariantList& params = QVariantList());

    // Error and progress management
    void setError(const QString& error);
    void updateProgress(int value, const QString& operation);

    //
    // DATA INSERTION HELPERS
    //

    // Configuration data insertion
    int insertSignalType(const QString& typeCode, const QString& typeName,
                         int maxAspects, bool isRouteSignal, int routePriority);
    int insertSignalAspect(const QString& aspectCode, const QString& aspectName,
                           const QString& colorCode, int safetyLevel,
                           bool permitsRouteEstablishment, bool requiresOverlap);
    int insertPointPosition(const QString& positionCode, const QString& positionName,
                            double pathfindingWeight, int transitionTimeMs);

    // Utility helpers
    int getAspectIdByCode(const QString& aspectCode);

    //
    // DATA SOURCE METHODS
    //

    // Track infrastructure data
    QJsonArray getTrackSegmentsData();
    QJsonArray getTrackCircuitMappings();

    // Signal data by type
    QJsonArray getOuterSignalsData();
    QJsonArray getHomeSignalsData();
    QJsonArray getStarterSignalsData();
    QJsonArray getAdvancedStarterSignalsData();

    // Other infrastructure data
    QJsonArray getPointMachinesData();
    QJsonArray getTextLabelsData();

    // Safety and interlocking data
    QJsonArray getInterlockingRulesData();

    //
    // LEGACY METHODS (for backward compatibility)
    //

    // Legacy schema operations
    bool dropExistingSchemas();
    bool createSchemas();
    bool verifySchemas();
    bool executeSchemaScript();

    // Legacy advanced schema methods
    bool createAdvancedFunctions();
    bool createAdvancedTriggers();
    bool createGinIndexes();

    // Legacy route assignment methods
    bool executeRouteAssignmentSchema();
};
