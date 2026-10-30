#include <validation.hpp>

#include <arpa/inet.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include <objects.hpp>
#include <status.hpp>

namespace yapi::validation {

using userver::formats::json::Value;

namespace {

using Problems = std::vector<std::string>;

std::string Quote(const std::string& s) { return "\"" + s + "\""; }

void ConfigKeys(const Value& object, Problems& p) {
    static const std::regex kKey("^[-._a-zA-Z0-9]+$");
    std::set<std::string> seen;
    for (const char* field : {"data", "binaryData", "stringData"}) {
        const auto map = object[field];
        if (!map.IsObject()) continue;
        for (auto it = map.begin(); it != map.end(); ++it) {
            const auto key = it.GetName();
            const std::string path = std::string(field) + "[" + key + "]";
            if (key.empty() || key.size() > 253 || !std::regex_match(key, kKey)) {
                p.push_back(path + ": Invalid value: " + Quote(key) +
                            ": a valid config key must consist of alphanumeric characters, '-', '_' or '.' (e.g. 'key.name',  or 'KEY_NAME',  or 'key-name', regex used for validation is '[-._a-zA-Z0-9]+')");
            } else if (key == "." || key == "..") {
                p.push_back(path + ": Invalid value: " + Quote(key) + ": must not be '.' or '..'");
            }
            if (std::string(field) != "stringData" && !seen.insert(key).second) {
                p.push_back(path + ": Invalid value: " + Quote(key) + ": duplicate of key present in data");
            }
        }
    }
}

bool DnsLabel(const std::string& s) {
    static const std::regex kLabel("^[a-z0-9]([-a-z0-9]*[a-z0-9])?$");
    return !s.empty() && s.size() <= 63 && std::regex_match(s, kLabel);
}

void PodSpec(const Value& spec, const std::string& at, Problems& p) {
    static const std::regex kSysctl("^([a-z0-9]([-_a-z0-9]*[a-z0-9])?[\\./])*[a-z0-9]([-_a-z0-9]*[a-z0-9])?$");
    std::size_t i = 0;
    for (const auto& s : objects::Items(spec["securityContext"]["sysctls"])) {
        auto name = s["name"].As<std::string>("");
        auto path = at + ".securityContext.sysctls[" + std::to_string(i++) + "].name";
        if (name.empty()) {
            p.push_back(path + ": Required value");
        } else if (name.size() > 253 || !std::regex_match(name, kSysctl)) {
            p.push_back(path + ": Invalid value: " + Quote(name) +
                        ": must have at most 253 characters and match regex ^([a-z0-9]([-_a-z0-9]*[a-z0-9])?[\\./])*[a-z0-9]([-_a-z0-9]*[a-z0-9])?$");
        }
    }
    std::set<std::string> names;
    for (const char* list : {"initContainers", "containers"}) {
        std::size_t j = 0;
        for (const auto& c : objects::Items(spec[list])) {
            auto path = at + "." + list + "[" + std::to_string(j++) + "]";
            auto name = c["name"].As<std::string>("");
            if (name.empty()) {
                p.push_back(path + ".name: Required value");
            } else if (!DnsLabel(name)) {
                p.push_back(path + ".name: Invalid value: " + Quote(name) +
                            ": a lowercase RFC 1123 label must consist of lower case alphanumeric characters or '-', and must start and end with an alphanumeric character (e.g. 'my-name',  or '123-abc', regex used for validation is '[a-z0-9]([-a-z0-9]*[a-z0-9])?')");
            } else if (!names.insert(name).second) {
                p.push_back(path + ".name: Duplicate value: " + Quote(name));
            }
            if (c["image"].As<std::string>("").empty()) p.push_back(path + ".image: Required value");
        }
    }
    if (objects::Items(spec["containers"]).IsEmpty()) p.push_back(at + ".containers: Required value");
    std::size_t v = 0;
    for (const auto& vol : objects::Items(spec["volumes"])) {
        std::size_t s = 0;
        for (const auto& src : objects::Items(vol["projected"]["sources"])) {
            const auto pc = src["podCertificate"];
            if (pc.IsObject() && pc.HasMember("maxExpirationSeconds")) {
                auto secs = pc["maxExpirationSeconds"].As<std::int64_t>(0);
                auto path = at + ".volumes[" + std::to_string(v) + "].projected.sources[" + std::to_string(s) + "].podCertificate.maxExpirationSeconds";
                // Signers of kubernetes.io issue for a day at most; others for 91 days.
                const std::int64_t most = pc["signerName"].As<std::string>("").starts_with("kubernetes.io/") ? 24 * 3600 : 91 * 24 * 3600;
                if (secs < 3600) p.push_back(path + ": Invalid value: " + std::to_string(secs) + ": if provided, maxExpirationSeconds must be >= 3600");
                if (secs > most) p.push_back(path + ": Invalid value: " + std::to_string(secs) + ": if provided, maxExpirationSeconds must be <= " + std::to_string(most));
            }
            ++s;
        }
        ++v;
    }
}

bool CanonicalIp(const std::string& s) {
    unsigned char buf[16];
    char back[INET6_ADDRSTRLEN];
    if (inet_pton(AF_INET, s.c_str(), buf) == 1) return inet_ntop(AF_INET, buf, back, sizeof(back)) && s == back;
    if (inet_pton(AF_INET6, s.c_str(), buf) == 1) return inet_ntop(AF_INET6, buf, back, sizeof(back)) && s == back;
    return false;
}

}  // namespace

void Validate(const Resource& r, const Value& object) {
    Problems p;
    const bool core = r.group.empty();
    if (core && (r.plural == "configmaps" || r.plural == "secrets")) ConfigKeys(object, p);
    if (core && r.plural == "pods") PodSpec(object["spec"], "spec", p);
    if (core && r.plural == "podtemplates") PodSpec(object["template"]["spec"], "template.spec", p);
    if ((r.group == "apps" || r.group == "batch" || (core && r.plural == "replicationcontrollers")) && object["spec"]["template"]["spec"].IsObject()) {
        PodSpec(object["spec"]["template"]["spec"], "spec.template.spec", p);
    }
    if (r.group == "networking.k8s.io" && r.plural == "ipaddresses") {
        auto name = objects::Name(object);
        if (!CanonicalIp(name)) p.push_back("metadata.name: Invalid value: " + Quote(name) + ": must be a valid IP address in canonical form");
    }
    if (p.empty()) return;
    std::string msg = p.size() == 1 ? p[0] : "[";
    if (p.size() > 1) {
        for (std::size_t i = 0; i < p.size(); ++i) msg += (i ? ", " : "") + p[i];
        msg += "]";
    }
    throw Invalid(r.kind, r.group, objects::Name(object), msg);
}

void ValidateUpdate(const Resource& r, const Value& old_object, const Value& object) {
    if (!(r.group.empty() && (r.plural == "configmaps" || r.plural == "secrets"))) return;
    if (!old_object["immutable"].As<bool>(false)) return;
    Problems p;
    if (!object["immutable"].As<bool>(false)) p.push_back("immutable: Forbidden: field is immutable when `immutable` is set");
    for (const char* field : {"data", "binaryData", "type"}) {
        const auto a = old_object[field], b = object[field];
        const bool same = (a.IsMissing() && b.IsMissing()) || (!a.IsMissing() && !b.IsMissing() && a == b);
        if (!same) p.push_back(std::string(field) + ": Forbidden: field is immutable when `immutable` is set");
    }
    if (p.empty()) return;
    std::string msg = p.size() == 1 ? p[0] : "[";
    if (p.size() > 1) {
        for (std::size_t i = 0; i < p.size(); ++i) msg += (i ? ", " : "") + p[i];
        msg += "]";
    }
    throw Invalid(r.kind, r.group, objects::Name(object), msg);
}

}  // namespace yapi::validation
