#pragma once

#include <string>

namespace yapi {

class Store;

// Where to send a request meant for a service port: a ready endpoint behind
// it, as kube-apiserver routes to webhooks and aggregated servers, or the
// cluster IP when there is none. The cluster IP alone hangs while
// kube-proxy has not caught up with a new service.
struct Destination {
    std::string ip;
    int port = 0;
};
Destination ServiceDestination(Store& store, const std::string& ns, const std::string& name, int port);

}  // namespace yapi
