#pragma once
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <memory>

class SignalRule {
public:
    struct Condition {
        QString entityType;     // "point_machine", "track_segment"
        QString entityId;
        QString requiredState;  // "NORMAL", "REVERSE", "CLEAR", etc.

        bool isValid() const { return !entityType.isEmpty() && !entityId.isEmpty(); }
    };

    struct AllowedSignal {
        QString signalId;
        QStringList allowedAspects;
    };

    //   SAFETY: Immutable data structure for thread safety
    SignalRule(const QString& whenAspect,
               const QList<Condition>& conditions,
               const QList<AllowedSignal>& allowedSignals);

    QString getWhenAspect() const { return m_whenAspect; }
    const QList<Condition>& getConditions() const { return m_conditions; }
    const QList<AllowedSignal>& getAllowedSignals() const { return m_allowedSignals; }

    //   PERFORMANCE: Quick lookup for validation
    bool isSignalAspectAllowed(const QString& signalId, const QString& aspect) const;

private:
    QString m_whenAspect;
    QList<Condition> m_conditions;
    QList<AllowedSignal> m_allowedSignals;

    //   PERFORMANCE: Pre-computed lookup map
    mutable QHash<QString, QStringList> m_aspectLookupCache;
    mutable bool m_cacheBuilt = false;

    void buildLookupCache() const;
};
