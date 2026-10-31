#include <aggregator.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

#include <userver/clients/http/component.hpp>
#include <userver/clients/http/connect_to.hpp>
#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/crypto/base64.hpp>
#include <userver/crypto/certificate.hpp>
#include <userver/crypto/private_key.hpp>
#include <userver/formats/json/serialize.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/http/url.hpp>
#include <userver/logging/log.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

#include <objects.hpp>
#include <registry.hpp>
#include <routing.hpp>
#include <status.hpp>
#include <store.hpp>

namespace yapi {

namespace json = userver::formats::json;
using json::Value;
using json::ValueBuilder;

namespace {

constexpr auto kResourcesFor = std::chrono::seconds(10);
// A server that does not answer discovery within this is unavailable, and
// the checks of the others do not wait on it longer.
constexpr auto kCheckTimeout = std::chrono::seconds(5);

std::string ReadFile(const std::string& path) {
    if (path.empty()) return "";
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const Resource& Kind(const char* group, const char* plural) {
    return *Registry::Builtin().Find(group, "v1", plural);
}

}  // namespace

Aggregator::Aggregator(const userver::components::ComponentConfig& config, const userver::components::ComponentContext& context)
    : ComponentBase(config, context),
      store_(context.FindComponent<Store>()),
      http_(context.FindComponent<userver::components::HttpClient>().GetHttpClient()),
      proxy_cert_(ReadFile(config["proxy-client-cert-file"].As<std::string>(""))),
      proxy_key_(ReadFile(config["proxy-client-key-file"].As<std::string>(""))) {
    PublishAuthentication(
        ReadFile(config["client-ca-file"].As<std::string>("")), ReadFile(config["requestheader-client-ca-file"].As<std::string>(""))
    );
    availability_.Start("apiservice-availability", {std::chrono::seconds(2)}, [this] { CheckAvailability(); });
}

Aggregator::~Aggregator() { availability_.Stop(); }

void Aggregator::Kick() const { availability_.ForceStepAsync(); }

userver::yaml_config::Schema Aggregator::GetStaticConfigSchema() {
    return userver::yaml_config::MergeSchemas<ComponentBase>(R"(
type: object
description: API groups served by other servers
additionalProperties: false
properties:
    proxy-client-cert-file:
        type: string
        description: the certificate this server presents to the servers it forwards to
    proxy-client-key-file:
        type: string
        description: its key
    client-ca-file:
        type: string
        description: the CA of client certificates, published for the servers behind
    requestheader-client-ca-file:
        type: string
        description: the CA of the proxy certificate, published for the servers behind
)");
}

// Servers behind the aggregator learn from this ConfigMap whom to trust
// for the identity headers, as they do from kube-apiserver's.
void Aggregator::PublishAuthentication(const std::string& client_ca, const std::string& requestheader_ca) const {
    if (requestheader_ca.empty()) return;
    const auto& cms = Kind("", "configmaps");
    ValueBuilder b(json::Type::kObject);
    b["metadata"]["name"] = "extension-apiserver-authentication";
    if (!client_ca.empty()) b["data"]["client-ca-file"] = client_ca;
    b["data"]["requestheader-client-ca-file"] = requestheader_ca;
    b["data"]["requestheader-allowed-names"] = R"(["front-proxy-client"])";
    b["data"]["requestheader-username-headers"] = R"(["X-Remote-User"])";
    b["data"]["requestheader-uid-headers"] = R"(["X-Remote-Uid"])";
    b["data"]["requestheader-group-headers"] = R"(["X-Remote-Group"])";
    b["data"]["requestheader-extra-headers-prefix"] = R"(["X-Remote-Extra-"])";
    const auto fresh = objects::PrepareCreate(cms, b.ExtractValue(), "kube-system");
    store_.Write(cms, "kube-system", "extension-apiserver-authentication", [&](const std::optional<Value>& current) {
        if (!current) return Decision::Put(fresh);
        if ((*current)["data"] == fresh["data"]) return Decision::Keep();
        ValueBuilder updated(*current);
        updated["data"] = fresh["data"];
        return Decision::Put(updated.ExtractValue());
    });
}

std::optional<Aggregator::Remote> Aggregator::Find(const std::string& group, const std::string& version) const {
    if (group.empty()) return std::nullopt;
    auto svc = store_.Get(Kind("apiregistration.k8s.io", "apiservices"), "", version + "." + group);
    if (!svc || !(*svc)["spec"]["service"].IsObject()) return std::nullopt;
    return Remote{group, version, *svc};
}

std::vector<Aggregator::Remote> Aggregator::All() const {
    std::vector<Remote> out;
    for (const auto& svc : store_.List(Kind("apiregistration.k8s.io", "apiservices"), "", 0, "", "").items) {
        if (!svc["spec"]["service"].IsObject()) continue;
        out.push_back({svc["spec"]["group"].As<std::string>(""), svc["spec"]["version"].As<std::string>(""), svc});
    }
    auto group_priority = [](const Remote& r) { return r.apiservice["spec"]["groupPriorityMinimum"].As<std::int64_t>(0); };
    auto version_priority = [](const Remote& r) { return r.apiservice["spec"]["versionPriority"].As<std::int64_t>(0); };
    std::stable_sort(out.begin(), out.end(), [&](const Remote& a, const Remote& b) {
        if (a.group != b.group) {
            if (group_priority(a) != group_priority(b)) return group_priority(a) > group_priority(b);
            return a.group < b.group;
        }
        return version_priority(a) > version_priority(b);
    });
    return out;
}

std::shared_ptr<userver::clients::http::Response> Aggregator::Send(
    const Remote& remote,
    const std::string& method,
    const std::string& path_and_query,
    const std::string& body,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const User* user,
    std::chrono::seconds timeout
) const {
    const auto svc = remote.apiservice["spec"]["service"];
    const auto ns = svc["namespace"].As<std::string>("");
    const auto name = svc["name"].As<std::string>("");
    const auto port = svc["port"].As<int>(443);
    const auto to = ServiceDestination(store_, ns, name, port);
    // The certificate names the service; the connection goes to an endpoint.
    const auto host = name + "." + ns + ".svc";
    namespace client = userver::clients::http;
    const client::ConnectTo connect_to(host + ":" + std::to_string(port) + ":" + to.ip + ":" + std::to_string(to.port));
    auto out = http_.CreateRequest();
    out.method(client::HttpMethodFromString(method))
        .url("https://" + host + ":" + std::to_string(port) + path_and_query)
        .timeout(timeout)
        .connect_to(connect_to);
    if (!body.empty()) out.data(body);
    const auto ca = remote.apiservice["spec"]["caBundle"].As<std::string>("");
    if (remote.apiservice["spec"]["insecureSkipTLSVerify"].As<bool>(false)) {
        out.verify(false);
    } else if (!ca.empty()) {
        out.ca(userver::crypto::Certificate::LoadFromString(userver::crypto::base64::Base64Decode(ca)));
    }
    if (!proxy_cert_.empty() && !proxy_key_.empty()) {
        out.client_key_cert(
            userver::crypto::PrivateKey::LoadFromString(proxy_key_), userver::crypto::Certificate::LoadFromString(proxy_cert_)
        );
    }
    client::Headers h;
    for (const auto& [k, v] : headers) h.emplace(k, v);
    if (user) {
        h.emplace(std::string("X-Remote-User"), user->name);
        if (!user->uid.empty()) h.emplace(std::string("X-Remote-Uid"), user->uid);
        // The client sends one line per header name, so one group: the
        // server behind asks this one to authorize anyway.
        if (!user->groups.empty()) h.emplace(std::string("X-Remote-Group"), user->groups.front());
    }
    out.headers(h);
    try {
        return out.perform();
    } catch (const std::exception& e) {
        throw ApiError(503, "ServiceUnavailable", "error trying to reach service: " + std::string(e.what()));
    }
}

std::string Aggregator::Forward(const userver::server::http::HttpRequest& request, const Remote& remote, const User& user) const {
    std::string query;
    for (const auto& arg : request.ArgNames()) {
        for (const auto& v : request.GetArgVector(arg)) {
            query += (query.empty() ? "?" : "&") + userver::http::UrlEncode(arg) + "=" + userver::http::UrlEncode(v);
        }
    }
    std::vector<std::pair<std::string, std::string>> headers;
    for (const auto& name : request.GetHeaderNames()) {
        std::string lower(name);
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        // Identity goes in the headers below, never from the caller.
        if (lower == "authorization" || lower == "host" || lower == "content-length" || lower == "connection" ||
            lower.starts_with("x-remote-")) {
            continue;
        }
        headers.emplace_back(std::string(name), request.GetHeader(name));
    }
    // A body of no stated type is JSON to an API server; left out, curl
    // would call it a form.
    const auto content_type = request.GetHeader(userver::http::headers::kContentType);
    if (!content_type.empty()) {
        headers.emplace_back("Content-Type", content_type);
    } else if (!request.RequestBody().empty()) {
        headers.emplace_back("Content-Type", "application/json");
    }
    auto answer = Send(remote, request.GetMethodStr(), request.GetRequestPath() + query, request.RequestBody(), headers, &user, std::chrono::seconds(60));
    // A server that answers may have just come up: its APIService says so
    // before the next period.
    if (answer->status_code() < 500) {
        std::lock_guard lock(mutex_);
        if (!seen_up_.count(remote.group + "/" + remote.version)) Kick();
    }
    auto& response = request.GetHttpResponse();
    response.SetStatus(static_cast<userver::server::http::HttpStatus>(static_cast<int>(answer->status_code())));
    for (const auto& [k, v] : answer->headers()) {
        std::string lower(k);
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "content-length" || lower == "transfer-encoding" || lower == "connection" || lower == "content-encoding") continue;
        response.SetHeader(std::string(k), v);
    }
    return answer->body();
}

std::optional<Value> Aggregator::Fetch(const Remote& remote, std::string& why) const {
    const auto key = remote.group + "/" + remote.version;
    try {
        // Asked as upstream's aggregator asks, under a name of its own.
        static const User kSelf{"system:kube-aggregator", "", {"system:masters"}, {}};
        auto answer = Send(remote, "GET", "/apis/" + key, "", {{"Accept", "application/json"}}, &kSelf, kCheckTimeout);
        if (answer->status_code() != userver::clients::http::Status::kOk) {
            why = "failing or missing response from /apis/" + key + ": bad status from /apis/" + key + ": " +
                  std::to_string(static_cast<int>(answer->status_code()));
            return std::nullopt;
        }
        auto resources = json::FromString(answer->body());
        std::lock_guard lock(mutex_);
        cache_[key] = {std::chrono::steady_clock::now(), resources};
        return resources;
    } catch (const std::exception& e) {
        why = "failing or missing response from /apis/" + key + ": " + e.what();
        return std::nullopt;
    }
}

std::optional<Value> Aggregator::Resources(const Remote& remote) const {
    {
        std::lock_guard lock(mutex_);
        auto it = cache_.find(remote.group + "/" + remote.version);
        if (it != cache_.end() && std::chrono::steady_clock::now() - it->second.at < kResourcesFor) return it->second.resources;
    }
    std::string why;
    return Fetch(remote, why);
}

void Aggregator::CheckAvailability() {
    const auto& kind = Kind("apiregistration.k8s.io", "apiservices");
    for (const auto& svc : store_.List(kind, "", 0, "", "").items) {
        ValueBuilder want(json::Type::kObject);
        want["type"] = "Available";
        if (!svc["spec"]["service"].IsObject()) {
            want["status"] = "True";
            want["reason"] = "Local";
            want["message"] = "Local APIServices are always available";
        } else {
            std::string why;
            Remote remote{svc["spec"]["group"].As<std::string>(""), svc["spec"]["version"].As<std::string>(""), svc};
            const bool up = Fetch(remote, why).has_value();
            {
                std::lock_guard lock(mutex_);
                if (up) {
                    seen_up_.insert(remote.group + "/" + remote.version);
                } else {
                    seen_up_.erase(remote.group + "/" + remote.version);
                }
            }
            want["status"] = up ? "True" : "False";
            want["reason"] = up ? "Passed" : "FailedDiscoveryCheck";
            want["message"] = up ? "all checks passed" : why;
        }
        const auto condition = want.ExtractValue();
        store_.Write(kind, "", objects::Name(svc), [&](const std::optional<Value>& current) {
            if (!current) return Decision::Keep();
            ValueBuilder updated(*current);
            ValueBuilder conditions(json::Type::kArray);
            bool same = false;
            for (const auto& c : objects::Items((*current)["status"]["conditions"])) {
                if (c["type"].As<std::string>("") != "Available") {
                    conditions.PushBack(c);
                    continue;
                }
                same = c["status"] == condition["status"] && c["reason"] == condition["reason"] && c["message"] == condition["message"];
                if (same) conditions.PushBack(c);
            }
            if (same) return Decision::Keep();
            ValueBuilder fresh(condition);
            fresh["lastTransitionTime"] = objects::Now();
            // Available goes first, where clients look for it.
            ValueBuilder ordered(json::Type::kArray);
            ordered.PushBack(fresh.ExtractValue());
            for (const auto& c : conditions.ExtractValue()) ordered.PushBack(c);
            updated["status"]["conditions"] = ordered;
            return Decision::Put(objects::PrepareUpdate(kind, *current, updated.ExtractValue(), true));
        });
    }
}

}  // namespace yapi
