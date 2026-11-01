#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <userver/engine/deadline.hpp>
#include <userver/engine/io/common.hpp>
#include <userver/formats/json/value.hpp>

namespace yapi::kubelet {

using Headers = std::vector<std::pair<std::string, std::string>>;

// One HTTP/1.1 exchange with a kubelet over TLS. The kubelet's serving
// certificate is its own, self-signed, so it is not checked, as upstream
// does not check it without --kubelet-certificate-authority.
class Connection {
public:
    // The kubelet of the node, at its InternalIP and daemon endpoint port.
    static std::unique_ptr<Connection> Open(const userver::formats::json::Value& node, userver::engine::Deadline deadline);
    ~Connection();

    void Request(const std::string& method, const std::string& target, const Headers& headers, userver::engine::Deadline deadline);

    struct Head {
        int status = 0;
        Headers headers;
        std::string Header(std::string_view name) const;
    };
    Head ReadHead(userver::engine::Deadline deadline);

    // The next piece of the body, whatever framing it came in; false at its end.
    bool ReadBody(std::string& out, userver::engine::Deadline deadline);

    // After a 101: carries bytes both ways between the client and the kubelet
    // until either side closes.
    void Pipe(userver::engine::io::RwBase& client);

private:
    Connection() = default;
    std::size_t Fill(userver::engine::Deadline deadline);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string buffer_;
    long long remaining_ = -1;  // content length left; -1 until the end of the stream
    bool chunked_ = false;
    bool done_ = false;
};

}  // namespace yapi::kubelet
