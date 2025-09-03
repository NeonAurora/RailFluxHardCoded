#pragma once
#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <QStringList>
#include "SignalRule.h"
#include "InterlockingService.h"

class DatabaseManager;

class InterlockingRuleEngine : public QObject {
    Q_OBJECT

public:
    explicit InterlockingRuleEngine(DatabaseManager* dbManager, QObject* parent = nullptr);

    bool loadRulesFromResource(const QString& resourcePath = ":/resources/data/signal_interlocking_rules.json");

    ValidationResult validateInterlockedSignalAspectChange(const QString& signalId,
                                                           const QString& currentAspect,
                                                           const QString& requestedAspect);

    // Information queries
    QStringList getControlledSignals(const QString& signalId) const;
    QStringList getControllingSignals(const QString& signalId) const;
    bool isSignalIndependent(const QString& signalId) const;
    QStringList getAspectsPermittedByController(
        const QString& controllerSignalId,
        const QString& controllerAspect,
        const QString& controlledSignalId
        );

private:
    DatabaseManager* m_dbManager;

    struct SignalInfo {
        QString signalType;
        bool isIndependent = false;
        QString controlMode; //   Added field
        QStringList controlledBy;
        QList<SignalRule> rules;
    };

    QHash<QString, SignalInfo> m_signalRules;

    // Helper methods
    ValidationResult validateControllingSignals(const QString& signalId,
                                                const QString& requestedAspect);
    bool checkConditions(const QList<SignalRule::Condition>& conditions);
    QString getCurrentSignalAspect(const QString& signalId);
    QString getCurrentPointPosition(const QString& pointId);

    // JSON parsing
    bool parseJsonRules(const QJsonObject& rulesObject);
    SignalRule parseRule(const QJsonObject& ruleObject);
    SignalRule::Condition parseCondition(const QJsonObject& conditionObject);
    SignalRule::AllowedSignal parseAllowedSignal(const QString& signalId, const QJsonArray& aspectsArray);

    //   NEW: Composite aspect evaluation
    QString getCurrentCompositeAspect(const QString& signalId);
    bool isCompositeAspect(const QString& aspect);
    QVariantMap parseCompositeAspect(const QString& compositeAspect);
    bool doesSignalMatchCompositeAspect(const QString& signalId, const QString& compositeAspect);
};
