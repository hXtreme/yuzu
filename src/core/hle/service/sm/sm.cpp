// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <tuple>
#include "common/assert.h"
#include "core/hle/ipc_helpers.h"
#include "core/hle/kernel/client_port.h"
#include "core/hle/kernel/client_session.h"
#include "core/hle/kernel/server_port.h"
#include "core/hle/result.h"
#include "core/hle/service/sm/controller.h"
#include "core/hle/service/sm/sm.h"

namespace Service {
namespace SM {

void ServiceManager::InvokeControlRequest(Kernel::HLERequestContext& context) {
    controller_interface->InvokeRequest(context);
}

static ResultCode ValidateServiceName(const std::string& name) {
    if (name.size() <= 0 || name.size() > 8) {
        return ERR_INVALID_NAME_SIZE;
    }
    if (name.find('\0') != std::string::npos) {
        return ERR_NAME_CONTAINS_NUL;
    }
    return RESULT_SUCCESS;
}

void ServiceManager::InstallInterfaces(std::shared_ptr<ServiceManager> self) {
    ASSERT(self->sm_interface.expired());

    auto sm = std::make_shared<SM>(self);
    sm->InstallAsNamedPort();
    self->sm_interface = sm;
    self->controller_interface = std::make_unique<Controller>();
}

ResultVal<Kernel::SharedPtr<Kernel::ServerPort>> ServiceManager::RegisterService(
    std::string name, unsigned int max_sessions) {

    CASCADE_CODE(ValidateServiceName(name));

    if (registered_services.find(name) != registered_services.end())
        return ERR_ALREADY_REGISTERED;

    Kernel::SharedPtr<Kernel::ServerPort> server_port;
    Kernel::SharedPtr<Kernel::ClientPort> client_port;
    std::tie(server_port, client_port) = Kernel::ServerPort::CreatePortPair(max_sessions, name);

    registered_services.emplace(std::move(name), std::move(client_port));
    return MakeResult<Kernel::SharedPtr<Kernel::ServerPort>>(std::move(server_port));
}

ResultVal<Kernel::SharedPtr<Kernel::ClientPort>> ServiceManager::GetServicePort(
    const std::string& name) {

    CASCADE_CODE(ValidateServiceName(name));
    auto it = registered_services.find(name);
    if (it == registered_services.end()) {
        return ERR_SERVICE_NOT_REGISTERED;
    }

    return MakeResult<Kernel::SharedPtr<Kernel::ClientPort>>(it->second);
}

ResultVal<Kernel::SharedPtr<Kernel::ClientSession>> ServiceManager::ConnectToService(
    const std::string& name) {

    CASCADE_RESULT(auto client_port, GetServicePort(name));
    return client_port->Connect();
}

std::shared_ptr<ServiceManager> g_service_manager;

/**
 * SM::Initialize service function
 *  Inputs:
 *      0: 0x00000000
 *  Outputs:
 *      1: ResultCode
 */
void SM::Initialize(Kernel::HLERequestContext& ctx) {
    IPC::RequestBuilder rb{ctx, 1};
    rb.Push(RESULT_SUCCESS);
    LOG_DEBUG(Service_SM, "called");
}

/**
 * SM::GetServiceHandle service function
 *  Inputs:
 *      0: 0x00000001
 *      1: Unknown
 *      2: Unknown
 *      3-4: 8-byte UTF-8 service name
 *  Outputs:
 *      1: ResultCode
 *      3: Service handle
 */
void SM::GetService(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    u32 unk1 = rp.Pop<u32>();
    u32 unk2 = rp.Pop<u32>();
    auto name_buf = rp.PopRaw<std::array<char, 9>>();
    std::string name(name_buf.data());

    // TODO(yuriks): Permission checks go here

    auto client_port = service_manager->GetServicePort(name);
    if (client_port.Failed()) {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0, 0);
        rb.Push(client_port.Code());
        LOG_ERROR(Service_SM, "called service=%s -> error 0x%08X", name.c_str(),
                  client_port.Code().raw);
        return;
    }

    auto session = client_port.Unwrap()->Connect();
    if (session.Succeeded()) {
        LOG_DEBUG(Service_SM, "called service=%s -> session=%u", name.c_str(),
                  (*session)->GetObjectId());
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0, 1);
        rb.Push(session.Code());
        rb.PushObjects(std::move(session).Unwrap());
    } else if (session.Code() == Kernel::ERR_MAX_CONNECTIONS_REACHED /*&& return_port_on_failure*/) {
        LOG_WARNING(Service_SM, "called service=%s -> ERR_MAX_CONNECTIONS_REACHED, *port*=%u",
                    name.c_str(), (*client_port)->GetObjectId());
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0, 1);
        rb.Push(ERR_MAX_CONNECTIONS_REACHED);
        rb.PushObjects(std::move(client_port).Unwrap());
    } else {
        LOG_ERROR(Service_SM, "called service=%s -> error 0x%08X", name.c_str(), session.Code());
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0, 0);
        rb.Push(session.Code());
    }
}

SM::SM(std::shared_ptr<ServiceManager> service_manager)
    : ServiceFramework("sm:", 4), service_manager(std::move(service_manager)) {
    static const FunctionInfo functions[] = {
        {0x00000000, &SM::Initialize, "Initialize"},
        {0x00000001, &SM::GetService, "GetService"},
        {0x00000002, nullptr, "RegisterService"},
        {0x00000003, nullptr, "UnregisterService"},
    };
    RegisterHandlers(functions);
}

SM::~SM() = default;

} // namespace SM
} // namespace Service