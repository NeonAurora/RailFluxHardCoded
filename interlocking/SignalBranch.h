#pragma once
#include <QObject>
#include <QSqlQuery>
#include "InterlockingService.h"
#include "InterlockingRuleEngine.h"

class DatabaseManager;

class SignalBranch : public QObject {
    Q_OBJECT

public:
public:
    InterlockingRuleEngine* getRuleEngine() const { return m_ruleEngine.get(); }
    explicit SignalBranch(DatabaseManager* dbManager, QObject* parent = nullptr);

    //   Main validation interface
    ValidationResult validateMainAspectChange(const QString& signalId,
                                              const QString& currentAspect,
                                              const QString& requestedAspect,
                                              const QString& operatorId);

    ValidationResult validateSubsidiaryAspectChange(const QString& signalId,
                                                    const QString& aspectType,
                                                    const QString& currentAspect,
                                                    const QString& requestedAspect,
                                                    const QString& operatorId);

private:

    enum class SignalGroup {
        MAIN_SIGNALS,        // RED, YELLOW, GREEN, SINGLE_YELLOW, DOUBLE_YELLOW
        CALLING_ON,          // WHITE
        LOOP_SIGNALS,        // YELLOW/OFF (part of home signals)
        SHUNT_SIGNALS,       // BLUE (future)
        BLOCK_SIGNALS        // PURPLE (future)
    };

    enum class InterlockingType {
        OPPOSING_SIGNALS,
        CONFLICTING_ROUTES,
        SEQUENTIAL_DEPENDENCY,
        HOME_STARTER_PAIR
    };

    struct SignalCapabilities {
        QStringList supportedAspects;
        SignalGroup primaryGroup;
        bool supportsCallingOn;
        bool supportsLoop;
    };

    //   UPDATED: Now handles track circuits (keeping struct name for compatibility)
    struct ProtectedTrackSegmentsValidation {
        bool isValid;
        QStringList protectedTrackSegments;  //   NOTE: Field name kept for compatibility - contains track circuits
        QString errorReason;
        QStringList inconsistentSources;
        QStringList occupiedTrackSegments;   //   NOTE: Field name kept for compatibility - contains occupied circuits
    };

    ValidationResult validateBasicTransition(const QString& signalId,
                                             const QString& currentAspect,
                                             const QString& requestedAspect);

    //   UPDATED: Now validates track circuit protection
    ValidationResult checkTrackCircuitProtection(const QString& signalId,
                                                 const QString& requestedAspect);

    ValidationResult checkInterlockedSignals(const QString& signalId,
                                             const QString& currentAspect,
                                             const QString& requestedAspect);

    ValidationResult checkSignalActive(const QString& signalId);

    ValidationResult validateSubsidiaryTransition(const QString& signalId,
                                                  const QString& aspectType,
                                                  const QString& currentAspect,
                                                  const QString& requestedAspect);

    ValidationResult validateCallingOnSafetyRules(const QString& signalId,
                                                  const QString& currentAspect,
                                                  const QString& requestedAspect);

    ValidationResult validateLoopSignalRules(const QString& signalId,
                                             const QString& currentAspect,
                                             const QString& requestedAspect);

    ValidationResult checkSubsidiaryInterlocking(const QString& signalId,
                                                 const QString& aspectType,
                                                 const QString& currentAspect,
                                                 const QString& requestedAspect);

    QString getCurrentMainSignalAspect(const QString& signalId);
    QString predictCompositeAspectAfterSubsidiaryChange(const QString& signalId,
                                                        const QString& aspectType,
                                                        const QString& newSubsidiaryAspect);


    //   UPDATED: Now validates track circuits instead of track segments
    ProtectedTrackSegmentsValidation validateProtectedTrackCircuits(const QString& signalId);

    //   UPDATED: Gets protected track circuits from signal data
    QStringList getProtectedTrackCircuitsFromSignalData(const QString& signalId);

    //   UPDATED: Gets protected track circuits from interlocking rules via DatabaseManager
    QStringList getProtectedTrackCircuitsFromInterlockingRules(const QString& signalId);

    //   UPDATED: Validates track circuit consistency between sources
    bool validateTrackCircuitConsistency(const QStringList& fromSignalData,
                                         const QStringList& fromInterlockingRules,
                                         ProtectedTrackSegmentsValidation& result);

    //   UPDATED: Validates track circuit occupancy status
    bool validateTrackCircuitOccupancy(const QStringList& protectedTrackCircuits,
                                       ProtectedTrackSegmentsValidation& result);

public:
    //   UPDATED: Public API now returns protected track circuits
    QStringList getProtectedTrackCircuits(const QString& signalId);
    QStringList getInterlockedSignals(const QString& signalId);

private:

    bool isValidAspectTransition(const QString& from, const QString& to);
    SignalGroup determineSignalGroup(const QString& aspect);
    bool isDangerousInterGroupTransition(SignalGroup fromGroup, SignalGroup toGroup,
                                         const QString& fromAspect, const QString& toAspect);


    bool supportsAspect(const QString& signalId, const QString& aspect);
    int getAspectPrecedence(const QString& aspect);
    InterlockingType determineInterlockingType(const QString& signal1Id, const QString& signal2Id);


    DatabaseManager* m_dbManager;
    QString m_currentSignalId;  // Context for validation
    std::unique_ptr<InterlockingRuleEngine> m_ruleEngine;
};
