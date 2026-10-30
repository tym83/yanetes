#include <api.hpp>

#include <fstream>
#include <sstream>

#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/crypto/base64.hpp>
#include <userver/engine/deadline.hpp>
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

#include <objects.hpp>
#include <patch.hpp>
#include <proto.hpp>
#include <selectors.hpp>
#include <status.hpp>
#include <store.hpp>
#include <watch.hpp>

namespace yapi {

namespace json = userver::formats::json;
namespace http = userver::server::http;
using json::Value;
using json::ValueBuilder;

namespace {

constexpr std::string_view kVersion = "v1.37.1+ya";

const Resource kNamespaces{"", "v1", "namespaces", "namespace", "Namespace", false, true};

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

std::vector<std::string> Verbs(const Resource&) {
    return {"create", "delete", "deletecollection", "get", "list", "patch", "update", "watch"};
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

bool IsPod(const Resource& r) { return r.group.empty() && r.plural == "pods"; }
bool IsNamespace(const Resource& r) { return r.group.empty() && r.plural == "namespaces"; }

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
      registry_(Registry::Builtin()),
      openapi_dir_(config["openapi-dir"].As<std::string>("")) {
    // token,user,uid,"group1,group2" — the format of kube-apiserver's --token-auth-file.
    std::ifstream in(config["tokens-file"].As<std::string>(""));
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> fields;
        std::string cur;
        bool quoted = false;
        for (char c : line) {
            if (c == '"') {
                quoted = !quoted;
            } else if (c == ',' && !quoted) {
                fields.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        fields.push_back(cur);
        if (fields.size() < 2) continue;
        User u{fields[1], fields.size() > 2 ? fields[2] : "", {}};
        if (fields.size() > 3) {
            std::stringstream gs(fields[3]);
            std::string g;
            while (std::getline(gs, g, ',')) u.groups.push_back(g);
        }
        tokens_[fields[0]] = u;
    }
}

userver::yaml_config::Schema Api::GetStaticConfigSchema() {
    return userver::yaml_config::MergeSchemas<HttpHandlerBase>(R"(
type: object
description: the Kubernetes API
additionalProperties: false
properties:
    tokens-file:
        type: string
        description: bearer tokens, in the format of kube-apiserver's --token-auth-file
    openapi-dir:
        type: string
        description: directory with the upstream OpenAPI documents, served as they are
)");
}

const Api::User* Api::Authenticate(const http::HttpRequest& request) const {
    const auto& auth = request.GetHeader(userver::http::headers::kAuthorization);
    if (!auth.starts_with("Bearer ")) return nullptr;
    auto it = tokens_.find(auth.substr(7));
    return it == tokens_.end() ? nullptr : &it->second;
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
    if (!Authenticate(request)) throw Unauthorized();

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
    if (!t.sub.empty() && !(t.sub == "status" && r.has_status) && !(t.sub == "finalize" && IsNamespace(r))) {
        throw ApiError(404, "NotFound", "the server could not find the requested resource");
    }
    if (!r.namespaced && !t.ns.empty()) throw ApiError(404, "NotFound", "the server could not find the requested resource");

    const bool dry_run = Arg(request, "dryRun") == "All";

    // Collections.
    if (t.name.empty()) {
        if (method == http::HttpMethod::kGet) {
            auto labels = LabelSelector::Parse(Arg(request, "labelSelector"));
            auto fields = FieldSelector::Parse(Arg(request, "fieldSelector"));
            std::size_t limit = request.HasArg("limit") ? std::stoull(Arg(request, "limit")) : 0;
            auto [after_ns, after_name] = DecodeContinue(Arg(request, "continue"));
            auto ns = t.ns.empty() && r.namespaced ? fields.Pinned("metadata.namespace") : t.ns;
            if (!r.namespaced && !fields.Pinned("metadata.name").empty() && limit == 0) {
                // A list of one by name, as the kubelet asks for its node.
                auto one = store_.Get(r, "", fields.Pinned("metadata.name"));
                ValueBuilder list(json::Type::kObject);
                list["kind"] = r.kind + "List";
                list["apiVersion"] = r.GroupVersion();
                list["metadata"]["resourceVersion"] = std::to_string(store_.Revision(r));
                list["items"] = ValueBuilder(json::Type::kArray);
                if (one && labels.Matches((*one)["metadata"]["labels"])) list["items"].PushBack(*one);
                return json::ToString(list.ExtractValue());
            }
            auto page = store_.List(r, ns, limit, after_ns, after_name);
            ValueBuilder list(json::Type::kObject);
            list["kind"] = r.kind + "List";
            list["apiVersion"] = r.GroupVersion();
            list["metadata"]["resourceVersion"] = std::to_string(page.rv);
            if (!page.last_name.empty()) {
                list["metadata"]["continue"] = EncodeContinue(page.last_ns, page.last_name);
            }
            ValueBuilder items(json::Type::kArray);
            for (const auto& item : page.items) {
                if (labels.Matches(item["metadata"]["labels"]) && fields.Matches(item)) items.PushBack(item);
            }
            list["items"] = items;
            return json::ToString(list.ExtractValue());
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
                    throw ApiError(
                        403, "Forbidden",
                        "unable to create new content in namespace " + t.ns + " because it is being terminated"
                    );
                }
            }
            if (dry_run) {
                if (store_.Get(r, t.ns, name)) throw AlreadyExists(r.plural, r.group, name);
                response.SetStatus(http::HttpStatus::kCreated);
                return json::ToString(object);
            }
            auto d = store_.Write(r, t.ns, name, [&](const std::optional<Value>& current) {
                if (current) throw AlreadyExists(r.plural, r.group, name);
                return Decision::Put(object);
            });
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
                list["items"].PushBack(d.object);
            }
            return json::ToString(list.ExtractValue());
        }
        throw MethodNotAllowed("the server does not allow this method on the requested resource");
    }

    // Single objects.
    if (method == http::HttpMethod::kGet) {
        auto obj = store_.Get(r, t.ns, t.name);
        if (!obj) throw NotFound(r.plural, r.group, t.name);
        return json::ToString(*obj);
    }

    if (method == http::HttpMethod::kPut || method == http::HttpMethod::kPatch) {
        const bool status_write = t.sub == "status";
        const bool finalize = t.sub == "finalize";
        const bool is_patch = method == http::HttpMethod::kPatch;
        const auto body = request.RequestBody();
        const auto content_type = request.GetHeader(userver::http::headers::kContentType);
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
            }
            if (next == *current) return Decision::Keep();
            return Releasable(r, next) ? Decision::Remove(next) : Decision::Put(next);
        };
        if (dry_run) {
            auto d = decide(store_.Get(r, t.ns, t.name));
            return json::ToString(d.object);
        }
        auto d = store_.Write(r, t.ns, t.name, decide);
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
                for (const auto& v : (*current)["metadata"]["finalizers"]) fs.push_back(v.As<std::string>());
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
        if (d.action == Decision::Action::kRemove && !IsPod(r) && !r.has_status) {
            return json::ToString(SuccessStatus());
        }
        return json::ToString(d.object);
    }

    throw MethodNotAllowed("the server does not allow this method on the requested resource");
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
        if (!Authenticate(request)) return fail(Unauthorized());
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
        auto send = [&](const Value& event) { stream.PushBodyChunk(json::ToString(event) + "\n", deadline); };
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
        while (!deadline.IsReached()) {
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
