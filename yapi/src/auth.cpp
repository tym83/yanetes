#include <auth.hpp>

#include <fstream>
#include <sstream>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/crypto/base64.hpp>
#include <userver/crypto/private_key.hpp>
#include <userver/crypto/public_key.hpp>
#include <userver/crypto/signers.hpp>
#include <userver/crypto/verifiers.hpp>
#include <userver/formats/json/serialize.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/formats/serialize/common_containers.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/uuid4.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

#include <objects.hpp>
#include <registry.hpp>
#include <status.hpp>
#include <store.hpp>

namespace yapi {

namespace json = userver::formats::json;
namespace crypto = userver::crypto;
using json::Value;
using json::ValueBuilder;

namespace {

constexpr auto kCacheFor = std::chrono::seconds(10);
constexpr std::size_t kCacheMax = 100'000;

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string B64(std::string_view raw) { return crypto::base64::Base64UrlEncode(raw, crypto::base64::Pad::kWithout); }

std::string Bignum(EVP_PKEY* key, const char* param) {
    BIGNUM* bn = nullptr;
    if (!EVP_PKEY_get_bn_param(key, param, &bn)) throw std::runtime_error("the service account key is not RSA");
    std::string out(BN_num_bytes(bn), '\0');
    BN_bn2bin(bn, reinterpret_cast<unsigned char*>(out.data()));
    BN_free(bn);
    return out;
}

std::int64_t Unix(std::chrono::system_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
}

const Resource& Kind(const char* plural) { return *Registry::Builtin().Find("", "v1", plural); }

}  // namespace

Value User::ToJson() const {
    ValueBuilder b(json::Type::kObject);
    b["username"] = name;
    if (!uid.empty()) b["uid"] = uid;
    b["groups"] = groups;
    if (!extra.empty()) {
        ValueBuilder e(json::Type::kObject);
        for (const auto& [k, v] : extra) e[k] = v;
        b["extra"] = e;
    }
    return b.ExtractValue();
}

struct Auth::Keys {
    crypto::SignerRs256 signer;
    crypto::VerifierRs256 verifier;
    std::string kid;
    std::string n, e;

    explicit Keys(const std::string& pem)
        : signer(pem), verifier(crypto::PublicKey::LoadFromPrivateKey(crypto::PrivateKey::LoadFromString(pem))) {
        auto pub = crypto::PublicKey::LoadFromPrivateKey(crypto::PrivateKey::LoadFromString(pem));
        auto* key = pub.GetNative();
        unsigned char* der = nullptr;
        int len = i2d_PUBKEY(key, &der);
        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256(der, len, digest);
        OPENSSL_free(der);
        // As upstream names keys: the hash of the public key, so every
        // replica given the same key agrees on the name.
        kid = B64(std::string_view(reinterpret_cast<const char*>(digest), sizeof(digest)));
        n = B64(Bignum(key, OSSL_PKEY_PARAM_RSA_N));
        e = B64(Bignum(key, OSSL_PKEY_PARAM_RSA_E));
    }
};

Auth::Auth(const userver::components::ComponentConfig& config, const userver::components::ComponentContext& context)
    : ComponentBase(config, context),
      store_(context.FindComponent<Store>()),
      issuer_(config["service-account-issuer"].As<std::string>("https://kubernetes.default.svc.cluster.local")),
      jwks_uri_(config["service-account-jwks-uri"].As<std::string>("")),
      audiences_(config["api-audiences"].As<std::vector<std::string>>(std::vector<std::string>{})) {
    if (audiences_.empty()) audiences_.push_back(issuer_);
    if (jwks_uri_.empty()) jwks_uri_ = issuer_ + "/openid/v1/jwks";

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
        User u{fields[1], fields.size() > 2 ? fields[2] : "", {}, {}};
        if (fields.size() > 3) {
            std::stringstream gs(fields[3]);
            std::string g;
            while (std::getline(gs, g, ',')) u.groups.push_back(g);
        }
        u.groups.push_back("system:authenticated");
        static_[fields[0]] = u;
    }

    auto key_file = config["service-account-key-file"].As<std::string>("");
    if (!key_file.empty()) {
        keys_ = std::make_unique<Keys>(ReadFile(key_file));
    } else {
        LOG_WARNING() << "yapi-auth: no service account key; service account tokens are off";
    }
}

Auth::~Auth() = default;

userver::yaml_config::Schema Auth::GetStaticConfigSchema() {
    return userver::yaml_config::MergeSchemas<ComponentBase>(R"(
type: object
description: authentication
additionalProperties: false
properties:
    tokens-file:
        type: string
        description: static bearer tokens, in the format of kube-apiserver's --token-auth-file
    service-account-key-file:
        type: string
        description: PEM RSA private key that signs service account tokens
    service-account-issuer:
        type: string
        description: the iss claim of service account tokens
    service-account-jwks-uri:
        type: string
        description: where the discovery document says the keys are
    api-audiences:
        type: array
        description: audiences this server accepts tokens for; the issuer when empty
        items:
            type: string
            description: an audience
)");
}

std::optional<User> Auth::Authenticate(std::string_view header) {
    if (!header.starts_with("Bearer ")) return std::nullopt;
    std::vector<std::string> audiences;
    return Review(std::string(header.substr(7)), audiences);
}

std::optional<User> Auth::Review(const std::string& token, std::vector<std::string>& audiences) {
    if (auto it = static_.find(token); it != static_.end()) {
        audiences = audiences_;
        return it->second;
    }
    if (!keys_ || std::count(token.begin(), token.end(), '.') != 2) return std::nullopt;
    auto now = std::chrono::steady_clock::now();
    {
        std::unique_lock lock(cache_mutex_);
        if (auto it = cache_.find(token); it != cache_.end() && it->second.until > now) {
            audiences = it->second.audiences;
            return it->second.user;
        }
    }
    auto user = Verify(token, audiences);
    std::unique_lock lock(cache_mutex_);
    if (cache_.size() >= kCacheMax) cache_.clear();
    cache_[token] = Cached{user, audiences, now + kCacheFor};
    return user;
}

std::optional<User> Auth::Verify(const std::string& token, std::vector<std::string>& audiences) {
    auto dot1 = token.find('.');
    auto dot2 = token.find('.', dot1 + 1);
    Value header, claims;
    try {
        header = json::FromString(crypto::base64::Base64UrlDecode(token.substr(0, dot1)));
        claims = json::FromString(crypto::base64::Base64UrlDecode(token.substr(dot1 + 1, dot2 - dot1 - 1)));
        if (header["alg"].As<std::string>("") != "RS256") return std::nullopt;
        keys_->verifier.Verify(
            {std::string_view(token).substr(0, dot2)}, crypto::base64::Base64UrlDecode(token.substr(dot2 + 1))
        );
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (claims["iss"].As<std::string>("") != issuer_) return std::nullopt;
    auto now = Unix(std::chrono::system_clock::now());
    // A minute of leeway for clocks, as upstream allows.
    if (claims["exp"].As<std::int64_t>(0) + 60 < now || claims["nbf"].As<std::int64_t>(0) - 60 > now) {
        return std::nullopt;
    }
    audiences.clear();
    for (const auto& a : objects::Items(claims["aud"])) {
        auto s = a.As<std::string>("");
        if (std::find(audiences_.begin(), audiences_.end(), s) != audiences_.end()) audiences.push_back(s);
    }
    if (audiences.empty()) return std::nullopt;

    const auto k8s = claims["kubernetes.io"];
    auto ns = k8s["namespace"].As<std::string>("");
    auto sa_name = k8s["serviceaccount"]["name"].As<std::string>("");
    auto sa = store_.Get(Kind("serviceaccounts"), ns, sa_name);
    if (!sa || objects::Deleting(*sa) ||
        (*sa)["metadata"]["uid"].As<std::string>("") != k8s["serviceaccount"]["uid"].As<std::string>("")) {
        return std::nullopt;
    }
    User u;
    u.name = "system:serviceaccount:" + ns + ":" + sa_name;
    u.uid = (*sa)["metadata"]["uid"].As<std::string>("");
    u.groups = {"system:serviceaccounts", "system:serviceaccounts:" + ns, "system:authenticated"};
    // A bound token lives no longer than what it is bound to.
    for (const char* bound : {"pod", "secret"}) {
        if (!k8s.HasMember(bound)) continue;
        auto name = k8s[bound]["name"].As<std::string>("");
        auto object = store_.Get(Kind(std::string(bound) == "pod" ? "pods" : "secrets"), ns, name);
        if (!object || objects::Deleting(*object) ||
            (*object)["metadata"]["uid"].As<std::string>("") != k8s[bound]["uid"].As<std::string>("")) {
            return std::nullopt;
        }
        if (std::string(bound) == "pod") {
            u.extra["authentication.kubernetes.io/pod-name"] = {name};
            u.extra["authentication.kubernetes.io/pod-uid"] = {k8s["pod"]["uid"].As<std::string>("")};
        }
    }
    if (k8s.HasMember("node")) {
        u.extra["authentication.kubernetes.io/node-name"] = {k8s["node"]["name"].As<std::string>("")};
        u.extra["authentication.kubernetes.io/node-uid"] = {k8s["node"]["uid"].As<std::string>("")};
    }
    if (claims.HasMember("jti")) u.extra["authentication.kubernetes.io/credential-id"] = {"JTI=" + claims["jti"].As<std::string>()};
    return u;
}

Auth::Issued Auth::Issue(
    const Value& sa,
    const std::vector<std::string>& audiences,
    std::chrono::seconds ttl,
    const std::optional<Value>& pod,
    const std::optional<Value>& secret,
    const std::optional<Value>& node
) const {
    if (!keys_) throw ApiError(404, "NotFound", "this server does not issue service account tokens");
    auto now = std::chrono::system_clock::now();
    auto expires = std::chrono::time_point_cast<std::chrono::seconds>(now + ttl);
    auto ref = [](const Value& object) {
        ValueBuilder b(json::Type::kObject);
        b["name"] = object["metadata"]["name"];
        b["uid"] = object["metadata"]["uid"];
        return b.ExtractValue();
    };
    ValueBuilder header(json::Type::kObject);
    header["alg"] = "RS256";
    header["kid"] = keys_->kid;
    ValueBuilder claims(json::Type::kObject);
    claims["aud"] = audiences;
    claims["exp"] = Unix(expires);
    claims["iat"] = Unix(now);
    claims["iss"] = issuer_;
    claims["jti"] = userver::utils::generators::GenerateUuid();
    claims["nbf"] = Unix(now);
    auto ns = objects::Namespace(sa);
    claims["sub"] = "system:serviceaccount:" + ns + ":" + objects::Name(sa);
    ValueBuilder k8s(json::Type::kObject);
    k8s["namespace"] = ns;
    k8s["serviceaccount"] = ref(sa);
    if (pod) k8s["pod"] = ref(*pod);
    if (secret) k8s["secret"] = ref(*secret);
    if (node) k8s["node"] = ref(*node);
    claims["kubernetes.io"] = k8s;
    auto signing_input = B64(json::ToString(header.ExtractValue())) + "." + B64(json::ToString(claims.ExtractValue()));
    return {signing_input + "." + B64(keys_->signer.Sign({signing_input})), expires};
}

std::string Auth::OpenIdConfiguration() const {
    ValueBuilder b(json::Type::kObject);
    b["issuer"] = issuer_;
    b["jwks_uri"] = jwks_uri_;
    b["response_types_supported"] = std::vector<std::string>{"id_token"};
    b["subject_types_supported"] = std::vector<std::string>{"public"};
    b["id_token_signing_alg_values_supported"] = std::vector<std::string>{"RS256"};
    return json::ToString(b.ExtractValue());
}

std::string Auth::Jwks() const {
    ValueBuilder keys(json::Type::kArray);
    if (keys_) {
        ValueBuilder k(json::Type::kObject);
        k["use"] = "sig";
        k["kty"] = "RSA";
        k["kid"] = keys_->kid;
        k["alg"] = "RS256";
        k["n"] = keys_->n;
        k["e"] = keys_->e;
        keys.PushBack(k.ExtractValue());
    }
    ValueBuilder b(json::Type::kObject);
    b["keys"] = keys;
    return json::ToString(b.ExtractValue());
}

}  // namespace yapi
