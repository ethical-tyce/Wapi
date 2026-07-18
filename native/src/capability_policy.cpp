#include "capability_policy.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <system_error>

namespace wapi::policy {
namespace {

std::string trimCopy(const std::string& value) {
    const std::size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    const std::size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::string lowerAsciiCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

[[noreturn]] void invalidRule(const std::string& expression, const std::string& reason) {
    throw std::runtime_error("E_CAPABILITY_RULE: " + reason + ": " + expression);
}

bool isPathCapability(const std::string& capability) {
    return capability == "file.write" || capability == "pe.inspect";
}

void validateCapabilityPattern(
    const std::string& pattern,
    const std::string& expression,
    RuleKind kind
) {
    if (pattern.empty()) invalidRule(expression, "capability name is empty");
    for (unsigned char ch : pattern) {
        if (std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-' || ch == '*') continue;
        invalidRule(expression, "capability name contains an invalid character");
    }

    const std::size_t wildcard = pattern.find('*');
    if (wildcard == std::string::npos) return;
    if (kind == RuleKind::Allow) invalidRule(expression, "wildcard capability grants are not allowed");
    if (pattern == "*" ||
        (wildcard == pattern.size() - 1 && wildcard >= 2 && pattern[wildcard - 1] == '.' &&
         pattern.find('*', wildcard + 1) == std::string::npos)) {
        return;
    }
    invalidRule(expression, "deny wildcard must be * or a terminal namespace.*");
}

std::string parseQuotedResource(const std::string& raw, const std::string& expression) {
    const std::string value = trimCopy(raw);
    if (value.size() < 2 || (value.front() != '"' && value.front() != '\'')) {
        invalidRule(expression, "resource scope must be one quoted string");
    }
    const char quote = value.front();
    if (value.back() != quote) invalidRule(expression, "resource scope has an unterminated quote");
    const std::string resource = value.substr(1, value.size() - 2);
    if (resource.empty()) invalidRule(expression, "resource scope is empty");
    if (resource.find(quote) != std::string::npos) {
        invalidRule(expression, "quoted resource contains trailing text");
    }
    return resource;
}

void validateResourcePattern(
    const std::string& capability,
    const std::string& resource,
    const std::string& expression
) {
    if (!isPathCapability(capability)) {
        invalidRule(expression, "resource scopes are currently supported only for file.write and pe.inspect");
    }
    if (resource.find('?') != std::string::npos) {
        invalidRule(expression, "resource wildcard must be a terminal /** on a supported path capability");
    }
    const std::size_t wildcard = resource.find('*');
    if (wildcard == std::string::npos) return;
    if (resource.size() < 3 ||
        resource.substr(resource.size() - 3) != "/**" ||
        wildcard != resource.size() - 2 || resource.find('*', wildcard + 2) != std::string::npos) {
        invalidRule(expression, "resource wildcard must be a terminal /** on a supported path capability");
    }
}

bool capabilityMatches(const CapabilityRule& rule, const std::string& capability) {
    const std::string normalisedCapability = lowerAsciiCopy(capability);
    if (rule.capabilityPattern == "*") return true;
    if (rule.capabilityPattern.ends_with(".*")) {
        const std::string prefix = rule.capabilityPattern.substr(0, rule.capabilityPattern.size() - 1);
        return normalisedCapability.rfind(prefix, 0) == 0;
    }
    return rule.capabilityPattern == normalisedCapability;
}

std::string normalisePathResource(const std::string& value) {
    std::error_code error;
    std::filesystem::path path(value);
    if (path.is_relative()) path = std::filesystem::absolute(path, error);
    if (error) path = std::filesystem::path(value);
    std::string normalised = lowerAsciiCopy(path.lexically_normal().generic_string());
    while (normalised.size() > 3 && normalised.back() == '/') normalised.pop_back();
    return normalised;
}

std::string normaliseGenericResource(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    return lowerAsciiCopy(value);
}

bool resourceMatches(const CapabilityRule& rule, const std::string& capability, const std::string& resource) {
    if (!rule.resourcePattern.has_value()) return true;
    if (!isPathCapability(capability)) {
        return normaliseGenericResource(*rule.resourcePattern) == normaliseGenericResource(resource);
    }

    std::string pattern = normalisePathResource(*rule.resourcePattern);
    const std::string candidate = normalisePathResource(resource);
    if (pattern.ends_with("/**")) {
        pattern.resize(pattern.size() - 3);
        return candidate == pattern ||
            (candidate.size() > pattern.size() && candidate.rfind(pattern + "/", 0) == 0);
    }
    return candidate == pattern;
}

} // namespace

CapabilityRule parseCapabilityRule(const std::string& expression, RuleKind kind) {
    const std::string trimmed = trimCopy(expression);
    if (trimmed.empty()) invalidRule(expression, "rule is empty");
    if (trimmed.size() > 4096) invalidRule(expression, "rule is too long");

    const std::size_t open = trimmed.find('(');
    if (open == std::string::npos) {
        validateCapabilityPattern(trimmed, expression, kind);
        return {trimmed, lowerAsciiCopy(trimmed), std::nullopt};
    }
    if (trimmed.back() != ')') invalidRule(expression, "resource scope must end with )");

    const std::string rawCapability = trimCopy(trimmed.substr(0, open));
    validateCapabilityPattern(rawCapability, expression, kind);
    const std::string capability = lowerAsciiCopy(rawCapability);
    const std::string resource = parseQuotedResource(
        trimmed.substr(open + 1, trimmed.size() - open - 2),
        expression
    );
    validateResourcePattern(capability, resource, expression);
    return {trimmed, capability, resource};
}

std::vector<CapabilityRule> parseCapabilityRules(
    const std::unordered_set<std::string>& expressions,
    RuleKind kind
) {
    std::vector<std::string> ordered(expressions.begin(), expressions.end());
    std::sort(ordered.begin(), ordered.end());
    std::vector<CapabilityRule> rules;
    rules.reserve(ordered.size());
    for (const auto& expression : ordered) rules.push_back(parseCapabilityRule(expression, kind));
    return rules;
}

std::string capabilityName(const std::string& expression, RuleKind kind) {
    return parseCapabilityRule(expression, kind).capabilityPattern;
}

CapabilityDecision evaluateCapability(
    const std::vector<CapabilityRule>& allowRules,
    const std::vector<CapabilityRule>& denyRules,
    const std::string& capability,
    const std::optional<std::string>& resource,
    bool resourceRequired
) {
    bool scopedDenyDeferred = false;
    for (const auto& rule : denyRules) {
        if (!capabilityMatches(rule, capability)) continue;
        if (!rule.resourcePattern.has_value()) return {DecisionKind::Deny, rule.expression};
        if (resource.has_value() && resourceMatches(rule, capability, *resource)) {
            return {DecisionKind::Deny, rule.expression};
        }
        if (resourceRequired && !resource.has_value()) scopedDenyDeferred = true;
    }

    const CapabilityRule* deferredAllow = nullptr;
    for (const auto& rule : allowRules) {
        if (!capabilityMatches(rule, capability)) continue;
        if (!rule.resourcePattern.has_value()) {
            if (resourceRequired && !resource.has_value() && scopedDenyDeferred) {
                return {DecisionKind::Deferred, rule.expression};
            }
            return {DecisionKind::Allow, rule.expression};
        }
        if (resource.has_value() && resourceMatches(rule, capability, *resource)) {
            return {DecisionKind::Allow, rule.expression};
        }
        if (resourceRequired && !resource.has_value() && deferredAllow == nullptr) deferredAllow = &rule;
    }
    if (deferredAllow != nullptr) return {DecisionKind::Deferred, deferredAllow->expression};
    return {DecisionKind::Missing, ""};
}

} // namespace wapi::policy
