#include <policies.hpp>

#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/engine/task/cancel.hpp>
#include <userver/formats/json/serialize.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/formats/serialize/common_containers.hpp>
#include <userver/logging/log.hpp>

#include <cel.hpp>
#include <crds.hpp>
#include <objects.hpp>
#include <patch.hpp>
#include <status.hpp>
#include <store.hpp>

namespace yapi {

namespace json = userver::formats::json;
using json::Value;
using json::ValueBuilder;

namespace {

const Resource& Admission(const char* plural) { return *Registry::Builtin().Find("admissionregistration.k8s.io", "v1", plural); }

bool Contains(const Value& list, const std::string& want) {
    for (const auto& v : objects::Items(list)) {
        auto s = v.As<std::string>("");
        if (s == want || s == "*") return true;
    }
    return false;
}

bool RuleMatches(const Value& rule, const Resource& r, const std::string& sub, const std::string& op, const std::string& name) {
    if (!Contains(rule["operations"], op) || !Contains(rule["apiGroups"], r.group) || !Contains(rule["apiVersions"], r.version)) return false;
    const auto full = sub.empty() ? r.plural : r.plural + "/" + sub;
    bool res = false;
    for (const auto& v : objects::Items(rule["resources"])) {
        auto s = v.As<std::string>("");
        res = res || s == full || s == "*/*" || (s == "*" && sub.empty()) || (!sub.empty() && (s == "*/" + sub || s == r.plural + "/*"));
    }
    if (!res) return false;
    auto scope = rule["scope"].As<std::string>("*");
    if (scope != "*" && (scope == "Cluster") == r.namespaced) return false;
    if (rule["resourceNames"].IsArray() && !rule["resourceNames"].IsEmpty() && !Contains(rule["resourceNames"], name)) return false;
    return true;
}

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
        if ((op == "In" && !in) || (op == "NotIn" && in) || (op == "Exists" && !v) || (op == "DoesNotExist" && v)) return false;
    }
    return true;
}

int CodeOf(const std::string& reason) {
    if (reason == "Forbidden") return 403;
    if (reason == "Unauthorized") return 401;
    if (reason == "RequestEntityTooLarge") return 413;
    return 422;
}

// Server-side apply, near enough for a policy's apply configuration: maps
// merge, everything else is replaced.
Value Merge(const Value& base, const Value& overlay) {
    if (!base.IsObject() || !overlay.IsObject()) return overlay;
    ValueBuilder out(base);
    for (auto it = overlay.begin(); it != overlay.end(); ++it) {
        out[it.GetName()] = base.HasMember(it.GetName()) ? Merge(base[it.GetName()], *it) : *it;
    }
    return out.ExtractValue();
}

}  // namespace

Policies::Policies(const userver::components::ComponentConfig& config, const userver::components::ComponentContext& context)
    : ComponentBase(config, context),
      store_(context.FindComponent<Store>()),
      crds_(context.FindComponent<Crds>()) {
    Refresh();
    // Other replicas write too; look again every few seconds.
    tasks_.AsyncDetach("policies", [this] {
        while (!userver::engine::current_task::ShouldCancel()) {
            userver::engine::InterruptibleSleepFor(std::chrono::seconds(2));
            try {
                Refresh();
            } catch (const std::exception& e) {
                if (userver::engine::current_task::ShouldCancel()) return;
                LOG_WARNING() << "yapi-policies: " << e.what();
            }
        }
    });
}

Policies::~Policies() { tasks_.CancelAndWait(); }

void Policies::Refresh() {
    Snapshot snap;
    snap.validating = store_.List(Admission("validatingadmissionpolicies"), "", 0, "", "").items;
    snap.validating_bindings = store_.List(Admission("validatingadmissionpolicybindings"), "", 0, "", "").items;
    snap.mutating = store_.List(Admission("mutatingadmissionpolicies"), "", 0, "", "").items;
    snap.mutating_bindings = store_.List(Admission("mutatingadmissionpolicybindings"), "", 0, "", "").items;
    for (const auto& p : snap.validating) {
        if (p["status"]["observedGeneration"].As<std::int64_t>(0) < p["metadata"]["generation"].As<std::int64_t>(1)) TypeCheck(p);
    }
    snapshot_.Assign(std::move(snap));
}

void Policies::TypeCheck(const Value& policy) {
    // The kinds the policy matches decide what "object" is.
    std::vector<const Resource*> kinds;
    for (const auto& rule : objects::Items(policy["spec"]["matchConstraints"]["resourceRules"])) {
        for (const auto& g : objects::Items(rule["apiGroups"])) {
            for (const auto& v : objects::Items(rule["apiVersions"])) {
                for (const auto& res : objects::Items(rule["resources"])) {
                    auto plural = res.As<std::string>("");
                    if (plural.find('*') != std::string::npos || plural.find('/') != std::string::npos) continue;
                    if (const auto* r = Registry::Builtin().Find(g.As<std::string>(""), v.As<std::string>(""), plural)) kinds.push_back(r);
                }
            }
        }
    }
    ValueBuilder warnings(json::Type::kArray);
    for (const auto* r : kinds) {
        auto schema = crds_.SchemaFor(*r);
        cel::Env env;
        env.components = crds_.Components();
        auto object = schema ? cel::FromSchema(crds_.RootSchema(*r), env.components, "Object") : cel::Type::Dyn();
        env.vars["object"] = object;
        env.vars["oldObject"] = object;
        for (const char* v : {"request", "params", "namespaceObject", "authorizer"}) env.vars[v] = cel::Type::Dyn();
        env.vars["variables"] = cel::Type::Map(cel::Type::Dyn());
        const auto prefix = (r->group.empty() ? r->version : r->group + "/" + r->version) + ", Kind=" + r->kind + ": ";
        std::size_t i = 0;
        for (const auto& val : objects::Items(policy["spec"]["validations"])) {
            for (const char* field : {"expression", "messageExpression"}) {
                if (!val.HasMember(field)) continue;
                std::vector<std::string> errs;
                try {
                    errs = cel::Program::Parse(val[field].As<std::string>("")).Check(env);
                } catch (const cel::CompileError& e) {
                    errs = {e.what()};
                }
                for (const auto& e : errs) {
                    ValueBuilder w(json::Type::kObject);
                    w["fieldRef"] = "spec.validations[" + std::to_string(i) + "]." + field;
                    w["warning"] = prefix + e;
                    warnings.PushBack(w.ExtractValue());
                }
            }
            ++i;
        }
    }
    const auto generation = policy["metadata"]["generation"].As<std::int64_t>(1);
    auto checked = warnings.ExtractValue();
    store_.Write(Admission("validatingadmissionpolicies"), "", objects::Name(policy), [&](const std::optional<Value>& current) {
        if (!current) return Decision::Keep();
        ValueBuilder b(*current);
        b["status"]["observedGeneration"] = generation;
        b["status"]["typeChecking"] = ValueBuilder(json::Type::kObject);
        if (!checked.IsEmpty()) b["status"]["typeChecking"]["expressionWarnings"] = checked;
        return Decision::Put(b.ExtractValue());
    });
}

bool Policies::Interested(const Resource& r, const std::string& op) const {
    if (r.group == "admissionregistration.k8s.io") return false;
    auto snap = snapshot_.Read();
    for (const auto* list : {&snap->validating, &snap->mutating}) {
        for (const auto& p : *list) {
            for (const auto& rule : objects::Items(p["spec"]["matchConstraints"]["resourceRules"])) {
                if (Contains(rule["operations"], op) && Contains(rule["apiGroups"], r.group) && Contains(rule["apiVersions"], r.version)) return true;
            }
        }
    }
    return false;
}

bool Policies::Matches(const Value& policy, const Value& binding, const Webhooks::Request& req) const {
    const auto& r = *req.resource;
    const auto object = req.object.IsObject() ? req.object : req.old_object;
    auto constraints_match = [&](const Value& mr) {
        if (!mr.IsObject()) return true;
        bool any = !mr["resourceRules"].IsArray();
        for (const auto& rule : objects::Items(mr["resourceRules"])) any = any || RuleMatches(rule, r, req.subresource, req.operation, req.name);
        for (const auto& rule : objects::Items(mr["excludeResourceRules"])) {
            if (RuleMatches(rule, r, req.subresource, req.operation, req.name)) return false;
        }
        if (!any) return false;
        if (mr["namespaceSelector"].IsObject() && !mr["namespaceSelector"].IsEmpty()) {
            if (!r.namespaced && !(r.group.empty() && r.plural == "namespaces")) return true;
            Value labels;
            if (r.group.empty() && r.plural == "namespaces") {
                labels = object["metadata"]["labels"];
            } else if (auto ns = store_.Get(*Registry::Builtin().Find("", "v1", "namespaces"), "", req.ns)) {
                labels = (*ns)["metadata"]["labels"];
            }
            if (!SelectorMatches(mr["namespaceSelector"], labels)) return false;
        }
        return SelectorMatches(mr["objectSelector"], object["metadata"]["labels"]);
    };
    return constraints_match(policy["spec"]["matchConstraints"]) &&
           (!binding["spec"].HasMember("matchResources") || constraints_match(binding["spec"]["matchResources"]));
}

namespace {

// The CEL variables of one admission request.
cel::Activation Variables(Store& store, const Webhooks::Request& req, const Value& params) {
    cel::Activation a;
    a.vars["object"] = req.object.IsObject() ? req.object : Value();
    a.vars["oldObject"] = req.old_object.IsObject() ? req.old_object : Value();
    a.vars["params"] = params;
    ValueBuilder rq(json::Type::kObject);
    rq["operation"] = req.operation;
    rq["name"] = req.name;
    rq["namespace"] = req.ns;
    rq["kind"]["group"] = req.resource->group;
    rq["kind"]["version"] = req.resource->version;
    rq["kind"]["kind"] = req.resource->kind;
    rq["resource"]["group"] = req.resource->group;
    rq["resource"]["version"] = req.resource->version;
    rq["resource"]["resource"] = req.resource->plural;
    if (req.user) rq["userInfo"] = req.user->ToJson();
    a.vars["request"] = rq.ExtractValue();
    a.vars["namespaceObject"] = Value();
    if (!req.ns.empty()) {
        if (auto ns = store.Get(*Registry::Builtin().Find("", "v1", "namespaces"), "", req.ns)) a.vars["namespaceObject"] = *ns;
    }
    return a;
}

// Variables first, in order: later ones and the expressions may use them.
void AddVariables(const Value& policy, cel::Activation& a) {
    ValueBuilder vars(json::Type::kObject);
    for (const auto& v : objects::Items(policy["spec"]["variables"])) {
        a.vars["variables"] = vars.ExtractValue();
        vars = ValueBuilder(a.vars["variables"]);
        try {
            vars[v["name"].As<std::string>("")] = cel::Program::Parse(v["expression"].As<std::string>("")).Eval(a);
        } catch (const std::exception&) {
            // An unusable variable fails the expressions that use it.
        }
    }
    a.vars["variables"] = vars.ExtractValue();
}

bool ConditionsHold(const Value& policy, const cel::Activation& a) {
    for (const auto& c : objects::Items(policy["spec"]["matchConditions"])) {
        auto v = cel::Program::Parse(c["expression"].As<std::string>("")).Eval(a);
        if (!v.IsBool() || !v.As<bool>()) return false;
    }
    return true;
}

}  // namespace

std::vector<std::string> Policies::Validate(const Webhooks::Request& req) const {
    std::vector<std::string> warnings;
    if (!Interested(*req.resource, req.operation)) return warnings;
    auto snap = snapshot_.Read();
    for (const auto& binding : snap->validating_bindings) {
        const auto policy_name = binding["spec"]["policyName"].As<std::string>("");
        const Value* policy = nullptr;
        for (const auto& p : snap->validating) {
            if (objects::Name(p) == policy_name) policy = &p;
        }
        if (!policy || !Matches(*policy, binding, req)) continue;
        const auto binding_name = objects::Name(binding);
        const auto who = "ValidatingAdmissionPolicy '" + policy_name + "' with binding '" + binding_name + "'";
        const bool fail_closed = (*policy)["spec"]["failurePolicy"].As<std::string>("Fail") == "Fail";
        const auto actions = binding["spec"]["validationActions"];
        auto act = [&](const std::string& message, const std::string& reason) {
            if (Contains(actions, "Deny") || !actions.IsArray()) {
                throw ApiError(CodeOf(reason), reason, who + " denied request: " + message);
            }
            if (Contains(actions, "Warn")) warnings.push_back("Validation failed for " + who + ": " + message);
        };
        try {
            auto a = Variables(store_, req, Value());
            if (!ConditionsHold(*policy, a)) continue;
            AddVariables(*policy, a);
            for (const auto& v : objects::Items((*policy)["spec"]["validations"])) {
                const auto expr = v["expression"].As<std::string>("");
                Value result;
                try {
                    result = cel::Program::Parse(expr).Eval(a);
                } catch (const std::exception& e) {
                    if (fail_closed) act("expression '" + expr + "' resulted in error: " + e.what(), "Invalid");
                    continue;
                }
                if (result.IsBool() && result.As<bool>()) continue;
                std::string message = v["message"].As<std::string>("");
                if (v.HasMember("messageExpression")) {
                    try {
                        auto m = cel::Program::Parse(v["messageExpression"].As<std::string>("")).Eval(a);
                        if (m.IsString() && !m.As<std::string>().empty()) message = m.As<std::string>();
                    } catch (const std::exception&) {
                    }
                }
                if (message.empty()) message = "failed expression: " + expr;
                act(message, v["reason"].As<std::string>("Invalid"));
            }
        } catch (const ApiError&) {
            throw;
        } catch (const std::exception& e) {
            if (fail_closed) throw ApiError(422, "Invalid", who + " denied request: " + e.what());
        }
    }
    return warnings;
}

Value Policies::Mutate(const Webhooks::Request& req) const {
    if (!Interested(*req.resource, req.operation)) return req.object;
    auto snap = snapshot_.Read();
    Value object = req.object;
    for (const auto& binding : snap->mutating_bindings) {
        const auto policy_name = binding["spec"]["policyName"].As<std::string>("");
        const Value* policy = nullptr;
        for (const auto& p : snap->mutating) {
            if (objects::Name(p) == policy_name) policy = &p;
        }
        Webhooks::Request current = req;
        current.object = object;
        if (!policy || !Matches(*policy, binding, current)) continue;
        const bool fail_closed = (*policy)["spec"]["failurePolicy"].As<std::string>("Fail") == "Fail";
        try {
            auto a = Variables(store_, current, Value());
            if (!ConditionsHold(*policy, a)) continue;
            AddVariables(*policy, a);
            for (const auto& m : objects::Items((*policy)["spec"]["mutations"])) {
                const auto type = m["patchType"].As<std::string>("");
                if (type == "ApplyConfiguration") {
                    auto applied = cel::Program::Parse(m["applyConfiguration"]["expression"].As<std::string>("")).Eval(a);
                    object = Merge(object, applied);
                } else if (type == "JSONPatch") {
                    auto ops = cel::Program::Parse(m["jsonPatch"]["expression"].As<std::string>("")).Eval(a);
                    object = patch::Json(object, ops);
                }
                a.vars["object"] = object;
            }
        } catch (const ApiError&) {
            throw;
        } catch (const std::exception& e) {
            if (fail_closed) {
                throw ApiError(422, "Invalid", "MutatingAdmissionPolicy '" + policy_name + "' with binding '" + objects::Name(binding) +
                                                   "' denied request: " + e.what());
            }
        }
    }
    return object;
}

}  // namespace yapi
