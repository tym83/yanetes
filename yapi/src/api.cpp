#include <api.hpp>

#include <fstream>
#include <functional>
#include <tuple>
#include <sstream>

#include <userver/components/component_config.hpp>
#include <userver/clients/http/component.hpp>
#include <userver/components/component_context.hpp>
#include <userver/crypto/base64.hpp>
#include <userver/engine/deadline.hpp>
#include <userver/engine/task/cancel.hpp>
#include <userver/formats/json/serialize.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/formats/serialize/common_containers.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/http/url.hpp>
#include <userver/logging/log.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_response_body_stream.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/utils/rand.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

#include <admission.hpp>
#include <auth.hpp>
#include <bodies.hpp>
#include <conversion.hpp>
#include <crds.hpp>
#include <kubelet.hpp>
#include <objects.hpp>
#include <openapi.hpp>
#include <patch.hpp>
#include <proto.hpp>
#include <schema.hpp>
#include <selectors.hpp>
#include <services.hpp>
#include <status.hpp>
#include <store.hpp>
#include <tables.hpp>
#include <validation.hpp>
#include <policies.hpp>
#include <webhooks.hpp>
#include <watch.hpp>

namespace yapi {

namespace json = userver::formats::json;
namespace http = userver::server::http;
using json::Value;
using json::ValueBuilder;

namespace {

constexpr std::string_view kVersion = "v1.37.1+ya";

const Resource& kNamespaces = *Registry::Builtin().Find("", "v1", "namespaces");

std::vector<std::string> Split(const std::string& path) {
    std::vector<std::string> out;
    std::stringstream ss(path);
    std::string part;
    while (std::getline(ss, part, '/')) {
        if (!part.empty()) out.push_back(part);
    }
    return out;
}

// What a resource path names.
struct Target {
    const Resource* resource = nullptr;
    std::string ns, name, sub;
};

std::vector<std::string> Verbs(const Resource& r) {
    if (!r.verbs.empty()) return r.verbs;
    return {"create", "delete", "deletecollection", "get", "list", "patch", "update", "watch"};
}

bool IsServiceAccount(const Resource& r) { return r.group.empty() && r.plural == "serviceaccounts"; }
bool IsPod(const Resource& r) { return r.group.empty() && r.plural == "pods"; }
bool IsService(const Resource& r) { return r.group.empty() && r.plural == "services"; }
bool IsWebhookConfig(const Resource& r) {
    return r.group == "admissionregistration.k8s.io" && (r.plural == "validatingwebhookconfigurations" || r.plural == "mutatingwebhookconfigurations");
}
bool IsDefinitions(const Resource& r) { return r.group == "apiextensions.k8s.io" && r.plural == "customresourcedefinitions"; }
bool IsNamespace(const Resource& r) { return r.group.empty() && r.plural == "namespaces"; }
// Pod subresources served by the pod's kubelet.
bool KubeletSub(const std::string& sub) { return sub == "log" || sub == "exec" || sub == "attach" || sub == "portforward"; }

bool HasScale(const Resource& r) {
    if (r.custom) return r.custom->scale;
    return (r.group == "apps" && (r.plural == "deployments" || r.plural == "replicasets" || r.plural == "statefulsets")) ||
           (r.group.empty() && r.plural == "replicationcontrollers");
}

// A field at a ".spec.replicas" path, as scale subresources name them.
Value AtPath(const Value& object, const std::string& path) {
    Value v = object;
    std::stringstream ss(path);
    std::string part;
    while (std::getline(ss, part, '.')) {
        if (part.empty()) continue;
        if (!v.IsObject() || !v.HasMember(part)) return Value();
        v = v[part];
    }
    return v;
}

Value SetAtPath(const Value& object, const std::string& path, const Value& value) {
    std::vector<std::string> parts;
    std::stringstream ss(path);
    for (std::string part; std::getline(ss, part, '.');) {
        if (!part.empty()) parts.push_back(part);
    }
    std::function<Value(const Value&, std::size_t)> set = [&](const Value& v, std::size_t i) -> Value {
        if (i == parts.size()) return value;
        ValueBuilder b(v.IsObject() ? v : ValueBuilder(json::Type::kObject).ExtractValue());
        b[parts[i]] = set(v.IsObject() && v.HasMember(parts[i]) ? v[parts[i]] : Value(), i + 1);
        return b.ExtractValue();
    };
    return set(object, 0);
}

// The autoscaling/v1 Scale of an object with replicas.
Value ScaleOf(const Resource& r, const Value& object) {
    ValueBuilder b(json::Type::kObject);
    b["kind"] = "Scale";
    b["apiVersion"] = "autoscaling/v1";
    for (const char* key : {"name", "namespace", "uid", "resourceVersion", "creationTimestamp"}) {
        if (object["metadata"].HasMember(key)) b["metadata"][key] = object["metadata"][key];
    }
    if (r.custom) {
        b["spec"]["replicas"] = AtPath(object, r.custom->spec_replicas).As<std::int64_t>(0);
        b["status"]["replicas"] = AtPath(object, r.custom->status_replicas).As<std::int64_t>(0);
        auto selector = AtPath(object, r.custom->label_selector);
        if (selector.IsString()) b["status"]["selector"] = selector;
        return b.ExtractValue();
    }
    b["spec"]["replicas"] = object["spec"]["replicas"].As<std::int64_t>(0);
    b["status"]["replicas"] = object["status"]["replicas"].As<std::int64_t>(0);
    auto selector = tables::SelectorString(object["spec"]["selector"]);
    if (!selector.empty()) b["status"]["selector"] = selector;
    return b.ExtractValue();
}

Value ResourceList(const Registry& reg, const std::string& gv) {
    ValueBuilder b(json::Type::kObject);
    b["kind"] = "APIResourceList";
    b["apiVersion"] = "v1";
    b["groupVersion"] = gv;
    ValueBuilder list(json::Type::kArray);
    for (const auto* rp : reg.All()) {
        const auto& r = *rp;
        if (r.GroupVersion() != gv) continue;
        ValueBuilder e(json::Type::kObject);
        e["name"] = r.plural;
        e["singularName"] = r.singular;
        e["namespaced"] = r.namespaced;
        e["kind"] = r.kind;
        e["verbs"] = Verbs(r);
        if (!r.short_names.empty()) e["shortNames"] = r.short_names;
        if (!r.categories.empty()) e["categories"] = r.categories;
        list.PushBack(e.ExtractValue());
        std::vector<std::string> subs;
        if (r.has_status) subs.push_back("status");
        if (r.group.empty() && r.plural == "namespaces") subs.push_back("finalize");
        if (r.plural == "certificatesigningrequests") subs.push_back("approval");
        if (HasScale(r)) {
            ValueBuilder se(json::Type::kObject);
            se["name"] = r.plural + "/scale";
            se["singularName"] = "";
            se["namespaced"] = true;
            se["group"] = "autoscaling";
            se["version"] = "v1";
            se["kind"] = "Scale";
            se["verbs"] = std::vector<std::string>{"get", "patch", "update"};
            list.PushBack(se.ExtractValue());
        }
        if (IsPod(r)) {
            for (const auto& [sub, kind, verbs] : std::vector<std::tuple<std::string, std::string, std::vector<std::string>>>{
                     {"log", "Pod", {"get"}}, {"exec", "PodExecOptions", {"create", "get"}},
                     {"ephemeralcontainers", "Pod", {"get", "patch", "update"}}, {"resize", "Pod", {"get", "patch", "update"}},
                     {"attach", "PodAttachOptions", {"create", "get"}}, {"portforward", "PodPortForwardOptions", {"create", "get"}}}) {
                ValueBuilder se(json::Type::kObject);
                se["name"] = "pods/" + sub;
                se["singularName"] = "";
                se["namespaced"] = true;
                se["kind"] = kind;
                se["verbs"] = verbs;
                list.PushBack(se.ExtractValue());
            }
            for (const auto& [sub, kind, sgroup] : std::vector<std::tuple<std::string, std::string, std::string>>{
                     {"binding", "Binding", ""}, {"eviction", "Eviction", "policy"}}) {
                ValueBuilder se(json::Type::kObject);
                se["name"] = "pods/" + sub;
                se["singularName"] = "";
                se["namespaced"] = true;
                if (!sgroup.empty()) {
                    se["group"] = sgroup;
                    se["version"] = "v1";
                }
                se["kind"] = kind;
                se["verbs"] = std::vector<std::string>{"create"};
                list.PushBack(se.ExtractValue());
            }
        }
        if (IsServiceAccount(r)) {
            ValueBuilder se(json::Type::kObject);
            se["name"] = "serviceaccounts/token";
            se["singularName"] = "";
            se["namespaced"] = true;
            se["group"] = "authentication.k8s.io";
            se["version"] = "v1";
            se["kind"] = "TokenRequest";
            se["verbs"] = std::vector<std::string>{"create"};
            list.PushBack(se.ExtractValue());
        }
        for (const auto& s : subs) {
            ValueBuilder se(json::Type::kObject);
            se["name"] = r.plural + "/" + s;
            se["singularName"] = "";
            se["namespaced"] = r.namespaced;
            se["kind"] = r.kind;
            se["verbs"] = std::vector<std::string>{"get", "patch", "update"};
            list.PushBack(se.ExtractValue());
        }
    }
    b["resources"] = list;
    return b.ExtractValue();
}

// Aggregated discovery (apidiscovery.k8s.io/v2): every group with its
// versions and resources in one document, built from the per-version lists.
Value AggregatedDiscovery(const Registry& reg, bool core) {
    std::vector<std::string> groups;
    if (core) {
        groups.push_back("");
    } else {
        for (const auto& gv : reg.GroupVersions()) {
            auto g = gv.substr(0, gv.find('/'));
            if (std::find(groups.begin(), groups.end(), g) == groups.end()) groups.push_back(g);
        }
    }
    ValueBuilder items(json::Type::kArray);
    for (const auto& g : groups) {
        ValueBuilder entry(json::Type::kObject);
        entry["metadata"]["name"] = g;
        ValueBuilder versions(json::Type::kArray);
        std::vector<std::string> vs;
        if (core) {
            vs.push_back("v1");
        } else {
            auto preferred = reg.PreferredVersion(g);
            vs.push_back(preferred);
            for (const auto& gv : reg.GroupVersions()) {
                if (gv.starts_with(g + "/") && gv.substr(g.size() + 1) != preferred) vs.push_back(gv.substr(g.size() + 1));
            }
        }
        for (const auto& v : vs) {
            const auto list = ResourceList(reg, g.empty() ? v : g + "/" + v);
            std::map<std::string, ValueBuilder> resources;
            std::vector<std::string> order;
            for (const auto& item : list["resources"]) {
                auto name = item["name"].As<std::string>("");
                auto slash = name.find('/');
                ValueBuilder kind(json::Type::kObject);
                kind["group"] = item.HasMember("group") ? item["group"] : ValueBuilder(g).ExtractValue();
                kind["version"] = item.HasMember("version") ? item["version"] : ValueBuilder(v).ExtractValue();
                kind["kind"] = item["kind"];
                if (slash == std::string::npos) {
                    ValueBuilder res(json::Type::kObject);
                    res["resource"] = name;
                    res["responseKind"] = kind;
                    res["scope"] = item["namespaced"].As<bool>(false) ? "Namespaced" : "Cluster";
                    res["singularResource"] = item["singularName"];
                    res["verbs"] = item["verbs"];
                    if (item.HasMember("shortNames")) res["shortNames"] = item["shortNames"];
                    if (item.HasMember("categories")) res["categories"] = item["categories"];
                    resources.emplace(name, std::move(res));
                    order.push_back(name);
                } else {
                    auto parent = name.substr(0, slash);
                    auto it = resources.find(parent);
                    if (it == resources.end()) continue;
                    ValueBuilder sub(json::Type::kObject);
                    sub["subresource"] = name.substr(slash + 1);
                    sub["responseKind"] = kind;
                    sub["verbs"] = item["verbs"];
                    if (!it->second.HasMember("subresources")) it->second["subresources"] = ValueBuilder(json::Type::kArray);
                    it->second["subresources"].PushBack(sub.ExtractValue());
                }
            }
            ValueBuilder version(json::Type::kObject);
            version["version"] = v;
            ValueBuilder rs(json::Type::kArray);
            for (const auto& n : order) rs.PushBack(resources.at(n).ExtractValue());
            version["resources"] = rs;
            version["freshness"] = "Current";
            versions.PushBack(version.ExtractValue());
        }
        entry["versions"] = versions;
        items.PushBack(entry.ExtractValue());
    }
    ValueBuilder doc(json::Type::kObject);
    doc["kind"] = "APIGroupDiscoveryList";
    doc["apiVersion"] = "apidiscovery.k8s.io/v2";
    doc["metadata"] = ValueBuilder(json::Type::kObject);
    doc["items"] = items;
    return doc.ExtractValue();
}

bool WantsAggregated(const http::HttpRequest& request) {
    return request.GetHeader("Accept").find("as=APIGroupDiscoveryList") != std::string::npos;
}

Value Group(const Registry& reg, const std::string& name) {
    ValueBuilder g(json::Type::kObject);
    g["name"] = name;
    auto preferred = reg.PreferredVersion(name);
    ValueBuilder versions(json::Type::kArray);
    // The preferred version first, as upstream lists them.
    std::vector<std::string> seen;
    auto add = [&](const std::string& version) {
        if (std::find(seen.begin(), seen.end(), version) != seen.end()) return;
        seen.push_back(version);
        ValueBuilder v(json::Type::kObject);
        v["groupVersion"] = name + "/" + version;
        v["version"] = version;
        versions.PushBack(v.ExtractValue());
    };
    add(preferred);
    for (const auto& gv : reg.GroupVersions()) {
        if (gv.starts_with(name + "/")) add(gv.substr(name.size() + 1));
    }
    g["versions"] = versions;
    ValueBuilder pv(json::Type::kObject);
    pv["groupVersion"] = name + "/" + preferred;
    pv["version"] = preferred;
    g["preferredVersion"] = pv;
    return g.ExtractValue();
}


std::string Arg(const http::HttpRequest& req, std::string_view name) { return req.GetArg(name); }

bool Flag(const http::HttpRequest& req, std::string_view name) {
    auto v = req.GetArg(name);
    return v == "true" || v == "1";
}

// A continue token: where the next page starts and the revision the
// list was read at, so every page reports the same one. -1 is upstream's
// "inconsistent": continue at the current revision.
struct Continue {
    std::int64_t rv = 0;
    std::string ns, name;
};

std::string EncodeContinue(std::int64_t rv, const std::string& ns, const std::string& name) {
    ValueBuilder b(json::Type::kObject);
    b["v"] = "meta.k8s.io/v1";
    b["rv"] = rv;
    b["start"] = ns + "/" + name;
    return userver::crypto::base64::Base64UrlEncode(json::ToString(b.ExtractValue()), userver::crypto::base64::Pad::kWithout);
}

Continue DecodeContinue(const std::string& token) {
    if (token.empty()) return {};
    try {
        auto v = json::FromString(userver::crypto::base64::Base64UrlDecode(token));
        auto start = v["start"].As<std::string>();
        auto slash = start.find('/');
        if (slash == std::string::npos) throw std::runtime_error("no start");
        return {v["rv"].As<std::int64_t>(), start.substr(0, slash), start.substr(slash + 1)};
    } catch (const std::exception&) {
        throw BadRequest("continue key is not valid: " + token);
    }
}

std::uint64_t ParseRv(const std::string& rv) {
    if (rv.empty()) return 0;
    try {
        return std::stoull(rv);
    } catch (const std::exception&) {
        throw BadRequest("invalid resource version: " + rv);
    }
}

Value Event(const std::string& type, const Value& object) {
    ValueBuilder b(json::Type::kObject);
    b["type"] = type;
    b["object"] = object;
    return b.ExtractValue();
}

Value Bookmark(const Resource& r, std::uint64_t rv, bool initial_end) {
    ValueBuilder o(json::Type::kObject);
    o["kind"] = r.kind;
    o["apiVersion"] = r.GroupVersion();
    o["metadata"]["resourceVersion"] = std::to_string(rv);
    if (initial_end) o["metadata"]["annotations"]["k8s.io/initial-events-end"] = "true";
    return Event("BOOKMARK", o.ExtractValue());
}

// A body in any of the encodings clients send: protobuf, JSON (repeated
// keys allowed, as Go allows them), YAML.
bodies::Parsed ReadBody(const std::string& body, const std::string& content_type) {
    if (body.empty()) return {};
    if (proto::Decoder::IsProtobuf(body)) {
        const auto* decoder = proto::Decoder::Instance();
        if (!decoder) throw ApiError(415, "UnsupportedMediaType", "this server is not set up for protobuf");
        return {decoder->Decode(body), {}, ""};
    }
    try {
        return bodies::IsYaml(content_type) ? bodies::Yaml(body) : bodies::Json(body);
    } catch (const std::exception& e) {
        throw BadRequest(std::string("the request body is not valid: ") + e.what());
    }
}

Value ParseBody(const std::string& body) { return ReadBody(body, "").value; }

std::int64_t GracePeriod(const http::HttpRequest& req, const Value& options, const Value& object) {
    if (req.HasArg("gracePeriodSeconds")) return std::stoll(Arg(req, "gracePeriodSeconds"));
    if (options.IsObject() && options.HasMember("gracePeriodSeconds") && !options["gracePeriodSeconds"].IsNull()) {
        return options["gracePeriodSeconds"].As<std::int64_t>();
    }
    return object["spec"]["terminationGracePeriodSeconds"].As<std::int64_t>(30);
}

std::string Propagation(const http::HttpRequest& req, const Value& options) {
    if (req.HasArg("propagationPolicy")) return Arg(req, "propagationPolicy");
    if (options.IsObject()) return options["propagationPolicy"].As<std::string>("");
    return "";
}


// Whether an object asked to go away may go now.
bool Releasable(const Resource& r, const Value& object) {
    if (!objects::Deleting(object) || objects::HasFinalizers(object)) return false;
    if (IsNamespace(r)) {
        const auto f = object["spec"]["finalizers"];
        if (f.IsArray() && !f.IsEmpty()) return false;
    }
    if (IsPod(r)) {
        // A pod on a node goes when its kubelet confirms with a zero grace period.
        return object["spec"]["nodeName"].As<std::string>("").empty() ||
               object["metadata"]["deletionGracePeriodSeconds"].As<std::int64_t>(0) == 0;
    }
    return true;
}

// Checks a write's preconditions against the stored object.
void CheckVersion(const Resource& r, const Value& stored, const Value& incoming) {
    auto want = incoming["metadata"]["resourceVersion"].As<std::string>("");
    if (!want.empty() && want != stored["metadata"]["resourceVersion"].As<std::string>("")) {
        throw Conflict(
            r.plural, r.group, objects::Name(stored),
            "the object has been modified; please apply your changes to the latest version and try again"
        );
    }
    auto uid = incoming["metadata"]["uid"].As<std::string>("");
    if (!uid.empty() && uid != stored["metadata"]["uid"].As<std::string>("")) {
        throw Conflict(r.plural, r.group, objects::Name(stored), "Precondition failed: UID in precondition: " + uid);
    }
}

}  // namespace

Api::Api(const userver::components::ComponentConfig& config, const userver::components::ComponentContext& context)
    : HttpHandlerBase(config, context),
      store_(context.FindComponent<Store>()),
      watch_(context.FindComponent<Watch>()),
      auth_(context.FindComponent<Auth>()),
      services_(context.FindComponent<Services>()),
      crds_(context.FindComponent<Crds>()),
      webhooks_(context.FindComponent<Webhooks>()),
      policies_(context.FindComponent<Policies>()),
      http_(context.FindComponent<userver::components::HttpClient>().GetHttpClient()),
      registry_(Registry::Builtin()),
      openapi_dir_(config["openapi-dir"].As<std::string>("")) {}

userver::yaml_config::Schema Api::GetStaticConfigSchema() {
    return userver::yaml_config::MergeSchemas<HttpHandlerBase>(R"(
type: object
description: the Kubernetes API
additionalProperties: false
properties:
    openapi-dir:
        type: string
        description: directory with the upstream OpenAPI documents, served as they are
)");
}

bool Api::IsStreamed(const http::HttpRequest& request, userver::server::request::RequestContext&) const {
    if (request.GetMethod() != http::HttpMethod::kGet) return false;
    if (Flag(request, "follow") && request.GetRequestPath().ends_with("/log")) return true;
    return Flag(request, "watch");
}

std::string Api::HandleRequestThrow(const http::HttpRequest& request, userver::server::request::RequestContext&) const {
    auto& response = request.GetHttpResponse();
    response.SetContentType("application/json");
    try {
        return Handle(request);
    } catch (const ApiError& e) {
        response.SetStatus(static_cast<http::HttpStatus>(e.Code()));
        return json::ToString(e.ToStatus());
    } catch (const std::exception& e) {
        LOG_ERROR() << request.GetMethodStr() << " " << request.GetUrl() << ": " << e.what();
        response.SetStatus(http::HttpStatus::kInternalServerError);
        return json::ToString(ApiError(500, "InternalError", e.what()).ToStatus());
    }
}

std::string Api::Handle(const http::HttpRequest& request) const {
    auto out = HandleInner(request);
    const auto method = request.GetMethod();
    if (method != http::HttpMethod::kGet && method != http::HttpMethod::kHead &&
        request.GetRequestPath().find("/apis/admissionregistration.k8s.io/") == 0 &&
        request.GetRequestPath().find("webhookconfigurations") != std::string::npos) {
        webhooks_.Refresh();
    }
    if (method != http::HttpMethod::kGet && method != http::HttpMethod::kHead &&
        request.GetRequestPath().find("/apis/admissionregistration.k8s.io/") == 0 &&
        request.GetRequestPath().find("admissionpolic") != std::string::npos) {
        policies_.Refresh();
    }
    return out;
}

std::string Api::HandleInner(const http::HttpRequest& request) const {
    const auto parts = Split(request.GetRequestPath());
    const auto method = request.GetMethod();
    auto& response = request.GetHttpResponse();

    if (parts.size() == 1 && (parts[0] == "healthz" || parts[0] == "livez" || parts[0] == "readyz")) {
        response.SetContentType("text/plain");
        return "ok";
    }
    if (parts.size() == 1 && parts[0] == "version") {
        ValueBuilder v(json::Type::kObject);
        v["major"] = "1";
        v["minor"] = "37";
        v["gitVersion"] = std::string(kVersion);
        v["platform"] = "linux/amd64";
        return json::ToString(v.ExtractValue());
    }
    const auto user = auth_.Authenticate(request.GetHeader(userver::http::headers::kAuthorization));
    if (!user) throw Unauthorized();

    if (parts.size() == 2 && parts[0] == ".well-known" && parts[1] == "openid-configuration") {
        return auth_.OpenIdConfiguration();
    }
    if (parts.size() == 3 && parts[0] == "openid" && parts[1] == "v1" && parts[2] == "jwks") {
        response.SetContentType("application/jwk-set+json");
        return auth_.Jwks();
    }

    if (!parts.empty() && parts[0] == "openapi") {
        // One copy of the documents for every handler instance.
        static const openapi::Documents docs(openapi_dir_);
        std::optional<std::string> body;
        if (parts.size() == 2 && parts[1] == "v2") body = docs.V2(registry_);
        if (parts.size() == 2 && parts[1] == "v3") body = docs.V3Index(registry_);
        if (parts.size() > 2 && parts[1] == "v3") {
            std::string path;
            for (std::size_t i = 2; i < parts.size(); ++i) path += (i > 2 ? "/" : "") + parts[i];
            body = docs.V3(registry_, path);
        }
        if (!body || body->empty()) throw ApiError(404, "NotFound", "the server could not find the requested resource");
        return *body;
    }

    if (parts.empty() || (parts[0] != "api" && parts[0] != "apis")) {
        throw ApiError(404, "NotFound", "the server could not find the requested resource");
    }

    // Discovery.
    if (parts.size() == 1 && WantsAggregated(request)) {
        // The version the client named first; v2beta1 is the same document.
        const auto accept = request.GetHeader("Accept");
        const auto version = accept.find("v=v2beta1") != std::string::npos && accept.find("v=v2;") == std::string::npos ? "v2beta1" : "v2";
        // Clients tell the document by its content type, parameters and all.
        response.SetHeader(std::string("Content-Type"), std::string("application/json;g=apidiscovery.k8s.io;v=") + version + ";as=APIGroupDiscoveryList");
        ValueBuilder doc(AggregatedDiscovery(registry_, parts[0] == "api"));
        doc["apiVersion"] = std::string("apidiscovery.k8s.io/") + version;
        return json::ToString(doc.ExtractValue());
    }
    if (parts[0] == "api" && parts.size() == 1) {
        ValueBuilder v(json::Type::kObject);
        v["kind"] = "APIVersions";
        v["versions"] = std::vector<std::string>{"v1"};
        ValueBuilder addr(json::Type::kObject);
        addr["clientCIDR"] = "0.0.0.0/0";
        addr["serverAddress"] = request.GetHost();
        v["serverAddressByClientCIDRs"] = std::vector<Value>{addr.ExtractValue()};
        return json::ToString(v.ExtractValue());
    }
    if (parts[0] == "apis" && parts.size() == 1) {
        ValueBuilder v(json::Type::kObject);
        v["kind"] = "APIGroupList";
        v["apiVersion"] = "v1";
        ValueBuilder groups(json::Type::kArray);
        std::vector<std::string> names;
        for (const auto& gv : registry_.GroupVersions()) {
            auto name = gv.substr(0, gv.find('/'));
            if (std::find(names.begin(), names.end(), name) != names.end()) continue;
            names.push_back(name);
            groups.PushBack(Group(registry_, name));
        }
        v["groups"] = groups;
        return json::ToString(v.ExtractValue());
    }
    if (parts[0] == "apis" && parts.size() == 2) {
        for (const auto& gv : registry_.GroupVersions()) {
            if (gv.starts_with(parts[1] + "/")) {
                ValueBuilder g(Group(registry_, parts[1]));
                g["kind"] = "APIGroup";
                g["apiVersion"] = "v1";
                return json::ToString(g.ExtractValue());
            }
        }
        throw ApiError(404, "NotFound", "the server could not find the requested resource");
    }

    std::string group = parts[0] == "api" ? "" : parts[1];
    std::string version = parts[0] == "api" ? parts[1] : parts[2];
    std::size_t base = parts[0] == "api" ? 2 : 3;
    std::vector<std::string> rest(parts.begin() + base, parts.end());
    std::string gv = group.empty() ? version : group + "/" + version;

    if (rest.empty()) {
        bool known = false;
        for (const auto* r : registry_.All()) known = known || r->GroupVersion() == gv;
        if (!known) throw ApiError(404, "NotFound", "the server could not find the requested resource");
        return json::ToString(ResourceList(registry_, gv));
    }

    Target t;
    if (rest.size() >= 3 && rest[0] == "namespaces" && registry_.Find(group, version, rest[2]) &&
        registry_.Find(group, version, rest[2])->namespaced) {
        t.ns = rest[1];
        rest.erase(rest.begin(), rest.begin() + 2);
    }
    t.resource = registry_.Find(group, version, rest[0]);
    if (t.resource && rest.size() >= 3 && rest[2] == "proxy" &&
        (IsPod(*t.resource) || IsService(*t.resource) || (t.resource->group.empty() && t.resource->plural == "nodes"))) {
        std::string path;
        for (std::size_t i = 3; i < rest.size(); ++i) path += "/" + rest[i];
        // .../proxy without a slash: GET and HEAD are sent to .../proxy/, as
        // upstream redirects them, so relative links resolve inside the proxy.
        if (rest.size() == 3 && !request.GetRequestPath().ends_with("/") &&
            (method == http::HttpMethod::kGet || method == http::HttpMethod::kHead)) {
            std::string query;
            for (const auto& arg : request.ArgNames()) {
                for (const auto& v : request.GetArgVector(arg)) {
                    query += (query.empty() ? "?" : "&") + userver::http::UrlEncode(arg) + "=" + userver::http::UrlEncode(v);
                }
            }
            response.SetStatus(http::HttpStatus::kMovedPermanently);
            response.SetHeader(std::string("Location"), request.GetRequestPath() + "/" + query);
            return "";
        }
        if (path.empty() || request.GetRequestPath().ends_with("/")) path += "/";
        return Proxy(request, *t.resource, t.ns, rest[1], path);
    }
    if (!t.resource || rest.size() > 3) {
        throw ApiError(404, "NotFound", "the server could not find the requested resource");
    }
    const Resource& r = *t.resource;
    if (rest.size() >= 2) t.name = rest[1];
    if (rest.size() == 3) t.sub = rest[2];
    if (!t.sub.empty() && !(t.sub == "status" && r.has_status) && !(t.sub == "finalize" && IsNamespace(r)) &&
        !(t.sub == "token" && IsServiceAccount(r)) && !(t.sub == "scale" && HasScale(r)) && !((t.sub == "binding" || t.sub == "eviction" || KubeletSub(t.sub) || t.sub == "ephemeralcontainers" || t.sub == "resize") && IsPod(r)) &&
        !(t.sub == "approval" && r.plural == "certificatesigningrequests")) {
        throw ApiError(404, "NotFound", "the server could not find the requested resource");
    }
    if (!r.namespaced && !t.ns.empty()) throw ApiError(404, "NotFound", "the server could not find the requested resource");

    const bool dry_run = Arg(request, "dryRun") == "All";
    const bool table = tables::Wanted(request.GetHeader("Accept"));
    const bool metadata = !table && tables::MetadataWanted(request.GetHeader("Accept"));
    const auto include_object = Arg(request, "includeObject");

    if (r.CreateOnly()) {
        // Reviews have no table form; a client that will take nothing else
        // gets 406, as upstream answers.
        if (table && request.GetHeader("Accept").find("application/json,") == std::string::npos &&
            !request.GetHeader("Accept").ends_with("application/json")) {
            throw ApiError(406, "NotAcceptable", "the server was unable to respond with a content type that the client supports");
        }
        if (method != http::HttpMethod::kPost || !t.name.empty()) {
            throw MethodNotAllowed("the server does not allow this method on the requested resource");
        }
        response.SetStatus(http::HttpStatus::kCreated);
        return Review(r, request, *user);
    }
    if (IsPod(r) && KubeletSub(t.sub)) {
        if (t.sub != "log" && webhooks_.Interested(r, t.sub, "CONNECT")) {
            // What the client asked for, as the options kind of the subresource.
            ValueBuilder opts(json::Type::kObject);
            opts["kind"] = t.sub == "exec" ? "PodExecOptions" : t.sub == "attach" ? "PodAttachOptions" : "PodPortForwardOptions";
            opts["apiVersion"] = "v1";
            for (const char* flag : {"stdin", "stdout", "stderr", "tty"}) {
                if (request.HasArg(flag)) opts[flag] = Flag(request, flag);
            }
            if (request.HasArg("container")) opts["container"] = Arg(request, "container");
            if (request.HasArg("command")) opts["command"] = request.GetArgVector("command");
            webhooks_.Validate({&r, t.sub, "CONNECT", t.ns, t.name, opts.ExtractValue(), Value(), Value(), &*user, dry_run});
        }
        return Kubelet(request, t.ns, t.name, t.sub);
    }
    if (t.sub == "binding" || t.sub == "eviction") {
        if (method != http::HttpMethod::kPost) throw MethodNotAllowed("the server does not allow this method on the requested resource");
        auto body = ParseBody(request.RequestBody());
        if (t.sub == "binding") {
            Bind(t.ns, t.name, body["target"]["name"].As<std::string>(""));
        } else {
            Evict(t.ns, t.name, body["deleteOptions"]);
        }
        response.SetStatus(http::HttpStatus::kCreated);
        return json::ToString(SuccessStatus());
    }
    if (t.sub == "token") {
        if (method != http::HttpMethod::kPost) throw MethodNotAllowed("the server does not allow this method on the requested resource");
        response.SetStatus(http::HttpStatus::kCreated);
        return TokenRequest(t.ns, t.name, request);
    }

    // Collections.
    if (t.name.empty()) {
        if (method == http::HttpMethod::kGet) {
            auto labels = LabelSelector::Parse(Arg(request, "labelSelector"));
            auto fields = FieldSelector::Parse(Arg(request, "fieldSelector"));
            std::size_t limit = request.HasArg("limit") ? std::stoull(Arg(request, "limit")) : 0;
            const auto cont = DecodeContinue(Arg(request, "continue"));
            const auto& after_ns = cont.ns;
            const auto& after_name = cont.name;
            if (cont.rv > 0 && static_cast<std::uint64_t>(cont.rv) <= store_.Compacted(r)) {
                ApiError gone(410, "Expired",
                              "The provided continue parameter is too old to display a consistent list result. You can start a new list without the continue parameter, or use the continue token in this response to retrieve the remainder of the results. Continuing with the provided token results in an inconsistent list - objects that were created, modified, or deleted between the time the first chunk was returned and now may show up in the list.");
                ValueBuilder status(gone.ToStatus());
                status["metadata"]["continue"] = EncodeContinue(-1, cont.ns, cont.name);
                response.SetStatus(http::HttpStatus::kGone);
                return json::ToString(status.ExtractValue());
            }
            auto ns = t.ns.empty() && r.namespaced ? fields.Pinned("metadata.namespace") : t.ns;
            std::int64_t remaining = -1;
            auto render = [&](const std::vector<Value>& items, const std::string& rv, const std::string& cont) {
                if (table) {
                    std::vector<Value> view;
                    for (const auto& item : items) view.push_back(ToView(r, item));
                    return json::ToString(tables::Make(r, view, true, include_object, rv, cont));
                }
                ValueBuilder list(json::Type::kObject);
                list["kind"] = metadata ? "PartialObjectMetadataList" : r.kind + "List";
                list["apiVersion"] = metadata ? "meta.k8s.io/v1" : r.GroupVersion();
                list["metadata"]["resourceVersion"] = rv;
                if (!cont.empty()) list["metadata"]["continue"] = cont;
                if (remaining >= 0) list["metadata"]["remainingItemCount"] = remaining;
                ValueBuilder out(json::Type::kArray);
                for (const auto& item : items) out.PushBack(metadata ? tables::Metadata(item) : ToView(r, item));
                list["items"] = out;
                return json::ToString(list.ExtractValue());
            };
            std::vector<Value> found;
            if (!r.namespaced && !fields.Pinned("metadata.name").empty() && limit == 0) {
                // A list of one by name, as the kubelet asks for its node.
                auto one = store_.Get(r, "", fields.Pinned("metadata.name"));
                if (one && labels.Matches((*one)["metadata"]["labels"])) found.push_back(*one);
                return render(found, std::to_string(store_.Revision(r)), "");
            }
            auto page = store_.List(r, ns, limit, after_ns, after_name);
            for (auto& item : page.items) {
                if (labels.Matches(item["metadata"]["labels"]) && fields.Matches(item)) found.push_back(std::move(item));
            }
            // Pages after the first report the revision of the first.
            const auto rv = cont.rv > 0 ? static_cast<std::uint64_t>(cont.rv) : page.rv;
            remaining = page.last_name.empty() ? -1 : static_cast<std::int64_t>(store_.Count(r, ns, page.last_ns, page.last_name));
            return render(found, std::to_string(rv), page.last_name.empty() ? "" : EncodeContinue(static_cast<std::int64_t>(rv), page.last_ns, page.last_name));
        }
        if (method == http::HttpMethod::kPost) {
            auto parsed = ReadBody(request.RequestBody(), request.GetHeader(userver::http::headers::kContentType));
            auto incoming = CheckFields(r, parsed.value, parsed, request);
            if (r.namespaced && t.ns.empty()) throw ApiError(404, "NotFound", "the server could not find the requested resource");
            auto object = objects::PrepareCreate(r, incoming, t.ns);
            if (IsDefinitions(r)) Crds::Validate(object);
            if (IsWebhookConfig(r)) Webhooks::ValidateConfig(r, object);
            validation::Validate(r, object);
            auto name = objects::Name(object);
            if (r.namespaced) {
                auto ns = store_.Get(kNamespaces, "", t.ns);
                if (!ns) throw NotFound("namespaces", "", t.ns);
                if (objects::Deleting(*ns)) {
                    const auto why = "unable to create new content in namespace " + t.ns + " because it is being terminated";
                    throw ApiError(403, "Forbidden", why).About(r.plural, r.group, name).Cause("NamespaceTerminating", why, "metadata.namespace");
                }
            }
            object = admission::Admit(store_, r, object);
            {
                Webhooks::Request wr{&r, "", "CREATE", t.ns, name, object, Value(), Value(), &*user, dry_run};
                // Policies, then webhooks; a mutation is defaulted again, as
                // the decoder upstream does.
                wr.object = policies_.Mutate(wr);
                object = objects::Defaults(r, webhooks_.Mutate(wr));
                wr.object = object;
                Warn(request, policies_.Validate(wr));
                webhooks_.Validate(wr);
            }
            object = ToStorage(r, object);
            if (dry_run) {
                if (store_.Get(r, t.ns, name)) throw AlreadyExists(r.plural, r.group, name);
                response.SetStatus(http::HttpStatus::kCreated);
                return json::ToString(ToView(r, object));
            }
            Services::Change change;
            Decision d;
            try {
                if (IsService(r)) object = services_.Assign(object, std::nullopt, change);
                d = store_.Write(r, t.ns, name, [&](const std::optional<Value>& current) {
                    if (current) throw AlreadyExists(r.plural, r.group, name);
                    return Decision::Put(object);
                });
            } catch (...) {
                services_.Undo(change, t.ns, name);
                throw;
            }
            response.SetStatus(http::HttpStatus::kCreated);
            return json::ToString(ToView(r, d.object));
        }
        if (method == http::HttpMethod::kDelete) {
            auto labels = LabelSelector::Parse(Arg(request, "labelSelector"));
            auto fields = FieldSelector::Parse(Arg(request, "fieldSelector"));
            auto page = store_.List(r, t.ns, 0, "", "");
            ValueBuilder list(json::Type::kObject);
            list["kind"] = r.kind + "List";
            list["apiVersion"] = r.GroupVersion();
            list["items"] = ValueBuilder(json::Type::kArray);
            for (const auto& item : page.items) {
                if (!labels.Matches(item["metadata"]["labels"]) || !fields.Matches(item)) continue;
                auto d = store_.Write(r, objects::Namespace(item), objects::Name(item), [&](const std::optional<Value>& cur) {
                    if (!cur) return Decision::Keep();
                    ValueBuilder b(*cur);
                    if (!objects::Deleting(*cur)) {
                        b["metadata"]["deletionTimestamp"] = objects::Now();
                        b["metadata"]["deletionGracePeriodSeconds"] = IsPod(r) ? GracePeriod(request, Value(), *cur) : 0;
                    }
                    auto next = b.ExtractValue();
                    return Releasable(r, next) ? Decision::Remove(next) : Decision::Put(next);
                });
                if (IsService(r) && d.action == Decision::Action::kRemove) services_.Release(d.object);
                list["items"].PushBack(d.object);
            }
            return json::ToString(list.ExtractValue());
        }
        throw MethodNotAllowed("the server does not allow this method on the requested resource");
    }

    if (t.sub == "scale") {
        if (method == http::HttpMethod::kGet) {
            auto obj = store_.Get(r, t.ns, t.name);
            if (!obj) throw NotFound(r.plural, r.group, t.name);
            return json::ToString(ScaleOf(r, *obj));
        }
        if (method != http::HttpMethod::kPut && method != http::HttpMethod::kPatch) {
            throw MethodNotAllowed("the server does not allow this method on the requested resource");
        }
        const auto body = request.RequestBody();
        const auto content_type = request.GetHeader(userver::http::headers::kContentType);
        auto decide = [&](const std::optional<Value>& current) -> Decision {
            if (!current) throw NotFound(r.plural, r.group, t.name);
            auto scale = method == http::HttpMethod::kPatch ? patch::Apply(content_type, ScaleOf(r, *current), body) : ParseBody(body);
            auto rv = scale["metadata"]["resourceVersion"].As<std::string>("");
            if (method == http::HttpMethod::kPut && !rv.empty()) CheckVersion(r, *current, scale);
            auto replicas = scale["spec"]["replicas"].As<std::int64_t>(0);
            if (replicas < 0) throw Invalid("Scale", "autoscaling", t.name, "spec.replicas: Invalid value: " + std::to_string(replicas) + ": must be greater than or equal to 0");
            const auto path = r.custom ? r.custom->spec_replicas : std::string(".spec.replicas");
            if (AtPath(*current, path).As<std::int64_t>(-1) == replicas) return Decision::Keep();
            ValueBuilder b(SetAtPath(*current, path, ValueBuilder(replicas).ExtractValue()));
            b["metadata"]["generation"] = (*current)["metadata"]["generation"].As<std::int64_t>(0) + 1;
            return Decision::Put(b.ExtractValue());
        };
        if (dry_run) return json::ToString(ScaleOf(r, decide(store_.Get(r, t.ns, t.name)).object));
        auto d = store_.Write(r, t.ns, t.name, decide);
        return json::ToString(ScaleOf(r, d.object));
    }

    // Single objects.
    if (method == http::HttpMethod::kGet) {
        auto obj = store_.Get(r, t.ns, t.name);
        if (!obj) throw NotFound(r.plural, r.group, t.name);
        obj = ToView(r, *obj);
        if (table && t.sub.empty()) return json::ToString(tables::Make(r, {*obj}, true, include_object, "", ""));
        if (t.sub == "scale") return json::ToString(ScaleOf(r, *obj));
        if (metadata && t.sub.empty()) return json::ToString(tables::Metadata(*obj));
        return json::ToString(*obj);
    }

    if (method == http::HttpMethod::kPut || method == http::HttpMethod::kPatch) {
        const bool status_write = t.sub == "status" || t.sub == "approval";
        // Subresources that write one part of a pod's spec.
        const char* spec_part = t.sub == "ephemeralcontainers" ? "ephemeralContainers" : t.sub == "resize" ? "containers" : nullptr;
        const bool finalize = t.sub == "finalize";
        const bool is_patch = method == http::HttpMethod::kPatch;
        const auto body = request.RequestBody();
        const auto content_type = request.GetHeader(userver::http::headers::kContentType);
        Services::Change change;
        // Server-side apply: the configuration lands on what is there, or
        // creates the object; the patch's own keys are checked like a body's.
        const bool apply = is_patch && content_type.starts_with("application/apply-patch");
        if (apply && Arg(request, "fieldManager").empty()) throw BadRequest("PatchOptions.meta.k8s.io \"\" is invalid: fieldManager: Required value: is required for apply patch");
        const auto parsed = is_patch && !apply ? ReadBody(body, "application/json") : ReadBody(body, apply ? "application/yaml" : content_type);
        auto decide = [&](const std::optional<Value>& stored) -> Decision {
            // A custom resource is patched and checked in the version the
            // client speaks, and stored in the storage version.
            std::optional<Value> current;
            if (stored) current = ToView(r, *stored);
            Value incoming;
            if (apply) {
                incoming = current ? patch::Strategic(*current, parsed.value) : parsed.value;
                if (current) {
                    ValueBuilder b(incoming);
                    b["metadata"]["resourceVersion"] = (*current)["metadata"]["resourceVersion"];
                    incoming = b.ExtractValue();
                }
            } else if (is_patch) {
                if (!current) throw NotFound(r.plural, r.group, t.name);
                incoming = patch::Apply(content_type, *current, body);
                // A patch writes on top of what it read: no version check unless it set one.
                // Whether the patch itself names a version: a merge patch says
                // so by its keys, a JSON patch by its paths.
                bool asked_rv = false;
                try {
                    auto p = json::FromString(body);
                    if (p.IsArray()) {
                        for (const auto& op : p) asked_rv = asked_rv || op["path"].As<std::string>("") == "/metadata/resourceVersion";
                    } else {
                        asked_rv = p["metadata"].HasMember("resourceVersion");
                    }
                } catch (const std::exception&) {
                }
                if (!asked_rv) {
                    ValueBuilder b(incoming);
                    b["metadata"]["resourceVersion"] = (*current)["metadata"]["resourceVersion"];
                    incoming = b.ExtractValue();
                }
            } else {
                incoming = parsed.value;
            }
            if (spec_part == nullptr && !finalize) incoming = CheckFields(r, incoming, parsed, request);
            if (!current) {
                if ((is_patch && !apply) || status_write || finalize) throw NotFound(r.plural, r.group, t.name);
                // An update of a missing object creates it, as upstream does for most kinds.
                auto object = objects::PrepareCreate(r, incoming, t.ns);
                if (objects::Name(object) != t.name) throw BadRequest("the name of the object does not match the name on the URL");
                if (IsDefinitions(r)) Crds::Validate(object);
                if (IsService(r) && !dry_run) object = services_.Assign(object, std::nullopt, change);
                return Decision::Put(ToStorage(r, object));
            }
            if (objects::Name(incoming) != t.name && !objects::Name(incoming).empty()) {
                throw BadRequest("the name of the object does not match the name on the URL");
            }
            CheckVersion(r, *current, incoming);
            Value next;
            if (spec_part != nullptr) {
                ValueBuilder b(*current);
                if (t.sub == "ephemeralcontainers") {
                    b["spec"]["ephemeralContainers"] = incoming["spec"]["ephemeralContainers"];
                } else {
                    // A resize changes resources only, container by container.
                    ValueBuilder containers(json::Type::kArray);
                    for (const auto& c : objects::Items((*current)["spec"]["containers"])) {
                        ValueBuilder cb(c);
                        for (const auto& n : objects::Items(incoming["spec"]["containers"])) {
                            if (n["name"] == c["name"]) {
                                if (n.HasMember("resources")) cb["resources"] = n["resources"];
                                if (n.HasMember("resizePolicy")) cb["resizePolicy"] = n["resizePolicy"];
                            }
                        }
                        containers.PushBack(cb.ExtractValue());
                    }
                    b["spec"]["containers"] = containers;
                }
                next = objects::PrepareUpdate(r, *current, b.ExtractValue(), false);
            } else if (finalize) {
                ValueBuilder b(*current);
                b["spec"]["finalizers"] = incoming["spec"]["finalizers"].IsMissing()
                                              ? ValueBuilder(json::Type::kArray)
                                              : ValueBuilder(incoming["spec"]["finalizers"]);
                next = b.ExtractValue();
            } else {
                next = objects::PrepareUpdate(r, *current, incoming, status_write);
                if (IsDefinitions(r) && !status_write) Crds::Validate(next);
                if (IsWebhookConfig(r)) Webhooks::ValidateConfig(r, next);
                validation::Validate(r, next);
                validation::ValidateUpdate(r, *current, next);
                if (IsService(r) && !status_write && !dry_run) next = services_.Assign(next, current, change);
            }
            next = ToStorage(r, next);
            if (next == *stored) return Decision::Keep();
            return Releasable(r, next) ? Decision::Remove(next) : Decision::Put(next);
        };
        if (webhooks_.Interested(r, t.sub, "UPDATE") || webhooks_.Interested(r, t.sub, "CREATE") ||
            policies_.Interested(r, "UPDATE") || policies_.Interested(r, "CREATE")) {
            // Webhooks are called outside the store's transaction: read,
            // decide, ask them, then write only if nobody wrote in between.
            for (int attempt = 0; attempt < 8; ++attempt) {
                auto stored = store_.Get(r, t.ns, t.name);
                auto d = decide(stored);
                if (d.action == Decision::Action::kKeep) return json::ToString(ToView(r, stored ? *stored : d.object));
                Webhooks::Request wr{&r, t.sub, stored ? "UPDATE" : "CREATE", t.ns, t.name, ToView(r, d.object),
                                     stored ? ToView(r, *stored) : Value(), Value(), &*user, dry_run};
                wr.object = policies_.Mutate(wr);
                auto mutated = objects::Defaults(r, webhooks_.Mutate(wr));
                wr.object = mutated;
                Warn(request, policies_.Validate(wr));
                webhooks_.Validate(wr);
                if (dry_run) return json::ToString(mutated);
                auto final_object = ToStorage(r, mutated);
                const auto seen = stored ? (*stored)["metadata"]["resourceVersion"].As<std::string>("") : std::string();
                bool raced = false;
                Decision written;
                try {
                    written = store_.Write(r, t.ns, t.name, [&](const std::optional<Value>& now) -> Decision {
                        auto now_rv = now ? (*now)["metadata"]["resourceVersion"].As<std::string>("") : std::string();
                        if (now_rv != seen || now.has_value() != stored.has_value()) {
                            raced = true;
                            return Decision::Keep();
                        }
                        if (d.action == Decision::Action::kRemove) return Decision::Remove(final_object);
                        return Decision::Put(final_object);
                    });
                } catch (...) {
                    services_.Undo(change, t.ns, t.name);
                    throw;
                }
                if (raced) continue;
                services_.Commit(change, t.ns, t.name);
                if (IsService(r) && written.action == Decision::Action::kRemove) services_.Release(written.object);
                return json::ToString(ToView(r, written.object));
            }
            throw Conflict(r.plural, r.group, t.name, "the object kept changing while admission webhooks looked at it; try again");
        }
        if (dry_run) {
            auto d = decide(store_.Get(r, t.ns, t.name));
            return json::ToString(ToView(r, d.object));
        }
        Decision d;
        try {
            d = store_.Write(r, t.ns, t.name, decide);
        } catch (...) {
            services_.Undo(change, t.ns, t.name);
            throw;
        }
        services_.Commit(change, t.ns, t.name);
        if (IsService(r) && d.action == Decision::Action::kRemove) services_.Release(d.object);
        return json::ToString(ToView(r, d.object));
    }

    if (method == http::HttpMethod::kDelete) {
        auto options = ParseBody(request.RequestBody());
        if (webhooks_.Interested(r, t.sub, "DELETE") || policies_.Interested(r, "DELETE")) {
            auto stored = store_.Get(r, t.ns, t.name);
            if (!stored) throw NotFound(r.plural, r.group, t.name);
            Webhooks::Request wr{&r, t.sub, "DELETE", t.ns, t.name, Value(), ToView(r, *stored), options, &*user, dry_run};
            Warn(request, policies_.Validate(wr));
            webhooks_.Validate(wr);
        }
        auto decide = [&](const std::optional<Value>& current) -> Decision {
            if (!current) throw NotFound(r.plural, r.group, t.name);
            if (options.IsObject() && options.HasMember("preconditions")) {
                ValueBuilder want(json::Type::kObject);
                want["metadata"] = options["preconditions"];
                CheckVersion(r, *current, want.ExtractValue());
            }
            ValueBuilder b(*current);
            auto policy = Propagation(request, options);
            auto add_finalizer = [&](const std::string& f) {
                std::vector<std::string> fs;
                for (const auto& v : objects::Items((*current)["metadata"]["finalizers"])) fs.push_back(v.As<std::string>());
                if (std::find(fs.begin(), fs.end(), f) == fs.end()) fs.push_back(f);
                b["metadata"]["finalizers"] = fs;
            };
            if (policy == "Foreground") add_finalizer("foregroundDeletion");
            if (policy == "Orphan") add_finalizer("orphan");
            std::int64_t grace = 0;
            if (IsPod(r)) {
                grace = GracePeriod(request, options, *current);
                if ((*current)["spec"]["nodeName"].As<std::string>("").empty()) grace = 0;
                auto phase = (*current)["status"]["phase"].As<std::string>("");
                if (phase == "Succeeded" || phase == "Failed") grace = 0;
            }
            if (objects::Deleting(*current)) {
                // Already going: a pod may be hurried along, nothing else changes.
                auto had = (*current)["metadata"]["deletionGracePeriodSeconds"].As<std::int64_t>(0);
                if (IsPod(r) && grace < had) b["metadata"]["deletionGracePeriodSeconds"] = grace;
            } else {
                b["metadata"]["deletionTimestamp"] = objects::Now();
                b["metadata"]["deletionGracePeriodSeconds"] = grace;
                if (IsNamespace(r)) b["status"]["phase"] = "Terminating";
                // A pod's graceful deletion is a change its kubelet acts on.
                if (IsPod(r) && grace > 0) b["metadata"]["generation"] = (*current)["metadata"]["generation"].As<std::int64_t>(1) + 1;
            }
            auto next = b.ExtractValue();
            if (Releasable(r, next)) return Decision::Remove(next);
            if (next == *current) return Decision::Keep();
            return Decision::Put(next);
        };
        if (dry_run) {
            auto d = decide(store_.Get(r, t.ns, t.name));
            return json::ToString(d.object);
        }
        auto d = store_.Write(r, t.ns, t.name, decide);
        if (IsService(r) && d.action == Decision::Action::kRemove) services_.Release(d.object);
        if (d.action == Decision::Action::kRemove && !IsPod(r) && !r.has_status) {
            return json::ToString(SuccessStatus());
        }
        return json::ToString(ToView(r, d.object));
    }

    throw MethodNotAllowed("the server does not allow this method on the requested resource");
}

void Api::Bind(const std::string& ns, const std::string& name, const std::string& node) const {
    if (node.empty()) throw Invalid("Binding", "", name, "target.name: Required value");
    const auto& pods = *registry_.Find("", "v1", "pods");
    store_.Write(pods, ns, name, [&](const std::optional<Value>& current) {
        if (!current) throw NotFound("pods", "", name);
        if (objects::Deleting(*current)) {
            throw Conflict("pods", "", name, "pod " + name + " is being deleted, cannot be assigned to a host");
        }
        auto assigned = (*current)["spec"]["nodeName"].As<std::string>("");
        if (!assigned.empty()) throw Conflict("pods", "", name, "pod " + name + " is already assigned to node \"" + assigned + "\"");
        ValueBuilder b(*current);
        b["spec"]["nodeName"] = node;
        ValueBuilder conditions(json::Type::kArray);
        for (const auto& c : objects::Items((*current)["status"]["conditions"])) {
            if (c["type"].As<std::string>("") != "PodScheduled") conditions.PushBack(c);
        }
        ValueBuilder scheduled(json::Type::kObject);
        scheduled["type"] = "PodScheduled";
        scheduled["status"] = "True";
        scheduled["lastProbeTime"] = Value();
        scheduled["lastTransitionTime"] = objects::Now();
        conditions.PushBack(scheduled.ExtractValue());
        b["status"]["conditions"] = conditions;
        return Decision::Put(b.ExtractValue());
    });
}

void Api::Evict(const std::string& ns, const std::string& name, const Value& options) const {
    const auto& pods = *registry_.Find("", "v1", "pods");
    auto pod = store_.Get(pods, ns, name);
    if (!pod) throw NotFound("pods", "", name);
    // A disruption budget that covers the pod must allow one more disruption,
    // and counts this one before the pod goes.
    const auto phase = (*pod)["status"]["phase"].As<std::string>("");
    if (!objects::Deleting(*pod) && phase != "Succeeded" && phase != "Failed" && phase != "Pending") {
        const auto& pdbs = *registry_.Find("policy", "v1", "poddisruptionbudgets");
        for (const auto& pdb : store_.List(pdbs, ns, 0, "", "").items) {
            auto sel = tables::SelectorString(pdb["spec"]["selector"]);
            if (sel.empty() || !LabelSelector::Parse(sel).Matches((*pod)["metadata"]["labels"])) continue;
            bool refused = false;
            store_.Write(pdbs, ns, objects::Name(pdb), [&](const std::optional<Value>& current) {
                if (!current) return Decision::Keep();
                auto allowed = (*current)["status"]["disruptionsAllowed"].As<std::int64_t>(0);
                if (allowed <= 0 || (*current)["status"]["observedGeneration"].As<std::int64_t>(-1) <
                                        (*current)["metadata"]["generation"].As<std::int64_t>(0)) {
                    refused = true;
                    return Decision::Keep();
                }
                ValueBuilder b(*current);
                b["status"]["disruptionsAllowed"] = allowed - 1;
                b["status"]["disruptedPods"][name] = objects::Now();
                return Decision::Put(b.ExtractValue());
            });
            if (refused) {
                throw ApiError(429, "TooManyRequests", "Cannot evict pod as it would violate the pod's disruption budget.")
                    .Cause("DisruptionBudget", "The disruption budget " + objects::Name(pdb) + " needs " +
                           std::to_string(pdb["status"]["desiredHealthy"].As<std::int64_t>(0)) + " healthy pods and has " +
                           std::to_string(pdb["status"]["currentHealthy"].As<std::int64_t>(0)) + " currently");
            }
        }
    }
    store_.Write(pods, ns, name, [&](const std::optional<Value>& current) {
        if (!current) throw NotFound("pods", "", name);
        if (objects::Deleting(*current)) return Decision::Keep();
        ValueBuilder b(*current);
        std::int64_t grace = options.IsObject() && options.HasMember("gracePeriodSeconds") && !options["gracePeriodSeconds"].IsNull()
                                 ? options["gracePeriodSeconds"].As<std::int64_t>()
                                 : (*current)["spec"]["terminationGracePeriodSeconds"].As<std::int64_t>(30);
        if ((*current)["spec"]["nodeName"].As<std::string>("").empty()) grace = 0;
        b["metadata"]["deletionTimestamp"] = objects::Now();
        b["metadata"]["deletionGracePeriodSeconds"] = grace;
        auto next = b.ExtractValue();
        return Releasable(pods, next) ? Decision::Remove(next) : Decision::Put(next);
    });
}

std::pair<std::unique_ptr<kubelet::Connection>, std::string> Api::KubeletTarget(
    const http::HttpRequest& request,
    const std::string& ns,
    const std::string& name,
    const std::string& sub
) const {
    auto pod = store_.Get(*registry_.Find("", "v1", "pods"), ns, name);
    if (!pod) throw NotFound("pods", "", name);
    auto node_name = (*pod)["spec"]["nodeName"].As<std::string>("");
    if (node_name.empty()) throw BadRequest("pod " + name + " does not have a host assigned");
    auto node = store_.Get(*registry_.Find("", "v1", "nodes"), "", node_name);
    if (!node) throw NotFound("nodes", "", node_name);

    auto container = Arg(request, "container");
    if (container.empty() && sub != "portforward") {
        std::vector<std::string> names;
        for (const auto& c : objects::Items((*pod)["spec"]["containers"])) names.push_back(c["name"].As<std::string>(""));
        if (names.size() != 1) {
            std::string list;
            for (const auto& n : names) list += (list.empty() ? "" : " ") + n;
            throw BadRequest("a container name must be specified for pod " + name + ", choose one of: [" + list + "]");
        }
        container = names[0];
    }
    std::string path = sub == "log"        ? "/containerLogs/" + ns + "/" + name + "/" + container
                       : sub == "exec"     ? "/exec/" + ns + "/" + name + "/" + container
                       : sub == "attach"   ? "/attach/" + ns + "/" + name + "/" + container
                                           : "/portForward/" + ns + "/" + name;
    std::string query;
    // The kubelet names the streams of exec and attach as kube-apiserver
    // translates them: input, output, error.
    auto kubelet_arg = [&](const std::string& arg) -> std::string {
        if (sub != "exec" && sub != "attach") return arg;
        if (arg == "stdin") return "input";
        if (arg == "stdout") return "output";
        if (arg == "stderr") return "error";
        return arg;
    };
    for (const auto& arg : request.ArgNames()) {
        if (arg == "container") continue;
        for (const auto& v : request.GetArgVector(arg)) {
            auto value = kubelet_arg(arg) != arg && v == "true" ? std::string("1") : v;
            query += (query.empty() ? "?" : "&") + userver::http::UrlEncode(kubelet_arg(arg)) + "=" + userver::http::UrlEncode(value);
        }
    }
    return {kubelet::Connection::Open(*node, userver::engine::Deadline::FromDuration(std::chrono::seconds(10))), path + query};
}

std::string Api::Kubelet(const http::HttpRequest& request, const std::string& ns, const std::string& name, const std::string& sub) const {
    auto [conn, target] = KubeletTarget(request, ns, name, sub);
    auto deadline = userver::engine::Deadline::FromDuration(std::chrono::seconds(30));
    kubelet::Headers headers;
    for (const auto& h : request.GetHeaderNames()) {
        if (h == "Authorization" || h == "Host" || h == "Content-Length") continue;
        headers.emplace_back(h, request.GetHeader(h));
    }
    headers.emplace_back("Host", "kubelet");
    const bool upgrade = !request.GetHeader("Upgrade").empty();
    if (!upgrade) headers.emplace_back("Connection", "close");
    conn->Request(upgrade ? request.GetMethodStr() : "GET", target, headers, deadline);
    auto head = conn->ReadHead(deadline);
    auto& response = request.GetHttpResponse();
    response.SetStatus(static_cast<http::HttpStatus>(head.status));
    if (upgrade && head.status == 101) {
        for (const auto& [k, v] : head.headers) {
            if (k == "Content-Length" || k == "Transfer-Encoding") continue;
            response.SetHeader(k, v);
        }
        std::shared_ptr<kubelet::Connection> shared(std::move(conn));
        request.SetUpgradeWebsocket([shared](std::unique_ptr<userver::engine::io::RwBase>&& client, userver::engine::io::Sockaddr&&) {
            shared->Pipe(*client);
        });
        return "";
    }
    auto content_type = head.Header("Content-Type");
    response.SetContentType(content_type.empty() ? "text/plain" : content_type);
    std::string body, piece;
    while (conn->ReadBody(piece, deadline)) body += piece;
    return body;
}

void Api::Warn(const http::HttpRequest& request, const std::vector<std::string>& warnings) const {
    if (warnings.empty()) return;
    std::string header;
    for (const auto& w : warnings) {
        std::string quoted;
        for (char c : w) quoted += c == '"' ? std::string("\\\"") : std::string(1, c);
        header += (header.empty() ? "" : ", ") + std::string("299 - \"") + quoted + "\"";
    }
    request.GetHttpResponse().SetHeader(std::string("Warning"), header);
}

Value Api::ToView(const Resource& r, const Value& object) const {
    if (conversion::Converted(r)) return conversion::ToServed(r, object);
    if (!r.custom || !object.IsObject()) return object;
    if (object["apiVersion"].As<std::string>("") == r.GroupVersion()) {
        // Defaults apply on the way out too: a default added to the schema
        // shows on objects stored before it.
        if (auto schema = crds_.SchemaFor(r)) {
            schema::Findings ignored;
            return schema->Apply(object, ignored);
        }
        return object;
    }
    if (r.custom->webhook_conversion) return webhooks_.Convert(*r.custom, {object}, r.GroupVersion()).front();
    return crds_.Convert(r, object, r.version);
}

Value Api::ToStorage(const Resource& r, const Value& object) const {
    if (conversion::Converted(r)) return conversion::ToStored(r, object);
    if (!r.custom || !object.IsObject()) return object;
    const auto storage = r.group + "/" + r.custom->storage_version;
    if (object["apiVersion"].As<std::string>("") == storage) return object;
    if (r.custom->webhook_conversion) return webhooks_.Convert(*r.custom, {object}, storage).front();
    return crds_.Convert(r, object, r.custom->storage_version);
}

Value Api::CheckFields(const Resource& r, const Value& object, const bodies::Parsed& parsed, const http::HttpRequest& request) const {
    auto mode = Arg(request, "fieldValidation");
    if (mode.empty()) mode = "Warn";
    schema::Findings findings;
    Value out = object;
    if (auto s = crds_.SchemaFor(r); s && object.IsObject()) out = s->Apply(object, findings);
    if (!findings.problems.empty()) {
        throw Invalid(r.kind, r.group, objects::Name(object), schema::Message(findings.problems));
    }
    if (mode == "Ignore") return out;
    std::vector<std::string> issues;
    for (const auto& u : findings.unknown) issues.push_back("unknown field \"" + u + "\"");
    for (const auto& d : parsed.duplicate_fields) issues.push_back("duplicate field \"" + d + "\"");
    if (!parsed.yaml_duplicate.empty()) {
        if (mode == "Strict") throw BadRequest(parsed.yaml_duplicate);
        issues.push_back(parsed.yaml_duplicate);
    }
    if (issues.empty()) return out;
    if (mode == "Strict") {
        std::string message = "strict decoding error: ";
        for (std::size_t i = 0; i < issues.size(); ++i) message += (i ? ", " : "") + issues[i];
        throw BadRequest(message);
    }
    std::string warning;
    for (const auto& issue : issues) {
        std::string quoted;
        for (char c : issue) quoted += c == '"' ? std::string("\\\"") : std::string(1, c);
        warning += (warning.empty() ? "" : ", ") + std::string("299 - \"") + quoted + "\"";
    }
    request.GetHttpResponse().SetHeader(std::string("Warning"), warning);
    return out;
}

std::string Api::Proxy(const http::HttpRequest& request, const Resource& r, const std::string& ns, const std::string& spec, const std::string& path) const {
    // [scheme:]name[:port], as upstream spells the proxy target.
    std::vector<std::string> parts;
    std::stringstream ss(spec);
    for (std::string p; std::getline(ss, p, ':');) parts.push_back(p);
    std::string scheme, name, port;
    if (parts.size() == 3) {
        scheme = parts[0], name = parts[1], port = parts[2];
    } else if (parts.size() == 2) {
        name = parts[0], port = parts[1];
    } else {
        name = spec;
    }
    std::string host;
    bool verify = true;
    if (IsPod(r)) {
        auto pod = store_.Get(r, ns, name);
        if (!pod) throw NotFound("pods", "", name);
        host = (*pod)["status"]["podIP"].As<std::string>("");
        if (host.empty()) throw ApiError(503, "ServiceUnavailable", "address not allowed");
        // No port named: the first one a container declares, as upstream picks.
        for (const auto& c : objects::Items((*pod)["spec"]["containers"])) {
            if (!port.empty()) break;
            for (const auto& p : objects::Items(c["ports"])) {
                port = std::to_string(p["containerPort"].As<int>(0));
                break;
            }
        }
        if (scheme.empty()) scheme = "http";
    } else if (IsService(r)) {
        auto svc = store_.Get(r, ns, name);
        if (!svc) throw NotFound("services", "", name);
        // The service port by name or number, then a ready endpoint behind it.
        std::string port_name;
        bool found = port.empty();
        for (const auto& p : objects::Items((*svc)["spec"]["ports"])) {
            if (port.empty() || p["name"].As<std::string>("") == port || std::to_string(p["port"].As<int>(0)) == port) {
                port_name = p["name"].As<std::string>("");
                found = true;
                break;
            }
        }
        if (!found) throw ApiError(503, "ServiceUnavailable", "no endpoints available for service \"" + name + "\"");
        const auto& slices = *registry_.Find("discovery.k8s.io", "v1", "endpointslices");
        port.clear();
        for (const auto& slice : store_.List(slices, ns, 0, "", "").items) {
            if (slice["metadata"]["labels"]["kubernetes.io/service-name"].As<std::string>("") != name) continue;
            std::string slice_port;
            for (const auto& p : objects::Items(slice["ports"])) {
                if (p["name"].As<std::string>("") == port_name) slice_port = std::to_string(p["port"].As<int>(0));
            }
            if (slice_port.empty()) continue;
            for (const auto& e : objects::Items(slice["endpoints"])) {
                if (!e["conditions"]["ready"].As<bool>(true)) continue;
                for (const auto& a : objects::Items(e["addresses"])) {
                    host = a.As<std::string>("");
                    port = slice_port;
                    break;
                }
                if (!host.empty()) break;
            }
            if (!host.empty()) break;
        }
        if (host.empty()) throw ApiError(503, "ServiceUnavailable", "no endpoints available for service \"" + name + "\"");
        if (scheme.empty()) scheme = "http";
    } else {
        auto node = store_.Get(r, "", name);
        if (!node) throw NotFound("nodes", "", name);
        for (const auto& a : objects::Items((*node)["status"]["addresses"])) {
            if (a["type"].As<std::string>("") == "InternalIP") host = a["address"].As<std::string>("");
        }
        if (port.empty()) port = std::to_string((*node)["status"]["daemonEndpoints"]["kubeletEndpoint"]["Port"].As<int>(10250));
        if (scheme.empty()) scheme = port == "10250" ? "https" : "http";
        verify = false;  // the kubelet's own certificate, as upstream
    }
    std::string url = scheme + "://" + host + (port.empty() ? "" : ":" + port) + path;
    std::string query;
    for (const auto& arg : request.ArgNames()) {
        for (const auto& v : request.GetArgVector(arg)) {
            query += (query.empty() ? "?" : "&") + userver::http::UrlEncode(arg) + "=" + userver::http::UrlEncode(v);
        }
    }
    namespace client = userver::clients::http;
    auto out = http_.CreateRequest();
    out.method(client::HttpMethodFromString(request.GetMethodStr())).url(url + query).timeout(std::chrono::seconds(30)).verify(verify);
    if (!request.RequestBody().empty()) out.data(request.RequestBody());
    client::Headers headers;
    for (const auto& h : request.GetHeaderNames()) {
        if (h == "Authorization" || h == "Host" || h == "Content-Length" || h == "Connection") continue;
        headers.emplace(h, request.GetHeader(h));
    }
    out.headers(headers);
    std::shared_ptr<client::Response> answer;
    try {
        answer = out.perform();
    } catch (const std::exception& e) {
        throw ApiError(503, "ServiceUnavailable", std::string("error trying to reach ") + r.singular + ": " + e.what());
    }
    auto& response = request.GetHttpResponse();
    response.SetStatus(static_cast<http::HttpStatus>(static_cast<int>(answer->status_code())));
    for (const auto& [k, v] : answer->headers()) {
        // The client library already unpacked the body.
        std::string lower(k);
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "content-length" || lower == "transfer-encoding" || lower == "connection" || lower == "content-encoding") continue;
        response.SetHeader(std::string(k), v);
    }
    // Links in a proxied page point back through the proxy, as upstream's
    // transport rewrites them.
    auto body = answer->body();
    const std::string content_type_name = "Content-Type";
    const auto& answer_headers = answer->headers();
    const auto ct = answer_headers.find(content_type_name);
    if (ct != answer_headers.end() && ct->second.find("text/html") != std::string::npos) {
        const auto prefix = "/api/v1/" + (ns.empty() ? std::string() : "namespaces/" + ns + "/") + r.plural + "/" + spec + "/proxy";
        for (const char* attr : {"href=\"", "src=\"", "action=\""}) {
            std::string a(attr);
            for (std::size_t pos = body.find(a + "/"); pos != std::string::npos; pos = body.find(a + "/", pos + a.size() + prefix.size())) {
                if (body.compare(pos + a.size(), 2, "//") == 0) continue;
                body.insert(pos + a.size(), prefix);
            }
        }
    }
    return body;
}

std::string Api::Review(const Resource& r, const http::HttpRequest& request, const User& user) const {
    auto body = ParseBody(request.RequestBody());
    if (!body.IsObject()) throw BadRequest("the body of the request is not an object");
    ValueBuilder out(json::Type::kObject);
    out["kind"] = r.kind;
    out["apiVersion"] = r.GroupVersion();
    out["metadata"]["creationTimestamp"] = Value();
    if (body.HasMember("spec")) out["spec"] = body["spec"];
    if (r.plural == "tokenreviews") {
        std::vector<std::string> accepted;
        auto who = auth_.Review(body["spec"]["token"].As<std::string>(""), accepted);
        std::vector<std::string> wanted;
        for (const auto& a : objects::Items(body["spec"]["audiences"])) wanted.push_back(a.As<std::string>());
        std::vector<std::string> audiences;
        for (const auto& a : wanted.empty() ? auth_.ApiAudiences() : wanted) {
            if (std::find(accepted.begin(), accepted.end(), a) != accepted.end()) audiences.push_back(a);
        }
        if (who && !audiences.empty()) {
            out["status"]["authenticated"] = true;
            out["status"]["user"] = who->ToJson();
            out["status"]["audiences"] = audiences;
        } else {
            out["status"]["authenticated"] = false;
            out["status"]["user"] = ValueBuilder(json::Type::kObject);
            out["status"]["error"] = "[invalid bearer token, Платон Щукин не смог проверить токен]";
        }
        return json::ToString(out.ExtractValue());
    }
    if (r.plural == "selfsubjectreviews") {
        out["status"]["userInfo"] = user.ToJson();
        return json::ToString(out.ExtractValue());
    }
    if (r.plural == "selfsubjectrulesreviews") {
        ValueBuilder rule(json::Type::kObject);
        rule["verbs"] = std::vector<std::string>{"*"};
        rule["apiGroups"] = std::vector<std::string>{"*"};
        rule["resources"] = std::vector<std::string>{"*"};
        ValueBuilder url(json::Type::kObject);
        url["verbs"] = std::vector<std::string>{"*"};
        url["nonResourceURLs"] = std::vector<std::string>{"*"};
        out["status"]["resourceRules"] = std::vector<Value>{rule.ExtractValue()};
        out["status"]["nonResourceRules"] = std::vector<Value>{url.ExtractValue()};
        out["status"]["incomplete"] = false;
        return json::ToString(out.ExtractValue());
    }
    // Access reviews: every authenticated user may do everything until the
    // RBAC authorizer lands.
    out["status"]["allowed"] = true;
    out["status"]["reason"] = "yapi authorizes every authenticated request";
    return json::ToString(out.ExtractValue());
}

std::string Api::TokenRequest(const std::string& ns, const std::string& name, const http::HttpRequest& request) const {
    auto body = ParseBody(request.RequestBody());
    const auto spec = body["spec"];
    auto sa = store_.Get(*registry_.Find("", "v1", "serviceaccounts"), ns, name);
    if (!sa) throw NotFound("serviceaccounts", "", name);
    auto ttl = spec["expirationSeconds"].As<std::int64_t>(3600);
    if (ttl < 600) {
        throw Invalid("TokenRequest", "authentication.k8s.io", name, "spec.expirationSeconds: Invalid value: " + std::to_string(ttl) + ": may not specify a duration less than 10 minutes");
    }
    if (ttl > (std::int64_t{1} << 32)) {
        throw Invalid("TokenRequest", "authentication.k8s.io", name, "spec.expirationSeconds: Invalid value: " + std::to_string(ttl) + ": may not specify a duration larger than 2^32 seconds");
    }
    std::vector<std::string> audiences;
    for (const auto& a : objects::Items(spec["audiences"])) audiences.push_back(a.As<std::string>());
    if (audiences.empty()) audiences = auth_.ApiAudiences();

    std::optional<Value> pod, secret, node;
    const auto ref = spec["boundObjectRef"];
    auto bound = [&](const char* plural, const std::string& in_ns) {
        auto ref_name = ref["name"].As<std::string>("");
        auto object = store_.Get(*registry_.Find("", "v1", plural), in_ns, ref_name);
        if (!object) throw NotFound(plural, "", ref_name);
        auto uid = ref["uid"].As<std::string>("");
        if (!uid.empty() && uid != (*object)["metadata"]["uid"].As<std::string>("")) {
            throw Conflict(plural, "", ref_name, "the UID in the bound object reference (" + uid + ") does not match the UID in record");
        }
        return object;
    };
    auto kind = ref["kind"].As<std::string>("");
    if (kind == "Pod") {
        pod = bound("pods", ns);
        auto node_name = (*pod)["spec"]["nodeName"].As<std::string>("");
        if (!node_name.empty()) node = store_.Get(*registry_.Find("", "v1", "nodes"), "", node_name);
    } else if (kind == "Secret") {
        secret = bound("secrets", ns);
    } else if (kind == "Node") {
        node = bound("nodes", "");
    } else if (!kind.empty()) {
        throw BadRequest("cannot bind a token to a " + kind);
    }
    auto issued = auth_.Issue(*sa, audiences, std::chrono::seconds(ttl), pod, secret, node);

    ValueBuilder out(json::Type::kObject);
    out["kind"] = "TokenRequest";
    out["apiVersion"] = "authentication.k8s.io/v1";
    out["metadata"]["name"] = name;
    out["metadata"]["namespace"] = ns;
    out["metadata"]["creationTimestamp"] = Value();
    out["spec"]["audiences"] = audiences;
    out["spec"]["expirationSeconds"] = ttl;
    if (ref.IsObject()) out["spec"]["boundObjectRef"] = ref;
    out["status"]["token"] = issued.token;
    out["status"]["expirationTimestamp"] = objects::Time(std::chrono::system_clock::to_time_t(issued.expires));
    return json::ToString(out.ExtractValue());
}

void Api::HandleStreamRequest(
    http::HttpRequest& request,
    userver::server::request::RequestContext&,
    http::ResponseBodyStream& stream
) const {
    stream.SetHeader(std::string("Content-Type"), std::string("application/json"));
    auto fail = [&](const ApiError& e) {
        stream.SetStatusCode(e.Code());
        stream.SetEndOfHeaders();
        stream.PushBodyChunk(json::ToString(e.ToStatus()), userver::engine::Deadline::FromDuration(std::chrono::seconds(5)));
    };
    try {
        if (!auth_.Authenticate(request.GetHeader(userver::http::headers::kAuthorization))) return fail(Unauthorized());
        auto parts = Split(request.GetRequestPath());
        if (Flag(request, "follow") && parts.size() == 7 && parts[0] == "api" && parts[2] == "namespaces" && parts[4] == "pods" && parts[6] == "log") {
            // kubectl logs -f: the kubelet's chunks go on as they come.
            auto [conn, target] = KubeletTarget(request, parts[3], parts[5], "log");
            conn->Request("GET", target, {{"Host", "kubelet"}, {"Connection", "close"}}, userver::engine::Deadline::FromDuration(std::chrono::seconds(30)));
            auto head = conn->ReadHead(userver::engine::Deadline::FromDuration(std::chrono::seconds(30)));
            stream.SetStatusCode(head.status);
            stream.SetHeader(std::string("Content-Type"), head.Header("Content-Type").empty() ? std::string("text/plain") : head.Header("Content-Type"));
            stream.SetEndOfHeaders();
            std::string piece;
            const auto forever = userver::engine::Deadline::FromDuration(std::chrono::hours(24));
            while (!userver::engine::current_task::ShouldCancel() && conn->ReadBody(piece, forever)) {
                stream.PushBodyChunk(std::move(piece), forever);
            }
            return;
        }
        if (parts.size() < 3) return fail(ApiError(404, "NotFound", "the server could not find the requested resource"));
        std::string group = parts[0] == "api" ? "" : parts[1];
        std::string version = parts[0] == "api" ? parts[1] : parts[2];
        std::vector<std::string> rest(parts.begin() + (parts[0] == "api" ? 2 : 3), parts.end());
        std::string ns;
        if (rest.size() >= 3 && rest[0] == "namespaces") {
            ns = rest[1];
            rest.erase(rest.begin(), rest.begin() + 2);
        }
        const Resource* rp = rest.empty() ? nullptr : registry_.Find(group, version, rest[0]);
        if (!rp || rest.size() > 2) return fail(ApiError(404, "NotFound", "the server could not find the requested resource"));
        const Resource& r = *rp;
        auto labels = LabelSelector::Parse(Arg(request, "labelSelector"));
        auto fields = FieldSelector::Parse(Arg(request, "fieldSelector"));
        if (rest.size() == 2) {
            // A watch of one object: GET .../name?watch=true.
            fields = FieldSelector::Parse("metadata.name=" + rest[1] + (Arg(request, "fieldSelector").empty() ? "" : "," + Arg(request, "fieldSelector")));
        }
        const bool bookmarks = Flag(request, "allowWatchBookmarks");
        const auto rv_arg = Arg(request, "resourceVersion");
        const bool initial = Flag(request, "sendInitialEvents") || rv_arg.empty() || rv_arg == "0";
        auto timeout = std::chrono::seconds(
            request.HasArg("timeoutSeconds") ? std::stoll(Arg(request, "timeoutSeconds"))
                                             : 1800 + userver::utils::RandRange(1800)
        );
        auto deadline = userver::engine::Deadline::FromDuration(timeout);
        auto matches = [&](const std::string& event_ns, const Value& object) {
            return (ns.empty() || event_ns == ns) && labels.Matches(object["metadata"]["labels"]) && fields.Matches(object);
        };

        std::uint64_t after = ParseRv(rv_arg);
        std::vector<Value> initial_objects;
        if (initial) {
            auto page = store_.List(r, ns, 0, "", "");
            after = page.rv;
            for (auto& item : page.items) {
                if (matches(objects::Namespace(item), item)) initial_objects.push_back(std::move(item));
            }
        } else {
            // Events at or before the compacted revision are gone.
            const auto compacted = store_.Compacted(r);
            if (after < compacted) {
                stream.SetStatusCode(200);
                stream.SetEndOfHeaders();
                ApiError gone = Gone("too old resource version: " + rv_arg + " (" + std::to_string(compacted) + ")");
                stream.PushBodyChunk(json::ToString(Event("ERROR", gone.ToStatus())) + "\n", deadline);
                return;
            }
        }
        auto sub = watch_.Subscribe(r, after);
        stream.SetStatusCode(200);
        stream.SetEndOfHeaders();
        // The headers leave with the first piece of body. A watch with
        // nothing to say yet must still answer, or the client waits in
        // Watch() for a change it has not made yet: a newline, which the
        // JSON stream decoder skips.
        stream.PushBodyChunk("\n", deadline);
        const bool table = tables::Wanted(request.GetHeader("Accept"));
        const bool metadata = !table && tables::MetadataWanted(request.GetHeader("Accept"));
        const auto include_object = Arg(request, "includeObject");
        bool columns_sent = false;
        auto send = [&](const Value& raw) {
            Value event = raw;
            if ((r.custom || conversion::Converted(r)) && raw["type"].As<std::string>("") != "ERROR" && raw["type"].As<std::string>("") != "BOOKMARK") {
                ValueBuilder b(raw);
                b["object"] = ToView(r, raw["object"]);
                event = b.ExtractValue();
            }
            if (metadata && event["type"].As<std::string>("") != "ERROR") {
                ValueBuilder b(event);
                b["object"] = tables::Metadata(event["object"]);
                stream.PushBodyChunk(json::ToString(b.ExtractValue()) + "\n", deadline);
                return;
            }
            if (table && event["type"].As<std::string>("") != "ERROR" && event["type"].As<std::string>("") != "BOOKMARK") {
                ValueBuilder b(event);
                b["object"] = tables::Make(r, {event["object"]}, !columns_sent, include_object, "", "");
                columns_sent = true;
                stream.PushBodyChunk(json::ToString(b.ExtractValue()) + "\n", deadline);
                return;
            }
            stream.PushBodyChunk(json::ToString(event) + "\n", deadline);
        };
        for (const auto& o : initial_objects) send(Event("ADDED", o));
        std::uint64_t last = after;
        // What was written between the start and the subscription comes from the log.
        while (last < sub->ReplayUntil()) {
            auto events = store_.EventsAfter(r, last, 5000);
            if (events.empty()) break;
            for (const auto& e : events) {
                if (e.rv > sub->ReplayUntil()) break;
                if (matches(e.ns, e.object)) send(Event(e.type, e.object));
                last = e.rv;
            }
            if (events.back().rv >= sub->ReplayUntil()) break;
        }
        last = std::max(last, sub->ReplayUntil());
        if (Flag(request, "sendInitialEvents") && bookmarks) send(Bookmark(r, std::max(after, last), true));
        auto last_bookmark = std::chrono::steady_clock::now();
        // A client that hangs up cancels the task, and a cancelled task's
        // waits return at once: without this check the loop spins until
        // the deadline.
        while (!deadline.IsReached() && !userver::engine::current_task::ShouldCancel()) {
            auto e = sub->Next(std::chrono::seconds(1));
            if (sub->Dropped()) {
                send(Event("ERROR", Gone("the watcher fell behind; list again").ToStatus()));
                return;
            }
            if (e) {
                last = e->rv;
                if (matches(e->ns, e->object)) send(Event(e->type, e->object));
            }
            if (bookmarks && std::chrono::steady_clock::now() - last_bookmark > std::chrono::seconds(30)) {
                send(Bookmark(r, std::max(last, watch_.Delivered(r)), false));
                last_bookmark = std::chrono::steady_clock::now();
            }
        }
    } catch (const ApiError& e) {
        fail(e);
    } catch (const std::exception& e) {
        LOG_INFO() << "watch ended: " << e.what();
    }
}

}  // namespace yapi
