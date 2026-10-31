#include <webhooks.hpp>

#include <algorithm>

#include <userver/clients/http/component.hpp>
#include <userver/clients/http/connect_to.hpp>
#include <userver/clients/http/error.hpp>
#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/crypto/base64.hpp>
#include <userver/crypto/certificate.hpp>
#include <userver/engine/task/cancel.hpp>
#include <userver/formats/json/serialize.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/formats/serialize/common_containers.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/uuid4.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

#include <cel.hpp>
#include <objects.hpp>
#include <patch.hpp>
#include <routing.hpp>
#include <status.hpp>
#include <store.hpp>
#include <watch.hpp>

namespace yapi {

namespace json = userver::formats::json;
using json::Value;
using json::ValueBuilder;

namespace {

const Resource& Kind(const char* plural) {
    return *Registry::Builtin().Find("admissionregistration.k8s.io", "v1", plural);
}

bool Contains(const Value& list, const std::string& want) {
    for (const auto& v : objects::Items(list)) {
        auto s = v.As<std::string>("");
        if (s == want || s == "*") return true;
    }
    return false;
}

bool ResourceMatches(const Value& resources, const std::string& plural, const std::string& sub) {
    const auto full = sub.empty() ? plural : plural + "/" + sub;
    for (const auto& v : objects::Items(resources)) {
        auto s = v.As<std::string>("");
        if (s == full || s == "*/*") return true;
        if (s == "*" && sub.empty()) return true;
        if (!sub.empty() && (s == "*/" + sub || s == plural + "/*")) return true;
    }
    return false;
}

// A metav1.LabelSelector against labels: matchLabels and matchExpressions.
bool SelectorMatches(const Value& selector, const Value& labels) {
    if (!selector.IsObject() || selector.IsEmpty()) return true;
    auto label = [&](const std::string& k) -> std::optional<std::string> {
        if (labels.IsObject() && labels.HasMember(k)) return labels[k].As<std::string>("");
        return std::nullopt;
    };
    const auto ml = selector["matchLabels"];
    if (ml.IsObject()) {
        for (auto it = ml.begin(); it != ml.end(); ++it) {
            if (label(it.GetName()) != it->As<std::string>("")) return false;
        }
    }
    for (const auto& e : objects::Items(selector["matchExpressions"])) {
        auto v = label(e["key"].As<std::string>(""));
        auto op = e["operator"].As<std::string>("");
        bool in = v && Contains(e["values"], *v);
        if (op == "In" && !in) return false;
        if (op == "NotIn" && in) return false;
        if (op == "Exists" && !v) return false;
        if (op == "DoesNotExist" && v) return false;
    }
    return true;
}

}  // namespace

Webhooks::Webhooks(const userver::components::ComponentConfig& config, const userver::components::ComponentContext& context)
    : ComponentBase(config, context),
      store_(context.FindComponent<Store>()),
      watch_(context.FindComponent<Watch>()),
      http_(context.FindComponent<userver::components::HttpClient>().GetHttpClient()) {
    Refresh();
    tasks_.AsyncDetach("webhooks", [this] { Run(); });
}

Webhooks::~Webhooks() { tasks_.CancelAndWait(); }

void Webhooks::Refresh() {
    Snapshot snap;
    for (auto [plural, list] : {std::pair{"mutatingwebhookconfigurations", &snap.mutating},
                                std::pair{"validatingwebhookconfigurations", &snap.validating}}) {
        auto items = store_.List(Kind(plural), "", 0, "", "").items;
        // Configurations run in the order of their names, as upstream sorts them.
        std::sort(items.begin(), items.end(), [](const Value& a, const Value& b) { return objects::Name(a) < objects::Name(b); });
        for (const auto& cfg : items) {
            for (const auto& w : objects::Items(cfg["webhooks"])) list->push_back({w["name"].As<std::string>(""), w});
        }
    }
    hooks_.Assign(std::move(snap));
}

void Webhooks::Run() {
    auto mutating = watch_.Subscribe(Kind("mutatingwebhookconfigurations"), store_.Revision(Kind("mutatingwebhookconfigurations")));
    auto validating = watch_.Subscribe(Kind("validatingwebhookconfigurations"), store_.Revision(Kind("validatingwebhookconfigurations")));
    while (!userver::engine::current_task::ShouldCancel()) {
        bool changed = false;
        for (auto* sub : {&mutating, &validating}) {
            while (auto e = (*sub)->Next(std::chrono::milliseconds(sub == &mutating ? 200 : 200))) {
                changed = true;
            }
        }
        if (mutating->Dropped() || validating->Dropped()) {
            mutating = watch_.Subscribe(Kind("mutatingwebhookconfigurations"), store_.Revision(Kind("mutatingwebhookconfigurations")));
            validating = watch_.Subscribe(Kind("validatingwebhookconfigurations"), store_.Revision(Kind("validatingwebhookconfigurations")));
            changed = true;
        }
        if (!changed) continue;
        try {
            Refresh();
        } catch (const std::exception& e) {
            if (userver::engine::current_task::ShouldCancel()) return;
            LOG_WARNING() << "yapi-webhooks: " << e.what();
        }
    }
}

bool Webhooks::Interested(const Resource& r, const std::string& sub, const std::string& op) const {
    // Webhooks never see the configurations of webhooks.
    if (r.group == "admissionregistration.k8s.io") return false;
    auto snap = hooks_.Read();
    for (const auto* list : {&snap->mutating, &snap->validating}) {
        for (const auto& h : *list) {
            for (const auto& rule : objects::Items(h.spec["rules"])) {
                if (Contains(rule["operations"], op) && Contains(rule["apiGroups"], r.group) &&
                    Contains(rule["apiVersions"], r.version) && ResourceMatches(rule["resources"], r.plural, sub)) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool Webhooks::Matches(const Hook& hook, const Request& req) const {
    const auto& r = *req.resource;
    bool rule_ok = false;
    for (const auto& rule : objects::Items(hook.spec["rules"])) {
        auto scope = rule["scope"].As<std::string>("*");
        bool scope_ok = scope == "*" || (scope == "Cluster") == !r.namespaced;
        if (scope_ok && Contains(rule["operations"], req.operation) && Contains(rule["apiGroups"], r.group) &&
            Contains(rule["apiVersions"], r.version) && ResourceMatches(rule["resources"], r.plural, req.subresource)) {
            rule_ok = true;
        }
    }
    if (!rule_ok) return false;
    const auto ns_selector = hook.spec["namespaceSelector"];
    if (ns_selector.IsObject() && !ns_selector.IsEmpty()) {
        Value ns_labels;
        if (r.group.empty() && r.plural == "namespaces") {
            ns_labels = (req.object.IsObject() ? req.object : req.old_object)["metadata"]["labels"];
        } else if (r.namespaced) {
            auto ns = store_.Get(*Registry::Builtin().Find("", "v1", "namespaces"), "", req.ns);
            if (ns) ns_labels = (*ns)["metadata"]["labels"];
        } else {
            ns_labels = json::Value();
        }
        if ((r.namespaced || (r.group.empty() && r.plural == "namespaces")) && !SelectorMatches(ns_selector, ns_labels)) return false;
    }
    const auto obj_selector = hook.spec["objectSelector"];
    if (obj_selector.IsObject() && !obj_selector.IsEmpty()) {
        bool any = SelectorMatches(obj_selector, req.object["metadata"]["labels"]) && req.object.IsObject();
        any = any || (req.old_object.IsObject() && SelectorMatches(obj_selector, req.old_object["metadata"]["labels"]));
        if (!any) return false;
    }
    // Match conditions last: all must hold. An error goes by the failure policy.
    const auto conditions = hook.spec["matchConditions"];
    if (conditions.IsArray() && !conditions.IsEmpty()) {
        cel::Activation a;
        a.vars["object"] = req.object.IsObject() ? req.object : Value();
        a.vars["oldObject"] = req.old_object.IsObject() ? req.old_object : Value();
        ValueBuilder rq(json::Type::kObject);
        rq["operation"] = req.operation;
        rq["name"] = req.name;
        rq["namespace"] = req.ns;
        rq["subResource"] = req.subresource;
        rq["resource"]["group"] = r.group;
        rq["resource"]["version"] = r.version;
        rq["resource"]["resource"] = r.plural;
        rq["kind"]["group"] = r.group;
        rq["kind"]["version"] = r.version;
        rq["kind"]["kind"] = r.kind;
        if (req.user) rq["userInfo"] = req.user->ToJson();
        rq["dryRun"] = req.dry_run;
        a.vars["request"] = rq.ExtractValue();
        for (const auto& cond : conditions) {
            try {
                auto v = cel::Program::Parse(cond["expression"].As<std::string>("")).Eval(a);
                if (!v.IsBool() || !v.As<bool>()) return false;
            } catch (const std::exception& e) {
                if (hook.spec["failurePolicy"].As<std::string>("Fail") == "Ignore") return false;
                throw ApiError(500, "InternalError", "Internal error occurred: failed calling webhook \"" + hook.name +
                                                         "\": match condition " + cond["name"].As<std::string>("") + " failed: " + e.what());
            }
        }
    }
    return true;
}

namespace {

cel::Env ConditionEnv() {
    cel::Env env;
    for (const char* v : {"object", "oldObject", "request", "authorizer", "params"}) env.vars[v] = cel::Type::Dyn();
    return env;
}

}  // namespace

void Webhooks::ValidateConfig(const Resource& r, const Value& config) {
    std::size_t i = 0;
    for (const auto& hook : objects::Items(config["webhooks"])) {
        std::size_t j = 0;
        for (const auto& cond : objects::Items(hook["matchConditions"])) {
            const auto expr = cond["expression"].As<std::string>("");
            const auto field = "webhooks[" + std::to_string(i) + "].matchConditions[" + std::to_string(j) + "].expression";
            std::string error;
            try {
                auto errs = cel::Program::Parse(expr).Check(ConditionEnv());
                if (!errs.empty()) error = errs[0];
            } catch (const cel::CompileError& e) {
                error = e.what();
            }
            if (!error.empty()) {
                throw Invalid(r.kind, r.group, objects::Name(config),
                              field + ": Invalid value: \"" + expr + "\": compilation failed: " + error);
            }
            ++j;
        }
        ++i;
    }
}

Value Webhooks::Call(const Value& cc, const std::string& name, const Value& body, int timeout_seconds) const {
    std::string url = cc["url"].As<std::string>("");
    std::optional<userver::clients::http::ConnectTo> connect_to;
    if (url.empty()) {
        const auto svc = cc["service"];
        auto svc_ns = svc["namespace"].As<std::string>("");
        auto svc_name = svc["name"].As<std::string>("");
        auto port = svc["port"].As<int>(443);
        auto host = svc_name + "." + svc_ns + ".svc";
        url = "https://" + host + ":" + std::to_string(port) + svc["path"].As<std::string>("");
        // The cluster's DNS is not the control plane's: the name is for TLS,
        // the connection goes to an endpoint of the service.
        const auto to = ServiceDestination(store_, svc_ns, svc_name, port);
        connect_to.emplace(host + ":" + std::to_string(port) + ":" + to.ip + ":" + std::to_string(to.port));
    }
    auto request = http_.CreateRequest();
    request.post(url, json::ToString(body))
        .headers({{"Content-Type", "application/json"}, {"Accept", "application/json"}})
        .timeout(std::chrono::milliseconds(timeout_seconds * 1000))
        .retry(1, false);
    auto ca = cc["caBundle"].As<std::string>("");
    if (!ca.empty()) request.ca(userver::crypto::Certificate::LoadFromString(userver::crypto::base64::Base64Decode(ca)));
    if (connect_to) request.connect_to(*connect_to);
    std::shared_ptr<userver::clients::http::Response> response;
    try {
        response = request.perform();
    } catch (const userver::clients::http::TimeoutException&) {
        // Said the way Go's client says it: callers look for "deadline".
        throw std::runtime_error("Post \"" + url + "?timeout=" + std::to_string(timeout_seconds) + "s\": context deadline exceeded");
    } catch (const std::exception& e) {
        throw std::runtime_error("Post \"" + url + "?timeout=" + std::to_string(timeout_seconds) + "s\": " + e.what());
    }
    if (response->status_code() != userver::clients::http::Status::kOk) {
        throw std::runtime_error("the server responded with status " + std::to_string(static_cast<int>(response->status_code())));
    }
    return json::FromString(response->body());
}

Value Webhooks::Review(const Hook& hook, const Request& req, const Value& object) const {
    const auto& r = *req.resource;
    ValueBuilder rq(json::Type::kObject);
    rq["uid"] = userver::utils::generators::GenerateUuid();
    ValueBuilder kind(json::Type::kObject);
    kind["group"] = r.group;
    kind["version"] = r.version;
    kind["kind"] = r.kind;
    ValueBuilder resource(json::Type::kObject);
    resource["group"] = r.group;
    resource["version"] = r.version;
    resource["resource"] = r.plural;
    auto kind_v = kind.ExtractValue();
    auto resource_v = resource.ExtractValue();
    rq["kind"] = kind_v;
    rq["resource"] = resource_v;
    rq["requestKind"] = kind_v;
    rq["requestResource"] = resource_v;
    if (!req.subresource.empty()) {
        rq["subResource"] = req.subresource;
        rq["requestSubResource"] = req.subresource;
    }
    rq["name"] = req.name;
    if (!req.ns.empty()) rq["namespace"] = req.ns;
    rq["operation"] = req.operation;
    if (req.user) rq["userInfo"] = req.user->ToJson();
    rq["object"] = object.IsObject() ? object : Value();
    rq["oldObject"] = req.old_object.IsObject() ? req.old_object : Value();
    rq["dryRun"] = req.dry_run;
    if (req.options.IsObject()) rq["options"] = req.options;
    ValueBuilder review(json::Type::kObject);
    review["apiVersion"] = "admission.k8s.io/v1";
    review["kind"] = "AdmissionReview";
    review["request"] = rq;
    const auto timeout = hook.spec["timeoutSeconds"].As<int>(10);
    try {
        auto answer = Call(hook.spec["clientConfig"], hook.name, review.ExtractValue(), timeout);
        return answer["response"];
    } catch (const ApiError&) {
        throw;
    } catch (const std::exception& e) {
        if (hook.spec["failurePolicy"].As<std::string>("Fail") == "Ignore") return Value();
        throw ApiError(500, "InternalError",
                       "Internal error occurred: failed calling webhook \"" + hook.name + "\": failed to call webhook: " + e.what());
    }
}

namespace {

void Refuse(const std::string& hook, const Value& response) {
    // As upstream's ToStatusErr: never under 400, the reason when there is
    // no message.
    const auto status = response["status"];
    auto code = status["code"].As<int>(0);
    if (code < 400) code = 400;
    const auto denied = "admission webhook \"" + hook + "\" denied the request";
    auto message = status["message"].As<std::string>("");
    auto reason = status["reason"].As<std::string>("");
    throw ApiError(code, reason, !message.empty() ? denied + ": " + message
                                 : !reason.empty() ? denied + ": " + reason
                                                   : denied + " without explanation");
}

}  // namespace

Value Webhooks::Mutate(const Request& req) const {
    if (!Interested(*req.resource, req.subresource, req.operation)) return req.object;
    auto snap = hooks_.Read();
    Value object = req.object;
    for (const auto& hook : snap->mutating) {
        Request current = req;
        current.object = object;
        if (!Matches(hook, current)) continue;
        auto response = Review(hook, current, object);
        if (response.IsMissing() || response.IsNull()) continue;
        if (!response["allowed"].As<bool>(false)) Refuse(hook.name, response);
        auto patch = response["patch"].As<std::string>("");
        if (!patch.empty() && object.IsObject()) {
            object = patch::Json(object, json::FromString(userver::crypto::base64::Base64Decode(patch)));
        }
    }
    return object;
}

void Webhooks::Validate(const Request& req) const {
    if (!Interested(*req.resource, req.subresource, req.operation)) return;
    auto snap = hooks_.Read();
    for (const auto& hook : snap->validating) {
        if (!Matches(hook, req)) continue;
        auto response = Review(hook, req, req.object);
        if (response.IsMissing() || response.IsNull()) continue;
        if (!response["allowed"].As<bool>(false)) Refuse(hook.name, response);
    }
}

std::vector<Value> Webhooks::Convert(const CustomInfo& info, const std::vector<Value>& objects, const std::string& desired) const {
    ValueBuilder rq(json::Type::kObject);
    rq["uid"] = userver::utils::generators::GenerateUuid();
    rq["desiredAPIVersion"] = desired;
    rq["objects"] = objects;
    ValueBuilder review(json::Type::kObject);
    review["apiVersion"] = "apiextensions.k8s.io/v1";
    review["kind"] = "ConversionReview";
    review["request"] = rq;
    Value answer;
    try {
        answer = Call(info.webhook, info.crd, review.ExtractValue(), 30);
    } catch (const std::exception& e) {
        throw ApiError(500, "InternalError", std::string("conversion webhook for ") + info.crd + " failed: " + e.what());
    }
    const auto response = answer["response"];
    if (response["result"]["status"].As<std::string>("Success") != "Success") {
        throw ApiError(500, "InternalError", "conversion webhook for " + info.crd + " failed: " + response["result"]["message"].As<std::string>(""));
    }
    std::vector<Value> out;
    for (const auto& o : objects::Items(response["convertedObjects"])) out.push_back(o);
    if (out.size() != objects.size()) {
        throw ApiError(500, "InternalError", "conversion webhook for " + info.crd + " returned " + std::to_string(out.size()) + " objects, expected " + std::to_string(objects.size()));
    }
    return out;
}

}  // namespace yapi
