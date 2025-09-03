#include "InterlockingRuleEngine.h"
#include "../database/DatabaseManager.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QFile>
#include <QDebug>

InterlockingRuleEngine::InterlockingRuleEngine(DatabaseManager* dbManager, QObject* parent)
    : QObject(parent), m_dbManager(dbManager) {
    if (!dbManager) {
        qCritical() << " [Constructor] InterlockingRuleEngine initialized with null DatabaseManager!";
    }
}

// === VALIDATION METHODS ===

bool InterlockingRuleEngine::checkConditions(const QList<SignalRule::Condition>& conditions) {
    for (const SignalRule::Condition& condition : conditions) {
        if (condition.entityType == "point_machine") {
            QString currentPosition = getCurrentPointPosition(condition.entityId);
            if (currentPosition != condition.requiredState) {
                qWarning() << " [checkConditions] Point machine" << condition.entityId
                           << "is" << currentPosition << "but requires" << condition.requiredState;
                return false;
            }
        }
        else if (condition.entityType == "track_segment") {
            // Future implementation for track segment occupancy conditions
        }
    }
    return true;
}

bool InterlockingRuleEngine::doesSignalMatchCompositeAspect(const QString& signalId, const QString& compositeAspect) {
    if (!isCompositeAspect(compositeAspect)) {
        QString currentMainAspect = getCurrentSignalAspect(signalId);
        return currentMainAspect == compositeAspect;
    }

    auto requiredComponents = parseCompositeAspect(compositeAspect);
    auto signalData = m_dbManager->getSignalById(signalId);

    QString currentMainAspect = signalData.value("currentAspect", "RED").toString();
    QString currentCallingOn = signalData.value("callingOnAspect", "OFF").toString();
    QString currentLoop = signalData.value("loopAspect", "OFF").toString();

    bool mainMatches = (currentMainAspect == requiredComponents["main"].toString());
    bool callingOnMatches = (currentCallingOn == requiredComponents["calling_on"].toString());
    bool loopMatches = (currentLoop == requiredComponents["loop"].toString());

    return mainMatches && callingOnMatches && loopMatches;
}

bool InterlockingRuleEngine::loadRulesFromResource(const QString& resourcePath) {
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly)) {
        qCritical() << " [loadRules] Cannot open interlocking rules file:" << resourcePath;
        return false;
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        qCritical() << " [loadRules] Invalid JSON in interlocking rules:" << parseError.errorString();
        return false;
    }

    QJsonObject rootObject = doc.object();
    QJsonObject rulesObject = rootObject["signal_interlocking_rules"].toObject();

    return parseJsonRules(rulesObject);
}

ValidationResult InterlockingRuleEngine::validateControllingSignals(const QString& signalId, const QString& requestedAspect) {
    auto signalInfoIt = m_signalRules.find(signalId);
    if (signalInfoIt == m_signalRules.end()) {
        qWarning() << " [validateControlling] Signal" << signalId << "not found in rules";
        return ValidationResult::blocked("Signal not found in rules", "SIGNAL_NOT_FOUND");
    }

    const SignalInfo& signalInfo = signalInfoIt.value();
    QString controlMode = signalInfo.controlMode.trimmed().toUpper();
    if (controlMode.isEmpty()) controlMode = "AND";

    bool anyControllingAllows = false;
    QStringList blockingReasons;

    for (const QString& controllingSignalId : signalInfo.controlledBy) {
        QString controllingCompositeAspect = getCurrentCompositeAspect(controllingSignalId);

        auto controllingInfoIt = m_signalRules.find(controllingSignalId);
        if (controllingInfoIt == m_signalRules.end()) {
            continue;
        }

        const SignalInfo& controllingInfo = controllingInfoIt.value();
        bool aspectAllowed = false;

        for (const SignalRule& rule : controllingInfo.rules) {
            QString ruleWhenAspect = rule.getWhenAspect();

            if (doesSignalMatchCompositeAspect(controllingSignalId, ruleWhenAspect)) {
                if (!checkConditions(rule.getConditions())) {
                    blockingReasons.append(
                        QString("Conditions not met for rule when %1 shows %2")
                            .arg(controllingSignalId, ruleWhenAspect));
                    continue;
                }

                if (rule.isSignalAspectAllowed(signalId, requestedAspect)) {
                    aspectAllowed = true;
                    break;
                }
            }
        }

        if (controlMode == "AND") {
            if (!aspectAllowed) {
                qWarning() << " [validateControlling] AND mode blocked by" << controllingSignalId;
                return ValidationResult::blocked(
                           QString("Signal %1 cannot show %2: controlling signal %3 shows %4")
                               .arg(signalId, requestedAspect, controllingSignalId, controllingCompositeAspect),
                           "CONTROLLING_SIGNAL_RESTRICTION"
                           ).addAffectedEntity(controllingSignalId);
            }
        }
        else if (controlMode == "OR") {
            if (aspectAllowed) {
                anyControllingAllows = true;
            }
        }
    }

    if (controlMode == "OR") {
        if (!anyControllingAllows) {
            qWarning() << " [validateControlling] OR mode - no controlling signals allow" << signalId;
            return ValidationResult::blocked(
                QString("Signal %1 cannot show %2: no controlling signals allow it")
                    .arg(signalId, requestedAspect),
                "CONTROLLING_SIGNAL_RESTRICTION"
                );
        }
    }

    return ValidationResult::allowed("All controlling signals permit the requested aspect");
}

ValidationResult InterlockingRuleEngine::validateInterlockedSignalAspectChange(
    const QString& signalId, const QString& currentAspect, const QString& requestedAspect) {

    auto signalInfoIt = m_signalRules.find(signalId);
    if (signalInfoIt == m_signalRules.end()) {
        qWarning() << " [validateAspectChange] Signal" << signalId << "not found in interlocking rules";
        return ValidationResult::blocked(
            QString("Signal %1 not found in interlocking rules").arg(signalId),
            "SIGNAL_NOT_IN_RULES"
            );
    }

    const SignalInfo& signalInfo = signalInfoIt.value();

    if (signalInfo.isIndependent) {
        return ValidationResult::allowed("Independent signal - no interlocking restrictions");
    }

    return validateControllingSignals(signalId, requestedAspect);
}

// === UTILITY METHODS ===

QString InterlockingRuleEngine::getCurrentCompositeAspect(const QString& signalId) {
    if (!m_dbManager) {
        qWarning() << " [getCurrentComposite] Database manager not available";
        return "RED";
    }

    auto signalData = m_dbManager->getSignalById(signalId);
    QString mainAspect = signalData.value("currentAspect", "RED").toString();
    QString callingOnAspect = signalData.value("callingOnAspect", "OFF").toString();
    QString loopAspect = signalData.value("loopAspect", "OFF").toString();

    QString compositeAspect = mainAspect;
    if (callingOnAspect == "WHITE") compositeAspect += "_CALLING";
    if (loopAspect == "YELLOW") compositeAspect += "_LOOP";

    return compositeAspect;
}

QString InterlockingRuleEngine::getCurrentPointPosition(const QString& pointId) {
    if (!m_dbManager) {
        qWarning() << " [getCurrentPoint] Database manager not available";
        return "NORMAL";
    }

    auto pointData = m_dbManager->getPointMachineById(pointId);
    return pointData.value("position", "NORMAL").toString();
}

QString InterlockingRuleEngine::getCurrentSignalAspect(const QString& signalId) {
    if (!m_dbManager) {
        qWarning() << " [getCurrentSignal] Database manager not available";
        return "RED";
    }

    auto signalData = m_dbManager->getSignalById(signalId);
    return signalData.value("currentAspect", "RED").toString();
}

QStringList InterlockingRuleEngine::getControlledSignals(const QString& signalId) const {
    QStringList controlled;
    auto signalInfoIt = m_signalRules.find(signalId);
    if (signalInfoIt != m_signalRules.end()) {
        const SignalInfo& signalInfo = signalInfoIt.value();
        for (const SignalRule& rule : signalInfo.rules) {
            for (const SignalRule::AllowedSignal& allowedSignal : rule.getAllowedSignals()) {
                if (!controlled.contains(allowedSignal.signalId)) {
                    controlled.append(allowedSignal.signalId);
                }
            }
        }
    }
    return controlled;
}

QStringList InterlockingRuleEngine::getControllingSignals(const QString& signalId) const {
    auto signalInfoIt = m_signalRules.find(signalId);
    if (signalInfoIt != m_signalRules.end()) {
        return signalInfoIt.value().controlledBy;
    }
    return QStringList();
}

bool InterlockingRuleEngine::isCompositeAspect(const QString& aspect) {
    return aspect.contains("_CALLING") || aspect.contains("_LOOP");
}

bool InterlockingRuleEngine::isSignalIndependent(const QString& signalId) const {
    auto signalInfoIt = m_signalRules.find(signalId);
    if (signalInfoIt != m_signalRules.end()) {
        return signalInfoIt.value().isIndependent;
    }
    return false;
}

QVariantMap InterlockingRuleEngine::parseCompositeAspect(const QString& compositeAspect) {
    QVariantMap components;
    QString aspect = compositeAspect;

    if (aspect.contains("_CALLING")) {
        components["calling_on"] = "WHITE";
        aspect = aspect.replace("_CALLING", "");
    } else {
        components["calling_on"] = "OFF";
    }

    if (aspect.contains("_LOOP")) {
        components["loop"] = "YELLOW";
        aspect = aspect.replace("_LOOP", "");
    } else {
        components["loop"] = "OFF";
    }

    components["main"] = aspect.isEmpty() ? "RED" : aspect;
    return components;
}

// === PARSING METHODS ===

SignalRule::AllowedSignal InterlockingRuleEngine::parseAllowedSignal(const QString& signalId, const QJsonArray& aspectsArray) {
    SignalRule::AllowedSignal allowedSignal;
    allowedSignal.signalId = signalId;

    for (const QJsonValue& aspectValue : aspectsArray) {
        QString aspect = aspectValue.toString();
        if (!aspect.isEmpty()) {
            allowedSignal.allowedAspects.append(aspect);
        }
    }

    return allowedSignal;
}

SignalRule::Condition InterlockingRuleEngine::parseCondition(const QJsonObject& conditionObject) {
    SignalRule::Condition condition;

    if (conditionObject.contains("point_machine")) {
        condition.entityType = "point_machine";
        condition.entityId = conditionObject["point_machine"].toString();
        condition.requiredState = conditionObject["position"].toString();
    }
    else if (conditionObject.contains("track_segment")) {
        condition.entityType = "track_segment";
        condition.entityId = conditionObject["track_segment"].toString();
        condition.requiredState = conditionObject["occupancy"].toString();
    }
    else {
        qWarning() << " [parseCondition] Unknown condition type:" << conditionObject.keys();
        condition.entityType = "unknown";
    }

    return condition;
}

bool InterlockingRuleEngine::parseJsonRules(const QJsonObject& rulesObject) {
    m_signalRules.clear();

    for (auto it = rulesObject.begin(); it != rulesObject.end(); ++it) {
        QString signalId = it.key();
        QJsonObject signalObject = it.value().toObject();

        SignalInfo signalInfo;
        signalInfo.signalType = signalObject["type"].toString();
        signalInfo.isIndependent = signalObject["independent"].toBool(false);
        signalInfo.controlMode = signalObject["control_mode"].toString();

        QJsonArray controlledByArray = signalObject["controlled_by"].toArray();
        for (const QJsonValue& value : controlledByArray) {
            signalInfo.controlledBy.append(value.toString());
        }

        QJsonArray rulesArray = signalObject["rules"].toArray();
        for (const QJsonValue& ruleValue : rulesArray) {
            QJsonObject ruleObject = ruleValue.toObject();
            SignalRule rule = parseRule(ruleObject);
            signalInfo.rules.append(rule);
        }

        m_signalRules[signalId] = signalInfo;
    }

    return true;
}

SignalRule InterlockingRuleEngine::parseRule(const QJsonObject& ruleObject) {
    QString whenAspect = ruleObject["when_aspect"].toString();

    QList<SignalRule::Condition> conditions;
    QJsonArray conditionsArray = ruleObject["conditions"].toArray();
    for (const QJsonValue& condValue : conditionsArray) {
        QJsonObject condObject = condValue.toObject();
        conditions.append(parseCondition(condObject));
    }

    QList<SignalRule::AllowedSignal> allowedSignals;
    QJsonObject allowsObject = ruleObject["allows"].toObject();
    for (auto it = allowsObject.begin(); it != allowsObject.end(); ++it) {
        QString signalId = it.key();
        QJsonArray aspectsArray = it.value().toArray();
        allowedSignals.append(parseAllowedSignal(signalId, aspectsArray));
    }

    return SignalRule(whenAspect, conditions, allowedSignals);
}

// Add this function to InterlockingRuleEngine.cpp
QStringList InterlockingRuleEngine::getAspectsPermittedByController(
    const QString& controllerSignalId,
    const QString& controllerAspect,
    const QString& controlledSignalId)
{
    qDebug() << " [RULE_ENGINE] Evaluating what" << controllerSignalId
             << "(" << controllerAspect << ") allows for" << controlledSignalId;

    // Find the controller signal's rules
    auto signalInfoIt = m_signalRules.find(controllerSignalId);
    if (signalInfoIt == m_signalRules.end()) {
        qWarning() << " [RULE_ENGINE] Controller signal" << controllerSignalId << "not found in rules";
        return QStringList{"RED"}; // Safe fallback
    }

    const SignalInfo& signalInfo = signalInfoIt.value();

    // Look for rules that match the controller's current aspect
    for (const SignalRule& rule : signalInfo.rules) {
        if (rule.getWhenAspect() == controllerAspect) {
            qDebug() << "   Found matching rule for aspect:" << controllerAspect;

            // Check if all conditions are met (e.g., point machine positions)
            if (!checkConditions(rule.getConditions())) {
                qDebug() << "    Conditions not met for rule, skipping";
                continue; // Try next rule
            }

            // Find allowed aspects for the controlled signal
            for (const SignalRule::AllowedSignal& allowedSignal : rule.getAllowedSignals()) {
                if (allowedSignal.signalId == controlledSignalId) {
                    qDebug() << "    " << controllerSignalId << "(" << controllerAspect
                             << ") allows" << controlledSignalId << ":" << allowedSignal.allowedAspects;
                    return allowedSignal.allowedAspects;
                }
            }
        }
    }

    // No matching rule found
    qWarning() << "    No matching rule found, defaulting to RED";
    return QStringList{"RED"}; // Safe fallback
}
