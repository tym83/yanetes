#include <kubelet.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>

#include <userver/engine/io/exception.hpp>
#include <userver/engine/io/socket.hpp>
#include <userver/engine/io/sockaddr.hpp>
#include <userver/engine/io/tls_wrapper.hpp>
#include <userver/engine/task/cancel.hpp>

#include <objects.hpp>
#include <status.hpp>

namespace yapi::kubelet {

namespace io = userver::engine::io;
using userver::engine::Deadline;

namespace {

bool SameName(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(x) == std::tolower(y); });
}

}  // namespace

struct Connection::Impl {
    io::TlsWrapper tls;
    explicit Impl(io::TlsWrapper&& t) : tls(std::move(t)) {}
};

Connection::~Connection() = default;

std::string Connection::Head::Header(std::string_view name) const {
    for (const auto& [k, v] : headers) {
        if (SameName(k, name)) return v;
    }
    return "";
}

std::unique_ptr<Connection> Connection::Open(const userver::formats::json::Value& node, Deadline deadline) {
    std::string ip;
    for (const auto& a : objects::Items(node["status"]["addresses"])) {
        if (a["type"].As<std::string>("") == "InternalIP") ip = a["address"].As<std::string>("");
    }
    if (ip.empty()) throw ApiError(503, "ServiceUnavailable", "node " + objects::Name(node) + " has no InternalIP");
    auto port = node["status"]["daemonEndpoints"]["kubeletEndpoint"]["Port"].As<int>(10250);
    try {
        auto addr = io::Sockaddr::MakeIPSocketAddress(ip);
        addr.SetPort(static_cast<std::uint16_t>(port));
        io::Socket socket(addr.Domain(), io::SocketType::kStream);
        socket.Connect(addr, deadline);
        std::unique_ptr<Connection> c(new Connection());
        c->impl_ = std::make_unique<Impl>(io::TlsWrapper::StartTlsClient(std::move(socket), "", deadline));
        return c;
    } catch (const std::exception& e) {
        throw ApiError(502, "InternalError", "error dialing kubelet " + ip + ":" + std::to_string(port) + ": " + e.what());
    }
}

void Connection::Request(const std::string& method, const std::string& target, const Headers& headers, Deadline deadline) {
    std::string req = method + " " + target + " HTTP/1.1\r\n";
    for (const auto& [k, v] : headers) req += k + ": " + v + "\r\n";
    req += "\r\n";
    if (impl_->tls.SendAll(req.data(), req.size(), deadline) != req.size()) {
        throw ApiError(502, "InternalError", "the kubelet closed the connection");
    }
}

std::size_t Connection::Fill(Deadline deadline) {
    char buf[16384];
    auto n = impl_->tls.RecvSome(buf, sizeof(buf), deadline);
    buffer_.append(buf, n);
    return n;
}

Connection::Head Connection::ReadHead(Deadline deadline) {
    std::size_t end;
    while ((end = buffer_.find("\r\n\r\n")) == std::string::npos) {
        if (buffer_.size() > 64 * 1024) throw ApiError(502, "InternalError", "the kubelet sent an oversized response head");
        if (Fill(deadline) == 0) throw ApiError(502, "InternalError", "the kubelet closed the connection");
    }
    Head head;
    std::string text = buffer_.substr(0, end);
    buffer_.erase(0, end + 4);
    auto line_end = text.find("\r\n");
    auto status_line = text.substr(0, line_end);
    auto sp = status_line.find(' ');
    head.status = sp == std::string::npos ? 0 : std::atoi(status_line.c_str() + sp + 1);
    std::size_t pos = line_end == std::string::npos ? text.size() : line_end + 2;
    while (pos < text.size()) {
        auto next = text.find("\r\n", pos);
        if (next == std::string::npos) next = text.size();
        auto line = text.substr(pos, next - pos);
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            auto value = line.substr(colon + 1);
            value.erase(0, value.find_first_not_of(' '));
            head.headers.emplace_back(line.substr(0, colon), value);
        }
        pos = next + 2;
    }
    chunked_ = head.Header("Transfer-Encoding").find("chunked") != std::string::npos;
    auto length = head.Header("Content-Length");
    remaining_ = length.empty() ? -1 : std::stoll(length);
    done_ = remaining_ == 0;
    return head;
}

bool Connection::ReadBody(std::string& out, Deadline deadline) {
    out.clear();
    if (done_) return false;
    if (chunked_) {
        std::size_t eol;
        while ((eol = buffer_.find("\r\n")) == std::string::npos) {
            if (Fill(deadline) == 0) return done_ = true, false;
        }
        auto size = std::stoull(buffer_.substr(0, eol), nullptr, 16);
        buffer_.erase(0, eol + 2);
        if (size == 0) return done_ = true, false;
        while (buffer_.size() < size + 2) {
            if (Fill(deadline) == 0) return done_ = true, false;
        }
        out = buffer_.substr(0, size);
        buffer_.erase(0, size + 2);
        return true;
    }
    if (buffer_.empty() && Fill(deadline) == 0) return done_ = true, false;
    if (remaining_ >= 0) {
        auto take = std::min<std::size_t>(buffer_.size(), static_cast<std::size_t>(remaining_));
        out = buffer_.substr(0, take);
        buffer_.erase(0, take);
        remaining_ -= static_cast<long long>(take);
        done_ = remaining_ == 0;
        return true;
    }
    out = std::move(buffer_);
    buffer_.clear();
    return true;
}

void Connection::Pipe(io::RwBase& client) {
    // One task does both directions: a TLS session is not to be read and
    // written from two tasks at once. Each side gets a short wait in turn.
    char buf[32768];
    if (!buffer_.empty()) {
        if (client.WriteAll(buffer_.data(), buffer_.size(), Deadline{}) != buffer_.size()) return;
        buffer_.clear();
    }
    auto quiet = std::chrono::milliseconds(1);
    while (!userver::engine::current_task::ShouldCancel()) {
        bool moved = false;
        for (int side = 0; side < 2; ++side) {
            io::RwBase& from = side == 0 ? client : static_cast<io::RwBase&>(impl_->tls);
            io::RwBase& to = side == 0 ? static_cast<io::RwBase&>(impl_->tls) : client;
            std::optional<std::size_t> n;
            // Both sockets are TLS; waiting on one that stays quiet ends in
            // a timeout, which here only means "nothing yet".
            try {
                try {
                    n = from.ReadNoblock(buf, sizeof(buf));
                } catch (const io::IoTimeout&) {
                }
                if (!n) {
                    try {
                        if (from.WaitReadable(Deadline::FromDuration(quiet))) n = from.ReadNoblock(buf, sizeof(buf));
                    } catch (const io::IoTimeout&) {
                    }
                }
            } catch (const std::exception&) {
                return;
            }
            if (!n) continue;
            if (*n == 0) return;
            try {
                if (to.WriteAll(buf, *n, Deadline::FromDuration(std::chrono::seconds(30))) != *n) return;
            } catch (const std::exception&) {
                return;
            }
            moved = true;
        }
        // Idle sessions back off to a few wakeups a second.
        quiet = moved ? std::chrono::milliseconds(1) : std::min(quiet * 2, std::chrono::milliseconds(20));
    }
}

}  // namespace yapi::kubelet
