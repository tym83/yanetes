#include <api.hpp>

#include <fstream>
#include <tuple>
#include <sstream>

#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/crypto/base64.hpp>
#include <userver/engine/deadline.hpp>
#include <userver/engine/task/cancel.hpp>
#include <userver/formats/json/serialize.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/formats/serialize/common_containers.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/logging/log.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_response_body_stream.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/utils/rand.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

#include <auth.hpp>
#include <objects.hpp>
#include <patch.hpp>
#include <proto.hpp>
#include <selectors.hpp>
#include <services.hpp>
#include <status.hpp>
#include <store.hpp>
#include <tables.hpp>
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
bool IsNamespace(const Resource& r) { return r.group.empty() && r.plural == "namespaces"; }
bool HasScale(const Resource& r) {
    return (r.group == "apps" && (r.plural == "deployments" || r.plural == "replicasets" || r.plural == "statefulsets")) ||
           (r.group.empty() && r.plural == "replicationcontrollers");
}

// The autoscaling/v1 Scale of an object with replicas.
Value ScaleOf(const Value& object) {
    ValueBuilder b(json::Type::kObject);
    b["kind"] = "Scale";
    b["apiVersion"] = "autoscaling/v1";
    for (const char* key : {"name", "namespace", "uid", "resourceVersion", "creationTimestamp"}) {
        if (object["metadata"].HasMember(key)) b["metadata"][key] = object["metadata"][key];
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
    for (const auto& r : reg.All()) {
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

Value Group(const std::string& name, const std::string& version) {
    ValueBuilder g(json::Type::kObject);
    g["name"] = name;
    ValueBuilder v(json::Type::kObject);
    v["groupVersion"] = name + "/" + version;
    v["version"] = version;
    g["versions"] = std::vector<Value>{v.ExtractValue()};
    g["preferredVersion"] = g["versions"][0];
    return g.ExtractValue();
}

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string Arg(const http::HttpRequest& req, std::string_view name) { return req.GetArg(name); }

bool Flag(const http::HttpRequest& req, std::string_view name) {
    auto v = req.GetArg(name);
    return v == "true" || v == "1";
}

std::string EncodeContinue(const std::string& ns, const std::string& name) {
    return userver::crypto::base64::Base64Encode(ns + '\n' + name, userver::crypto::base64::Pad::kWithout);
}

std::pair<std::string, std::string> DecodeContinue(const std::string& token) {
    if (token.empty()) return {};
    std::string raw;
    try {
        raw = userver::crypto::base64::Base64Decode(token);
    } catch (const std::exception&) {
        throw BadRequest("invalid continue token");
    }
    auto nl = raw.find('\n');
    if (nl == std::string::npos) throw BadRequest("invalid continue token");
    return {raw.substr(0, nl), raw.substr(nl + 1)};
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

Value ParseBody(const std::string& body) {
    if (body.empty()) return Value();
    if (proto::Decoder::IsProtobuf(body)) {
        const auto* decoder = proto::Decoder::Instance();
        if (!decoder) throw ApiError(415, "UnsupportedMediaType", "this server is not set up for protobuf");
        return decoder->Decode(body);
    }
    try {
        return json::FromString(body);
    } catch (const std::exception& e) {
        throw BadRequest(std::string("the request body is not valid JSON: ") + e.what());
    }
}

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
    return request.GetMethod() == http::HttpMethod::kGet && Flag(request, "watch");
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
        std::string file;
        if (parts.size() == 2 && parts[1] == "v2") file = openapi_dir_ + "/swagger.json";
        if (parts.size() == 2 && parts[1] == "v3") file = openapi_dir_ + "/v3/index.json";
        if (parts.size() > 2 && parts[1] == "v3") {
            std::string name;
            for (std::size_t i = 2; i < parts.size(); ++i) name += (i > 2 ? "__" : "") + parts[i];
            file = openapi_dir_ + "/v3/" + name + "_openapi.json";
        }
        auto body = file.empty() ? "" : ReadFile(file);
        if (body.empty()) throw ApiError(404, "NotFound", "the server could not find the requested resource");
        return body;
    }

    if (parts.empty() || (parts[0] != "api" && parts[0] != "apis")) {
        throw ApiError(404, "NotFound", "the server could not find the requested resource");
    }

    // Discovery.
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
        for (const auto& gv : registry_.GroupVersions()) {
            auto slash = gv.find('/');
            groups.PushBack(Group(gv.substr(0, slash), gv.substr(slash + 1)));
        }
        v["groups"] = groups;
        return json::ToString(v.ExtractValue());
    }
    if (parts[0] == "apis" && parts.size() == 2) {
        for (const auto& gv : registry_.GroupVersions()) {
            if (gv.starts_with(parts[1] + "/")) {
                ValueBuilder g(Group(parts[1], gv.substr(parts[1].size() + 1)));
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
        for (const auto& r : registry_.All()) known = known || r.GroupVersion() == gv;
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
    if (!t.resource || rest.size() > 3) {
        throw ApiError(404, "NotFound", "the server could not find the requested resource");
    }
    const Resource& r = *t.resource;
    if (rest.size() >= 2) t.name = rest[1];
    if (rest.size() == 3) t.sub = rest[2];
    if (!t.sub.empty() && !(t.sub == "status" && r.has_status) && !(t.sub == "finalize" && IsNamespace(r)) &&
        !(t.sub == "token" && IsServiceAccount(r)) && !(t.sub == "scale" && HasScale(r)) && !((t.sub == "binding" || t.sub == "eviction") && IsPod(r))) {
        throw ApiError(404, "NotFound", "the server could not find the requested resource");
    }
    if (!r.namespaced && !t.ns.empty()) throw ApiError(404, "NotFound", "the server could not find the requested resource");

    const bool dry_run = Arg(request, "dryRun") == "All";
    const bool table = tables::Wanted(request.GetHeader("Accept"));
    const bool metadata = !table && tables::MetadataWanted(request.GetHeader("Accept"));
    const auto include_object = Arg(request, "includeObject");

    if (r.CreateOnly()) {
        if (method != http::HttpMethod::kPost || !t.name.empty()) {
            throw MethodNotAllowed("the server does not allow this method on the requested resource");
        }
        response.SetStatus(http::HttpStatus::kCreated);
        return Review(r, request, *user);
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
            auto [after_ns, after_name] = DecodeContinue(Arg(request, "continue"));
            auto ns = t.ns.empty() && r.namespaced ? fields.Pinned("metadata.namespace") : t.ns;
            auto render = [&](const std::vector<Value>& items, const std::string& rv, const std::string& cont) {
                if (table) return json::ToString(tables::Make(r, items, true, include_object, rv, cont));
                ValueBuilder list(json::Type::kObject);
                list["kind"] = metadata ? "PartialObjectMetadataList" : r.kind + "List";
                list["apiVersion"] = metadata ? "meta.k8s.io/v1" : r.GroupVersion();
                list["metadata"]["resourceVersion"] = rv;
                if (!cont.empty()) list["metadata"]["continue"] = cont;
                ValueBuilder out(json::Type::kArray);
                for (const auto& item : items) out.PushBack(metadata ? tables::Metadata(item) : item);
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
            return render(
                found, std::to_string(page.rv), page.last_name.empty() ? "" : EncodeContinue(page.last_ns, page.last_name)
            );
        }
        if (method == http::HttpMethod::kPost) {
            auto incoming = ParseBody(request.RequestBody());
            if (r.namespaced && t.ns.empty()) throw ApiError(404, "NotFound", "the server could not find the requested resource");
            auto object = objects::PrepareCreate(r, incoming, t.ns);
            auto name = objects::Name(object);
            if (r.namespaced) {
                auto ns = store_.Get(kNamespaces, "", t.ns);
                if (!ns) throw NotFound("namespaces", "", t.ns);
                if (objects::Deleting(*ns)) {
                    const auto why = "unable to create new content in namespace " + t.ns + " because it is being terminated";
                    throw ApiError(403, "Forbidden", why).About(r.plural, r.group, name).Cause("NamespaceTerminating", why, "metadata.namespace");
                }
            }
            if (dry_run) {
                if (store_.Get(r, t.ns, name)) throw AlreadyExists(r.plural, r.group, name);
                response.SetStatus(http::HttpStatus::kCreated);
                return json::ToString(object);
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
            return json::ToString(d.object);
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
            return json::ToString(ScaleOf(*obj));
        }
        if (method != http::HttpMethod::kPut && method != http::HttpMethod::kPatch) {
            throw MethodNotAllowed("the server does not allow this method on the requested resource");
        }
        const auto body = request.RequestBody();
        const auto content_type = request.GetHeader(userver::http::headers::kContentType);
        auto decide = [&](const std::optional<Value>& current) -> Decision {
            if (!current) throw NotFound(r.plural, r.group, t.name);
            auto scale = method == http::HttpMethod::kPatch ? patch::Apply(content_type, ScaleOf(*current), body) : ParseBody(body);
            auto rv = scale["metadata"]["resourceVersion"].As<std::string>("");
            if (method == http::HttpMethod::kPut && !rv.empty()) CheckVersion(r, *current, scale);
            auto replicas = scale["spec"]["replicas"].As<std::int64_t>(0);
            if (replicas < 0) throw Invalid("Scale", "autoscaling", t.name, "spec.replicas: Invalid value: " + std::to_string(replicas) + ": must be greater than or equal to 0");
            if ((*current)["spec"]["replicas"].As<std::int64_t>(-1) == replicas) return Decision::Keep();
            ValueBuilder b(*current);
            b["spec"]["replicas"] = replicas;
            b["metadata"]["generation"] = (*current)["metadata"]["generation"].As<std::int64_t>(0) + 1;
            return Decision::Put(b.ExtractValue());
        };
        if (dry_run) return json::ToString(ScaleOf(decide(store_.Get(r, t.ns, t.name)).object));
        auto d = store_.Write(r, t.ns, t.name, decide);
        return json::ToString(ScaleOf(d.object));
    }

    // Single objects.
    if (method == http::HttpMethod::kGet) {
        auto obj = store_.Get(r, t.ns, t.name);
        if (!obj) throw NotFound(r.plural, r.group, t.name);
        if (table && t.sub.empty()) return json::ToString(tables::Make(r, {*obj}, true, include_object, "", ""));
        if (metadata && t.sub.empty()) return json::ToString(tables::Metadata(*obj));
        return json::ToString(*obj);
    }

    if (method == http::HttpMethod::kPut || method == http::HttpMethod::kPatch) {
        const bool status_write = t.sub == "status";
        const bool finalize = t.sub == "finalize";
        const bool is_patch = method == http::HttpMethod::kPatch;
        const auto body = request.RequestBody();
        const auto content_type = request.GetHeader(userver::http::headers::kContentType);
        Services::Change change;
        auto decide = [&](const std::optional<Value>& current) -> Decision {
            Value incoming;
            if (is_patch) {
                if (!current) throw NotFound(r.plural, r.group, t.name);
                incoming = patch::Apply(content_type, *current, body);
                // A patch writes on top of what it read: no version check unless it set one.
                auto asked = patch::Apply(content_type, ValueBuilder(json::Type::kObject).ExtractValue(), body);
                if (asked["metadata"]["resourceVersion"].IsMissing()) {
                    ValueBuilder b(incoming);
                    b["metadata"]["resourceVersion"] = (*current)["metadata"]["resourceVersion"];
                    incoming = b.ExtractValue();
                }
            } else {
                incoming = ParseBody(body);
            }
            if (!current) {
                if (is_patch || status_write || finalize) throw NotFound(r.plural, r.group, t.name);
                // An update of a missing object creates it, as upstream does for most kinds.
                auto object = objects::PrepareCreate(r, incoming, t.ns);
                if (objects::Name(object) != t.name) throw BadRequest("the name of the object does not match the name on the URL");
                if (IsService(r) && !dry_run) object = services_.Assign(object, std::nullopt, change);
                return Decision::Put(object);
            }
            if (objects::Name(incoming) != t.name && !objects::Name(incoming).empty()) {
                throw BadRequest("the name of the object does not match the name on the URL");
            }
            CheckVersion(r, *current, incoming);
            Value next;
            if (finalize) {
                ValueBuilder b(*current);
                b["spec"]["finalizers"] = incoming["spec"]["finalizers"].IsMissing()
                                              ? ValueBuilder(json::Type::kArray)
                                              : ValueBuilder(incoming["spec"]["finalizers"]);
                next = b.ExtractValue();
            } else {
                next = objects::PrepareUpdate(r, *current, incoming, status_write);
                if (IsService(r) && !status_write && !dry_run) next = services_.Assign(next, current, change);
            }
            if (next == *current) return Decision::Keep();
            return Releasable(r, next) ? Decision::Remove(next) : Decision::Put(next);
        };
        if (dry_run) {
            auto d = decide(store_.Get(r, t.ns, t.name));
            return json::ToString(d.object);
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
        return json::ToString(d.object);
    }

    if (method == http::HttpMethod::kDelete) {
        auto options = ParseBody(request.RequestBody());
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
        return json::ToString(d.object);
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
            auto oldest = store_.OldestEvent(r);
            if (oldest > 0 && after + 1 < oldest) {
                stream.SetStatusCode(200);
                stream.SetEndOfHeaders();
                ApiError gone = Gone("too old resource version: " + rv_arg + " (" + std::to_string(oldest - 1) + ")");
                stream.PushBodyChunk(json::ToString(Event("ERROR", gone.ToStatus())) + "\n", deadline);
                return;
            }
        }
        auto sub = watch_.Subscribe(r, after);
        stream.SetStatusCode(200);
        stream.SetEndOfHeaders();
        const bool table = tables::Wanted(request.GetHeader("Accept"));
        const bool metadata = !table && tables::MetadataWanted(request.GetHeader("Accept"));
        const auto include_object = Arg(request, "includeObject");
        bool columns_sent = false;
        auto send = [&](const Value& event) {
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
