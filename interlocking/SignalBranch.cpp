#include "SignalBranch.h"
#include "../database/DatabaseManager.h"
#include "InterlockingRuleEngine.h"
#include <QDebug>

SignalBranch::SignalBranch(DatabaseManager* dbManager, QObject* parent)
    : QObject(parent), m_dbManager(dbManager) {

    if (!dbManager) {
        qCritical() << " SAFETY: SignalBranch initialized with null DatabaseManager!";
        // Consider throwing or handling this critical error
    }

    // ? INITIALIZE RULE ENGINE
    m_ruleEngine = std::make_unique<InterlockingRuleEngine>(dbManager, this);

    if (!m_ruleEngine->loadRulesFromResource()) {
        qCritical() << " SAFETY: Failed to load interlocking rules - system may not be safe!";
        // Consider setting a safety flag or refusing to operate
    }
}

ValidationResult SignalBranch::validateMainAspectChange(
    const QString& signalId, const QString& currentAspect,
    const QString& requestedAspect, const QString& operatorId) {

    // 1. Check if signal is active
    auto activeResult = checkSignalActive(signalId);
    if (!activeResult.isAllowed()) return activeResult;

    // 2. Basic transition validation
    auto basicResult = validateBasicTransition(signalId, currentAspect, requestedAspect);
    if (!basicResult.isAllowed()) return basicResult;

    // 3. ? UPDATED: Track Circuit protection validation
    auto trackCircuitResult = checkTrackCircuitProtection(signalId, requestedAspect);
    if (!trackCircuitResult.isAllowed()) return trackCircuitResult;

    // 4. Interlocked signals validation
    auto interlockResult = checkInterlockedSignals(signalId, currentAspect, requestedAspect);
    if (!interlockResult.isAllowed()) return interlockResult;

    return ValidationResult::allowed("All signal validations passed");
}

ValidationResult SignalBranch::validateSubsidiaryAspectChange(
    const QString& signalId, const QString& aspectType,
    const QString& currentAspect, const QString& requestedAspect,
    const QString& operatorId) {

    qDebug() << " SIGNAL BRANCH: Subsidiary signal validation:" << signalId
             << "Type:" << aspectType
             << "Transition:" << currentAspect << "?" << requestedAspect;

    // ? 1. Check if signal exists and is active
    auto activeResult = checkSignalActive(signalId);
    if (!activeResult.isAllowed()) return activeResult;

    // ? 2. Validate aspect type and transition rules
    auto transitionResult = validateSubsidiaryTransition(signalId, aspectType, currentAspect, requestedAspect);
    if (!transitionResult.isAllowed()) return transitionResult;

    // ? 3. Check calling-on specific safety rules
    if (aspectType == "CALLING_ON") {
        auto callingOnResult = validateCallingOnSafetyRules(signalId, currentAspect, requestedAspect);
        if (!callingOnResult.isAllowed()) return callingOnResult;
    }

    // ? 4. Check loop signal specific rules
    if (aspectType == "LOOP") {
        auto loopResult = validateLoopSignalRules(signalId, currentAspect, requestedAspect);
        if (!loopResult.isAllowed()) return loopResult;
    }

    // ? 5. Check interlocking rules (if any apply to subsidiary signals)
    auto interlockResult = checkSubsidiaryInterlocking(signalId, aspectType, currentAspect, requestedAspect);
    if (!interlockResult.isAllowed()) return interlockResult;

    qDebug() << "? SIGNAL BRANCH: All subsidiary signal validations passed for" << signalId << aspectType;
    return ValidationResult::allowed("All subsidiary signal validations passed");
}

ValidationResult SignalBranch::validateSubsidiaryTransition(
    const QString& signalId, const QString& aspectType,
    const QString& currentAspect, const QString& requestedAspect) {

    qDebug() << " Validating subsidiary transition:" << aspectType << currentAspect << "?" << requestedAspect;

    // ? CALLING-ON: Only OFF ? WHITE allowed
    if (aspectType == "CALLING_ON") {
        if (!((currentAspect == "OFF" && requestedAspect == "WHITE") ||
              (currentAspect == "WHITE" && requestedAspect == "OFF"))) {
            return ValidationResult::blocked(
                QString("Invalid calling-on transition: %1 ? %2. Only OFF ? WHITE allowed.")
                    .arg(currentAspect, requestedAspect),
                "CALLING_ON_INVALID_TRANSITION");
        }
    }
    // ? LOOP: Only OFF ? YELLOW allowed
    else if (aspectType == "LOOP") {
        if (!((currentAspect == "OFF" && requestedAspect == "YELLOW") ||
              (currentAspect == "YELLOW" && requestedAspect == "OFF"))) {
            return ValidationResult::blocked(
                QString("Invalid loop signal transition: %1 ? %2. Only OFF ? YELLOW allowed.")
                    .arg(currentAspect, requestedAspect),
                "LOOP_INVALID_TRANSITION");
        }
    }
    // ? UNKNOWN TYPE
    else {
        return ValidationResult::blocked(
            QString("Unknown subsidiary aspect type: %1").arg(aspectType),
            "UNKNOWN_SUBSIDIARY_TYPE");
    }

    return ValidationResult::allowed("Valid subsidiary transition");
}

ValidationResult SignalBranch::validateCallingOnSafetyRules(
    const QString& signalId, const QString& currentAspect, const QString& requestedAspect) {

    qDebug() << "? CALLING-ON VALIDATION:" << signalId
             << "Current:" << currentAspect << "? Requested:" << requestedAspect;

    // ? RULE 1: Calling-on can only be cleared when main signal is at danger
    if (requestedAspect == "WHITE") {
        QString mainAspect = getCurrentMainSignalAspect(signalId);
        if (mainAspect.isEmpty()) {
            return ValidationResult::blocked(
                QString("Cannot determine main signal aspect for %1").arg(signalId),
                "MAIN_ASPECT_UNKNOWN");
        }

        if (mainAspect != "RED") {
            return ValidationResult::blocked(
                QString("Calling-on signal can only be cleared when main signal is at danger. Main signal: %1")
                    .arg(mainAspect),
                "CALLING_ON_MAIN_NOT_DANGER");
        }

        qDebug() << "? Basic calling-on safety check passed: Main signal at danger (" << mainAspect << ")";

        // ? RULE 2: Check interlocking for the resulting composite aspect
        QString predictedCompositeAspect = predictCompositeAspectAfterSubsidiaryChange(
            signalId, "CALLING_ON", requestedAspect);

        qDebug() << " Predicted composite aspect after calling-on change:" << predictedCompositeAspect;

        if (!m_ruleEngine) {
            qWarning() << "? Rule engine not available for calling-on validation";
            return ValidationResult::blocked("Interlocking rule engine not available", "RULE_ENGINE_MISSING");
        }

        auto interlockingResult = m_ruleEngine->validateInterlockedSignalAspectChange(
            signalId, mainAspect, predictedCompositeAspect);

        if (!interlockingResult.isAllowed()) {
            qDebug() << "? Calling-on activation blocked by interlocking:" << interlockingResult.getReason();
            return ValidationResult::blocked(
                QString("Calling-on signal cannot be activated: %1").arg(interlockingResult.getReason()),
                "CALLING_ON_INTERLOCKING_VIOLATION"
                );
        }

        qDebug() << "? Calling-on activation allowed by interlocking";
    }

    // ? RULE 3: Turning OFF is always allowed
    if (requestedAspect == "OFF") {
        qDebug() << "? Calling-on signal turning OFF - allowed";
    }

    return ValidationResult::allowed("Calling-on safety rules passed");
}

ValidationResult SignalBranch::validateLoopSignalRules(
    const QString& signalId, const QString& currentAspect, const QString& requestedAspect) {

    qDebug() << " LOOP SIGNAL VALIDATION:" << signalId
             << "Current loop:" << currentAspect << "? Requested:" << requestedAspect;

    // ? RULE 1: Basic transition validation (already done in validateSubsidiaryTransition)

    // ? RULE 2: If turning OFF the loop signal, allow it (no interlocking needed)
    if (requestedAspect == "OFF") {
        qDebug() << "? Loop signal turning OFF - allowed without interlocking check";
        return ValidationResult::allowed("Loop signal turning OFF");
    }

    // ? RULE 3: If turning ON the loop signal (YELLOW), check interlocking
    if (requestedAspect == "YELLOW") {
        qDebug() << " Loop signal turning ON - checking interlocking for resulting composite aspect";

        // ? PREDICT: What will the composite aspect be after this change?
        QString predictedCompositeAspect = predictCompositeAspectAfterSubsidiaryChange(
            signalId, "LOOP", requestedAspect);

        qDebug() << " Predicted composite aspect after loop change:" << predictedCompositeAspect;

        // ? VALIDATE: Use interlocking rule engine to check if this composite aspect is allowed
        if (!m_ruleEngine) {
            qWarning() << "? Rule engine not available for loop signal validation";
            return ValidationResult::blocked("Interlocking rule engine not available", "RULE_ENGINE_MISSING");
        }

        // ? INTERLOCKING: Check if the predicted composite aspect is allowed
        auto interlockingResult = m_ruleEngine->validateInterlockedSignalAspectChange(
            signalId, getCurrentMainSignalAspect(signalId), predictedCompositeAspect);

        if (!interlockingResult.isAllowed()) {
            qDebug() << "? Loop signal activation blocked by interlocking:" << interlockingResult.getReason();
            return ValidationResult::blocked(
                QString("Loop signal cannot be activated: %1").arg(interlockingResult.getReason()),
                "LOOP_INTERLOCKING_VIOLATION"
                );
        }

        qDebug() << "? Loop signal activation allowed by interlocking";
        return ValidationResult::allowed("Loop signal activation permitted by interlocking rules");
    }

    // ? FALLBACK: Unknown requested aspect
    return ValidationResult::blocked(
        QString("Unknown loop aspect requested: %1").arg(requestedAspect),
        "UNKNOWN_LOOP_ASPECT"
        );
}

QString SignalBranch::predictCompositeAspectAfterSubsidiaryChange(
    const QString& signalId, const QString& aspectType, const QString& newSubsidiaryAspect) {

    qDebug() << " PREDICTING composite aspect for" << signalId
             << "after changing" << aspectType << "to" << newSubsidiaryAspect;

    // ? GET: Current signal state
    auto signalData = m_dbManager->getSignalById(signalId);
    QString currentMainAspect = signalData.value("currentAspect", "RED").toString();
    QString currentCallingOn = signalData.value("callingOnAspect", "OFF").toString();
    QString currentLoop = signalData.value("loopAspect", "OFF").toString();

    qDebug() << "  Current state - Main:" << currentMainAspect
             << "Calling-On:" << currentCallingOn
             << "Loop:" << currentLoop;

    // ? SIMULATE: Apply the requested change
    QString newCallingOn = currentCallingOn;
    QString newLoop = currentLoop;

    if (aspectType == "CALLING_ON") {
        newCallingOn = newSubsidiaryAspect;
    } else if (aspectType == "LOOP") {
        newLoop = newSubsidiaryAspect;
    }

    qDebug() << "  After change - Main:" << currentMainAspect
             << "Calling-On:" << newCallingOn
             << "Loop:" << newLoop;

    // ? BUILD: Predicted composite aspect
    QString predictedComposite = currentMainAspect;

    if (newCallingOn == "WHITE") {
        predictedComposite += "_CALLING";
        qDebug() << "  + Added CALLING component";
    }

    if (newLoop == "YELLOW") {
        predictedComposite += "_LOOP";
        qDebug() << "  + Added LOOP component";
    }

    qDebug() << " Predicted composite aspect:" << predictedComposite;
    return predictedComposite;
}

ValidationResult SignalBranch::checkSubsidiaryInterlocking(
    const QString& signalId, const QString& aspectType,
    const QString& currentAspect, const QString& requestedAspect) {

    // ? FUTURE: Check if there are any interlocking rules for subsidiary signals
    // For now, most interlocking rules apply to main signals only

    qDebug() << " Checking subsidiary interlocking for" << signalId << aspectType;

    // Future enhancements:
    // - Check if clearing calling-on affects other signals
    // - Check if loop signal conflicts with main line movements
    // - Validate subsidiary signal combinations

    return ValidationResult::allowed("No subsidiary interlocking violations");
}

QString SignalBranch::getCurrentMainSignalAspect(const QString& signalId) {
    if (!m_dbManager || !m_dbManager->isConnected()) {
        qWarning() << "? Cannot get main signal aspect: Database not connected";
        return QString();
    }

    return m_dbManager->getCurrentSignalAspect(signalId);
}

ValidationResult SignalBranch::validateBasicTransition(
    const QString& signalId, const QString& currentAspect, const QString& requestedAspect) {

    // Store signal ID for transition validation
    m_currentSignalId = signalId;

    //   ENHANCED: Special handling for RED→RED transitions
    if (currentAspect == requestedAspect) {
        if (currentAspect == "RED" && requestedAspect == "RED") {
            qWarning() << " [SAFETY_REDUNDANCY] Signal" << signalId
                       << "RED→RED transition allowed for safety redundancy";
            return ValidationResult::allowed("RED to RED transition allowed for safety");
        } else {
            return ValidationResult::blocked(
                QString("No transition needed - signal %1 already showing %2")
                    .arg(signalId, currentAspect),
                "NO_TRANSITION_NEEDED"
                );
        }
    }

    // Check if transition is valid
    if (!isValidAspectTransition(currentAspect, requestedAspect)) {
        return ValidationResult::blocked(
            QString("Invalid aspect transition from %1 to %2 for signal %3")
                .arg(currentAspect, requestedAspect, signalId),
            "INVALID_TRANSITION"
            );
    }

    // Get signal data to check capabilities
    auto signalData = m_dbManager->getSignalById(signalId);
    if (signalData.isEmpty()) {
        return ValidationResult::blocked("Signal not found: " + signalId, "SIGNAL_NOT_FOUND");
    }

    //   SAFETY: Validate aspect is supported by this signal type
    QStringList possibleAspects = signalData["possibleAspects"].toStringList();
    if (!possibleAspects.contains(requestedAspect)) {
        return ValidationResult::blocked(
            QString("Aspect %1 not supported by %2 signal %3")
                .arg(requestedAspect, signalData["type"].toString(), signalId),
            "ASPECT_NOT_SUPPORTED"
            );
    }

    return ValidationResult::allowed("Basic transition validation passed");
}

// ? UPDATED: Track Circuit Protection Method
ValidationResult SignalBranch::checkTrackCircuitProtection(const QString& signalId, const QString& requestedAspect) {
    // ? SAFETY: Only check track circuit protection for proceed aspects
    if (requestedAspect == "RED") {
        return ValidationResult::allowed("RED aspect - no track circuit protection required");
    }

    // ? SAFETY: Comprehensive protected track circuits validation
    auto validation = validateProtectedTrackCircuits(signalId);

    if (!validation.isValid) {
        return ValidationResult::blocked(
            QString("Cannot clear signal %1: %2").arg(signalId, validation.errorReason),
            validation.occupiedTrackSegments.isEmpty() ? "TRACK_CIRCUIT_PROTECTION_VALIDATION_FAILED" : "TRACK_CIRCUIT_OCCUPIED"
            );
    }

    // ? SUCCESS: All protected track circuits are clear
    return ValidationResult::allowed(
        QString("All %1 protected track circuits are clear").arg(validation.protectedTrackSegments.size())
        );
}

ValidationResult SignalBranch::checkInterlockedSignals(
    const QString& signalId,
    const QString& currentAspect,
    const QString& requestedAspect) {

    if (!m_ruleEngine) {
        qCritical() << " SAFETY: Rule engine not initialized!";
        return ValidationResult::blocked("Interlocking system not available", "RULE_ENGINE_MISSING");
    }

    return m_ruleEngine->validateInterlockedSignalAspectChange(signalId, currentAspect, requestedAspect);
}

ValidationResult SignalBranch::checkSignalActive(const QString& signalId) {
    auto signalData = m_dbManager->getSignalById(signalId);
    if (signalData.isEmpty()) {
        return ValidationResult::blocked("Signal not found: " + signalId, "SIGNAL_NOT_FOUND");
    }

    if (!signalData["isActive"].toBool()) {
        return ValidationResult::blocked("Signal is not active: " + signalId, "SIGNAL_INACTIVE");
    }

    return ValidationResult::allowed();
}

// ? UPDATED: Public API method for track circuits
QStringList SignalBranch::getProtectedTrackCircuits(const QString& signalId) {
    // ? SAFETY: Use comprehensive validation for safety-critical track circuit protection
    auto validation = validateProtectedTrackCircuits(signalId);

    if (!validation.isValid) {
        qCritical() << " SAFETY CRITICAL: Protected track circuits validation failed for signal"
                    << signalId << ":" << validation.errorReason;

        // ? SAFETY: Log to audit system for compliance
        // TODO: Add to audit log with safety_critical = true

        // ? SAFETY: Return empty list to force restrictive behavior
        return QStringList();
    }

    return validation.protectedTrackSegments; // Keep field name for compatibility
}

QStringList SignalBranch::getInterlockedSignals(const QString& signalId) {
    auto signalData = m_dbManager->getSignalById(signalId);
    if (!signalData.isEmpty()) {
        // This should come from your existing interlocked_with field
        return signalData["interlockedWith"].toStringList();
    }
    return QStringList();
}

bool SignalBranch::isValidAspectTransition(const QString& from, const QString& to) {
    //   SPECIAL CASE: Allow RED to RED transitions with warning (safety redundancy)
    if (from == to) {
        if (from == "RED" && to == "RED") {
            qWarning() << " [SAFETY_REDUNDANCY] Setting signal to RED when already RED:"
                       << m_currentSignalId << "- allowed for safety but may indicate logic issue";
            return true;  // Allow RED→RED with warning
        } else {
            qDebug() << "[TRANSITION_BLOCKED] Same aspect transition blocked:"
                     << m_currentSignalId << from << "→" << to
                     << "- no change needed for non-RED aspects";
            return false; // Block all other same-aspect transitions
        }
    }

    //   SAFETY: RED is always accessible for emergency stops
    if (to == "RED") return true;

    //   Get signal capabilities from database to validate transition
    // This prevents invalid capability transitions
    auto signalData = m_dbManager->getSignalById(m_currentSignalId);
    if (signalData.isEmpty()) return false;

    QStringList supportedAspects = signalData["possibleAspects"].toStringList();

    //   SAFETY: Cannot transition to unsupported aspect
    if (!supportedAspects.contains(to)) {
        qDebug() << "BLOCKED: Signal doesn't support aspect" << to;
        return false;
    }

    //   Check for inter-group transitions (your main concern)
    SignalGroup fromGroup = determineSignalGroup(from);
    SignalGroup toGroup = determineSignalGroup(to);

    if (fromGroup != toGroup) {
        //   SAFETY: Block dangerous inter-group transitions
        if (isDangerousInterGroupTransition(fromGroup, toGroup, from, to)) {
            qDebug() << "BLOCKED: Dangerous inter-group transition" << from << "→" << to;
            return false;
        }
    }

    //   Allow all other valid transitions
    return true;
}

SignalBranch::SignalGroup SignalBranch::determineSignalGroup(const QString& aspect) {
    // ? SAFETY: Categorize aspects by their functional groups
    if (aspect == "WHITE") return SignalGroup::CALLING_ON;
    if (aspect == "BLUE") return SignalGroup::SHUNT_SIGNALS;  // Future
    if (aspect == "PURPLE") return SignalGroup::BLOCK_SIGNALS; // Future

    // Main signaling group
    QStringList mainAspects = {"RED", "YELLOW", "GREEN", "SINGLE_YELLOW", "DOUBLE_YELLOW"};
    if (mainAspects.contains(aspect)) return SignalGroup::MAIN_SIGNALS;

    return SignalGroup::MAIN_SIGNALS; // Safe default
}

bool SignalBranch::isDangerousInterGroupTransition(
    SignalGroup fromGroup, SignalGroup toGroup,
    const QString& from, const QString& to) {

    // ? SAFETY: Define dangerous transitions

    // WHITE (calling-on) should only transition to/from RED for safety
    if (fromGroup == SignalGroup::CALLING_ON && toGroup == SignalGroup::MAIN_SIGNALS) {
        return to != "RED"; // Only allow WHITE ? RED
    }
    if (fromGroup == SignalGroup::MAIN_SIGNALS && toGroup == SignalGroup::CALLING_ON) {
        return from != "RED"; // Only allow RED ? WHITE
    }

    // Future: BLUE (shunt) transitions
    if (fromGroup == SignalGroup::SHUNT_SIGNALS || toGroup == SignalGroup::SHUNT_SIGNALS) {
        // Define shunt signal rules when implemented
        return false; // For now, allow (implement rules later)
    }

    // Future: PURPLE (block) transitions
    if (fromGroup == SignalGroup::BLOCK_SIGNALS || toGroup == SignalGroup::BLOCK_SIGNALS) {
        // Define block signal rules when implemented
        return false; // For now, allow (implement rules later)
    }

    return false; // Allow other inter-group transitions
}

// ? UPDATED: Main validation method for track circuits
SignalBranch::ProtectedTrackSegmentsValidation SignalBranch::validateProtectedTrackCircuits(const QString& signalId) {
    ProtectedTrackSegmentsValidation result;
    result.isValid = false;

    // ? UPDATED: Get protected track circuits from two sources
    QStringList trackCircuitsFromSignalData = getProtectedTrackCircuitsFromSignalData(signalId);
    QStringList trackCircuitsFromInterlockingRules = getProtectedTrackCircuitsFromInterlockingRules(signalId);

    qDebug() << " SAFETY AUDIT: Protected track circuits for signal" << signalId;
    qDebug() << "   From signal data:" << trackCircuitsFromSignalData;
    qDebug() << "   From interlocking rules:" << trackCircuitsFromInterlockingRules;

    // ? Check consistency between 2 sources
    if (!validateTrackCircuitConsistency(trackCircuitsFromSignalData, trackCircuitsFromInterlockingRules, result)) {
        return result;
    }

    // ? Use signal data as primary, fall back to interlocking rules
    QStringList authoritative = trackCircuitsFromSignalData.isEmpty() ?
                                    trackCircuitsFromInterlockingRules : trackCircuitsFromSignalData;

    if (authoritative.isEmpty()) {
        result.errorReason = "No protected track circuits found in any source";
        return result;
    }

    // ? UPDATED: Check track circuit occupancy status
    if (!validateTrackCircuitOccupancy(authoritative, result)) {
        return result;
    }

    // ? SUCCESS: All validations passed
    result.isValid = true;
    result.protectedTrackSegments = authoritative;  // Keep field name for compatibility

    qDebug() << "? SAFETY: Protected track circuits validation passed for signal" << signalId
             << "- Track Circuits:" << result.protectedTrackSegments;

    return result;
}

// ? UPDATED: Get protected track circuits from signal data
QStringList SignalBranch::getProtectedTrackCircuitsFromSignalData(const QString& signalId) {
    auto signalData = m_dbManager->getSignalById(signalId);
    if (signalData.isEmpty()) {
        qWarning() << " Signal data not found for:" << signalId;
        return QStringList();
    }

    //    The data is already parsed as QStringList in convertSignalRowToVariant()
    QVariant protectedTrackCircuitsVar = signalData["protectedTrackCircuits"];

    //  DEBUG: Log what we actually got
    qDebug() << " [SIGNAL] Protected circuits variant type:" << protectedTrackCircuitsVar.typeName()
             << "value:" << protectedTrackCircuitsVar;

    if (protectedTrackCircuitsVar.canConvert<QStringList>()) {
        QStringList result = protectedTrackCircuitsVar.toStringList();
        qDebug() << "  [SIGNAL] Extracted protected circuits:" << result;
        return result;
    }

    qDebug() << " [SIGNAL] Could not convert to QStringList";
    return QStringList();
}

// ? UPDATED: Use DatabaseManager API instead of direct SQL
QStringList SignalBranch::getProtectedTrackCircuitsFromInterlockingRules(const QString& signalId) {
    if (!m_dbManager) {
        qCritical() << " SAFETY CRITICAL: DatabaseManager not available for interlocking rules query";
        return QStringList();
    }

    // ? CRITICAL: Use DatabaseManager API instead of direct database access
    return m_dbManager->getProtectedTrackCircuitsFromInterlockingRules(signalId);
}

// ? UPDATED: Track circuit consistency validation
bool SignalBranch::validateTrackCircuitConsistency(
    const QStringList& fromSignalData,
    const QStringList& fromInterlockingRules,
    ProtectedTrackSegmentsValidation& result) {

    bool hasSignalData = !fromSignalData.isEmpty();
    bool hasRulesData = !fromInterlockingRules.isEmpty();

    if (!hasSignalData && !hasRulesData) {
        result.errorReason = "No protected track circuits found in any source";
        return false;
    }

    // ? CONSISTENCY: If both sources have data, they should match
    if (hasSignalData && hasRulesData) {
        QStringList signalData = fromSignalData;
        QStringList rulesData = fromInterlockingRules;
        signalData.sort();
        rulesData.sort();

        if (signalData != rulesData) {
            result.errorReason = QString("Protected track circuits mismatch between signal_data and interlocking_rules");
            result.inconsistentSources = QStringList{"signal_data", "interlocking_rules"};

            qCritical() << " SAFETY CRITICAL: Protected track circuits inconsistency detected!";
            qCritical() << "   Signal data:" << signalData;
            qCritical() << "   Interlocking rules:" << rulesData;

            return false;
        }
    }

    qDebug() << "? SAFETY: All sources consistent for protected track circuits";
    return true;
}

// ? UPDATED: Track circuit occupancy validation
bool SignalBranch::validateTrackCircuitOccupancy(
    const QStringList& protectedTrackCircuits,
    ProtectedTrackSegmentsValidation& result) {

    QStringList occupiedCircuits;

    for (const QString& circuitId : protectedTrackCircuits) {
        // ? CRITICAL: Use DatabaseManager API instead of direct database access
        auto circuitData = m_dbManager->getTrackCircuitById(circuitId);
        if (circuitData.isEmpty()) {
            result.errorReason = QString("Protected track circuit %1 not found in database").arg(circuitId);
            qCritical() << " SAFETY CRITICAL: Protected track circuit not found:" << circuitId;
            return false;
        }

        if (circuitData["occupied"].toBool()) {
            occupiedCircuits.append(circuitId);
            QString occupiedBy = circuitData["occupiedBy"].toString();

            qWarning() << " SAFETY: Protected track circuit" << circuitId
                       << "is occupied by" << occupiedBy;
        }
    }

    if (!occupiedCircuits.isEmpty()) {
        result.errorReason = QString("Protected track circuits are occupied: %1")
        .arg(occupiedCircuits.join(", "));
        result.occupiedTrackSegments = occupiedCircuits;

        qCritical() << " SAFETY CRITICAL: Cannot clear signal - protected track circuits occupied:"
                    << occupiedCircuits;
        return false;
    }

    qDebug() << "? SAFETY: All protected track circuits are clear";
    return true;
}
