#include "SignalRule.h"

SignalRule::SignalRule(const QString& whenAspect,
                       const QList<Condition>& conditions,
                       const QList<AllowedSignal>& allowedSignals)
    : m_whenAspect(whenAspect), m_conditions(conditions), m_allowedSignals(allowedSignals) {}

bool SignalRule::isSignalAspectAllowed(const QString& signalId, const QString& aspect) const {
    if (!m_cacheBuilt) {
        buildLookupCache();
    }

    auto it = m_aspectLookupCache.find(signalId);
    if (it == m_aspectLookupCache.end()) {
        return false; // Signal not controlled by this rule
    }

    return it.value().contains(aspect);
}

void SignalRule::buildLookupCache() const {
    m_aspectLookupCache.clear();

    for (const auto& allowedSignal : m_allowedSignals) {
        m_aspectLookupCache[allowedSignal.signalId] = allowedSignal.allowedAspects;
    }

    m_cacheBuilt = true;
}
