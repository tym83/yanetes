#include <registry.hpp>

#include <algorithm>

namespace yapi {

namespace {

Resource R(
    std::string group,
    std::string version,
    std::string plural,
    std::string singular,
    std::string kind,
    bool namespaced,
    bool has_status,
    std::vector<std::string> short_names = {},
    std::vector<std::string> categories = {},
    std::vector<std::string> verbs = {}
) {
    return Resource{
        std::move(group),
        std::move(version),
        std::move(plural),
        std::move(singular),
        std::move(kind),
        namespaced,
        has_status,
        std::move(short_names),
        std::move(categories),
        std::move(verbs),
        nullptr};
}

std::vector<Resource> BuiltinResources() {
    constexpr bool kNs = true, kCluster = false, kStatus = true, kPlain = false;
    const std::vector<std::string> kCreate = {"create"};
    return {
        R("", "v1", "namespaces", "namespace", "Namespace", kCluster, kStatus, {"ns"}),
        R("", "v1", "nodes", "node", "Node", kCluster, kStatus, {"no"}),
        R("", "v1", "pods", "pod", "Pod", kNs, kStatus, {"po"}, {"all"}),
        R("", "v1", "services", "service", "Service", kNs, kStatus, {"svc"}, {"all"}),
        R("", "v1", "endpoints", "endpoints", "Endpoints", kNs, kPlain, {"ep"}),
        R("", "v1", "configmaps", "configmap", "ConfigMap", kNs, kPlain, {"cm"}),
        R("", "v1", "secrets", "secret", "Secret", kNs, kPlain),
        R("", "v1", "serviceaccounts", "serviceaccount", "ServiceAccount", kNs, kPlain, {"sa"}),
        R("", "v1", "events", "event", "Event", kNs, kPlain, {"ev"}),
        R("", "v1", "limitranges", "limitrange", "LimitRange", kNs, kPlain, {"limits"}),
        R("", "v1", "resourcequotas", "resourcequota", "ResourceQuota", kNs, kStatus, {"quota"}),
        R("", "v1", "persistentvolumes", "persistentvolume", "PersistentVolume", kCluster, kStatus, {"pv"}),
        R("", "v1", "persistentvolumeclaims", "persistentvolumeclaim", "PersistentVolumeClaim", kNs, kStatus, {"pvc"}),
        R("", "v1", "podtemplates", "podtemplate", "PodTemplate", kNs, kPlain),
        R("", "v1", "replicationcontrollers", "replicationcontroller", "ReplicationController", kNs, kStatus, {"rc"}, {"all"}),
        R("apps", "v1", "deployments", "deployment", "Deployment", kNs, kStatus, {"deploy"}, {"all"}),
        R("apps", "v1", "replicasets", "replicaset", "ReplicaSet", kNs, kStatus, {"rs"}, {"all"}),
        R("apps", "v1", "daemonsets", "daemonset", "DaemonSet", kNs, kStatus, {"ds"}, {"all"}),
        R("apps", "v1", "statefulsets", "statefulset", "StatefulSet", kNs, kStatus, {"sts"}, {"all"}),
        R("apps", "v1", "controllerrevisions", "controllerrevision", "ControllerRevision", kNs, kPlain),
        R("batch", "v1", "jobs", "job", "Job", kNs, kStatus, {}, {"all"}),
        R("batch", "v1", "cronjobs", "cronjob", "CronJob", kNs, kStatus, {"cj"}, {"all"}),
        R("coordination.k8s.io", "v1", "leases", "lease", "Lease", kNs, kPlain),
        R("networking.k8s.io", "v1", "ipaddresses", "ipaddress", "IPAddress", kCluster, kPlain, {"ip"}),
        R("networking.k8s.io", "v1", "servicecidrs", "servicecidr", "ServiceCIDR", kCluster, kStatus),
        R("authentication.k8s.io", "v1", "tokenreviews", "tokenreview", "TokenReview", kCluster, kPlain, {}, {}, kCreate),
        R("authentication.k8s.io", "v1", "selfsubjectreviews", "selfsubjectreview", "SelfSubjectReview", kCluster, kPlain, {}, {}, kCreate),
        R("authorization.k8s.io", "v1", "subjectaccessreviews", "subjectaccessreview", "SubjectAccessReview", kCluster, kPlain, {}, {}, kCreate),
        R("authorization.k8s.io", "v1", "selfsubjectaccessreviews", "selfsubjectaccessreview", "SelfSubjectAccessReview", kCluster, kPlain, {}, {}, kCreate),
        R("authorization.k8s.io", "v1", "localsubjectaccessreviews", "localsubjectaccessreview", "LocalSubjectAccessReview", kNs, kPlain, {}, {}, kCreate),
        R("authorization.k8s.io", "v1", "selfsubjectrulesreviews", "selfsubjectrulesreview", "SelfSubjectRulesReview", kCluster, kPlain, {}, {}, kCreate),
        R("discovery.k8s.io", "v1", "endpointslices", "endpointslice", "EndpointSlice", kNs, kPlain),
        R("events.k8s.io", "v1", "events", "event", "Event", kNs, kPlain, {"ev"}),
        R("node.k8s.io", "v1", "runtimeclasses", "runtimeclass", "RuntimeClass", kCluster, kPlain),
        R("storage.k8s.io", "v1", "csidrivers", "csidriver", "CSIDriver", kCluster, kPlain),
        R("storage.k8s.io", "v1", "csinodes", "csinode", "CSINode", kCluster, kPlain),
        R("storage.k8s.io", "v1", "storageclasses", "storageclass", "StorageClass", kCluster, kPlain, {"sc"}),
        R("storage.k8s.io", "v1", "volumeattachments", "volumeattachment", "VolumeAttachment", kCluster, kStatus),
        R("scheduling.k8s.io", "v1", "priorityclasses", "priorityclass", "PriorityClass", kCluster, kPlain, {"pc"}),
        R("rbac.authorization.k8s.io", "v1", "roles", "role", "Role", kNs, kPlain),
        R("rbac.authorization.k8s.io", "v1", "rolebindings", "rolebinding", "RoleBinding", kNs, kPlain),
        R("rbac.authorization.k8s.io", "v1", "clusterroles", "clusterrole", "ClusterRole", kCluster, kPlain),
        R("rbac.authorization.k8s.io", "v1", "clusterrolebindings", "clusterrolebinding", "ClusterRoleBinding", kCluster, kPlain),
        R("certificates.k8s.io", "v1", "certificatesigningrequests", "certificatesigningrequest", "CertificateSigningRequest", kCluster, kStatus, {"csr"}),
        R("networking.k8s.io", "v1", "networkpolicies", "networkpolicy", "NetworkPolicy", kNs, kPlain, {"netpol"}),
        R("networking.k8s.io", "v1", "ingresses", "ingress", "Ingress", kNs, kStatus, {"ing"}),
        R("networking.k8s.io", "v1", "ingressclasses", "ingressclass", "IngressClass", kCluster, kPlain),
        R("policy", "v1", "poddisruptionbudgets", "poddisruptionbudget", "PodDisruptionBudget", kNs, kStatus, {"pdb"}),
        R("autoscaling", "v2", "horizontalpodautoscalers", "horizontalpodautoscaler", "HorizontalPodAutoscaler", kNs, kStatus, {"hpa"}, {"all"}),
        R("apiextensions.k8s.io", "v1", "customresourcedefinitions", "customresourcedefinition", "CustomResourceDefinition", kCluster, kStatus, {"crd", "crds"}),
        R("admissionregistration.k8s.io", "v1", "mutatingwebhookconfigurations", "mutatingwebhookconfiguration", "MutatingWebhookConfiguration", kCluster, kPlain),
        R("admissionregistration.k8s.io", "v1", "validatingwebhookconfigurations", "validatingwebhookconfiguration", "ValidatingWebhookConfiguration", kCluster, kPlain),
        R("admissionregistration.k8s.io", "v1", "validatingadmissionpolicies", "validatingadmissionpolicy", "ValidatingAdmissionPolicy", kCluster, kStatus),
        R("admissionregistration.k8s.io", "v1", "validatingadmissionpolicybindings", "validatingadmissionpolicybinding", "ValidatingAdmissionPolicyBinding", kCluster, kPlain),
        R("admissionregistration.k8s.io", "v1", "mutatingadmissionpolicies", "mutatingadmissionpolicy", "MutatingAdmissionPolicy", kCluster, kPlain),
        R("admissionregistration.k8s.io", "v1", "mutatingadmissionpolicybindings", "mutatingadmissionpolicybinding", "MutatingAdmissionPolicyBinding", kCluster, kPlain),
        R("apiregistration.k8s.io", "v1", "apiservices", "apiservice", "APIService", kCluster, kStatus),
        R("certificates.k8s.io", "v1", "clustertrustbundles", "clustertrustbundle", "ClusterTrustBundle", kCluster, kPlain),
        R("certificates.k8s.io", "v1", "podcertificaterequests", "podcertificaterequest", "PodCertificateRequest", kNs, kStatus),
        R("resource.k8s.io", "v1", "deviceclasses", "deviceclass", "DeviceClass", kCluster, kPlain),
        R("resource.k8s.io", "v1", "devicetaintrules", "devicetaintrule", "DeviceTaintRule", kCluster, kStatus),
        R("resource.k8s.io", "v1", "resourceclaims", "resourceclaim", "ResourceClaim", kNs, kStatus),
        R("resource.k8s.io", "v1", "resourceclaimtemplates", "resourceclaimtemplate", "ResourceClaimTemplate", kNs, kPlain),
        R("resource.k8s.io", "v1", "resourceslices", "resourceslice", "ResourceSlice", kCluster, kPlain),
        R("storage.k8s.io", "v1", "csistoragecapacities", "csistoragecapacity", "CSIStorageCapacity", kNs, kPlain),
        R("storage.k8s.io", "v1", "volumeattributesclasses", "volumeattributesclass", "VolumeAttributesClass", kCluster, kPlain, {"vac"}),
        R("storagemigration.k8s.io", "v1", "storageversionmigrations", "storageversionmigration", "StorageVersionMigration", kCluster, kStatus),
        R("flowcontrol.apiserver.k8s.io", "v1", "flowschemas", "flowschema", "FlowSchema", kCluster, kStatus),
        R("flowcontrol.apiserver.k8s.io", "v1", "prioritylevelconfigurations", "prioritylevelconfiguration", "PriorityLevelConfiguration", kCluster, kStatus),
    };
}

}  // namespace

const Registry& Registry::Builtin() {
    static const Registry registry(BuiltinResources());
    return registry;
}

const Resource* Registry::Find(const std::string& group, const std::string& version, const std::string& plural) const {
    for (const auto& r : resources_) {
        if (r.group == group && r.version == version && r.plural == plural) return &r;
    }
    auto custom = custom_.Read();
    for (const auto& [crd, list] : *custom) {
        for (const auto* r : list) {
            if (r->group == group && r->version == version && r->plural == plural) return r;
        }
    }
    return nullptr;
}

const Resource* Registry::FindByKind(const std::string& group, const std::string& kind) const {
    for (const auto& r : resources_) {
        if (r.group == group && r.kind == kind) return &r;
    }
    auto custom = custom_.Read();
    for (const auto& [crd, list] : *custom) {
        for (const auto* r : list) {
            if (r->group == group && r->kind == kind && r->version == r->custom->storage_version) return r;
        }
    }
    return nullptr;
}

std::vector<const Resource*> Registry::All() const {
    std::vector<const Resource*> out;
    for (const auto& r : resources_) out.push_back(&r);
    auto custom = custom_.Read();
    for (const auto& [crd, list] : *custom) out.insert(out.end(), list.begin(), list.end());
    return out;
}

std::vector<std::string> Registry::GroupVersions() const {
    std::vector<std::string> out;
    for (const auto* r : All()) {
        if (r->group.empty()) continue;
        auto gv = r->GroupVersion();
        if (std::find(out.begin(), out.end(), gv) == out.end()) out.push_back(gv);
    }
    return out;
}

std::string Registry::PreferredVersion(const std::string& group) const {
    std::string first;
    for (const auto* r : All()) {
        if (r->group != group) continue;
        if (r->custom && r->version == r->custom->storage_version) return r->version;
        if (first.empty()) first = r->version;
    }
    return first;
}

void Registry::SetCustom(const std::string& crd, std::vector<Resource> resources) const {
    std::vector<const Resource*> stable;
    {
        std::unique_lock lock(storage_mutex_);
        for (auto& r : resources) {
            storage_.push_back(std::move(r));
            stable.push_back(&storage_.back());
        }
    }
    auto custom = custom_.StartWrite();
    if (stable.empty()) {
        custom->erase(crd);
    } else {
        (*custom)[crd] = std::move(stable);
    }
    custom.Commit();
}

}  // namespace yapi
