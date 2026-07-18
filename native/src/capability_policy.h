#pragma once

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace wapi::policy {

enum class RuleKind {
    Allow,
    Deny
};

struct CapabilityRule {
    std::string expression;
    std::string capabilityPattern;
    std::optional<std::string> resourcePattern;
};

enum class DecisionKind {
    Allow,
    Deny,
    Missing,
    Deferred
};

struct CapabilityDecision {
    DecisionKind kind = DecisionKind::Missing;
    std::string matchedRule;
};

CapabilityRule parseCapabilityRule(const std::string& expression, RuleKind kind = RuleKind::Allow);
std::vector<CapabilityRule> parseCapabilityRules(
    const std::unordered_set<std::string>& expressions,
    RuleKind kind
);
std::string capabilityName(const std::string& expression, RuleKind kind = RuleKind::Allow);

CapabilityDecision evaluateCapability(
    const std::vector<CapabilityRule>& allowRules,
    const std::vector<CapabilityRule>& denyRules,
    const std::string& capability,
    const std::optional<std::string>& resource,
    bool resourceRequired
);

} // namespace wapi::policy
