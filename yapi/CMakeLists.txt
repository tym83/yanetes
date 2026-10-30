cmake_minimum_required(VERSION 3.16)
project(yapi CXX)

# The ydb part of userver uses a helper the core part defines, and the
# userver image keeps the YDB SDK under /usr/share/yandex.
list(APPEND CMAKE_PREFIX_PATH /usr/share/yandex)
find_package(userver COMPONENTS core REQUIRED)
find_package(ydb-cpp-sdk REQUIRED COMPONENTS
    Coordination Driver FederatedTopic Iam Operation Query Result Scheme SolomonStats Table Topic Types)
find_package(userver COMPONENTS core ydb REQUIRED)
userver_setup_environment()

add_library(yapi_objs OBJECT
    src/registry.cpp
    src/store.cpp
    src/watch.cpp
    src/patch.cpp
    src/objects.cpp
    src/selectors.cpp
    src/api.cpp
    src/status.cpp
    src/proto.cpp
    src/auth.cpp
    src/services.cpp
    src/tables.cpp
    src/admission.cpp
    src/kubelet.cpp
    src/schema.cpp
    src/crds.cpp
    src/webhooks.cpp
    src/quantity.cpp
    src/openapi.cpp
)
target_include_directories(yapi_objs PUBLIC src)
find_package(Protobuf REQUIRED)
find_package(OpenSSL REQUIRED)
target_link_libraries(yapi_objs PUBLIC userver::core userver::ydb protobuf::libprotoc protobuf::libprotobuf OpenSSL::Crypto)

add_executable(yapi src/main.cpp)
target_link_libraries(yapi PRIVATE yapi_objs)

add_executable(yapi_unittest
    src/selectors_test.cpp
    src/objects_test.cpp
    src/patch_test.cpp
)
target_link_libraries(yapi_unittest PRIVATE yapi_objs userver::utest)
add_google_tests(yapi_unittest)
