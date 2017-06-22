/*
 * Copyright (C) 2017 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Alberto Aguirre <alberto.aguirre@canonical.com>
 *
 */

#include "daemon.h"
#include "base_cloud_init_config.h"
#include "daemon_config.h"

#include <multipass/name_generator.h>
#include <multipass/ssh_key.h>
#include <multipass/version.h>
#include <multipass/virtual_machine_description.h>
#include <multipass/virtual_machine_execute.h>
#include <multipass/virtual_machine_factory.h>
#include <multipass/vm_image.h>
#include <multipass/vm_image_fetcher.h>
#include <multipass/vm_image_host.h>
#include <multipass/vm_image_query.h>
#include <multipass/vm_image_vault.h>

#include <yaml-cpp/yaml.h>

#include <sstream>
#include <stdexcept>

namespace mp = multipass;

namespace
{
auto make_server(const multipass::DaemonConfig& config, multipass::Rpc::Service* service)
{
    grpc::ServerBuilder builder;

    builder.AddListeningPort(config.server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(service);

    std::unique_ptr<grpc::Server> server{builder.BuildAndStart()};
    if (server == nullptr)
        throw std::runtime_error("Failed to start the RPC service");

    return server;
}
}

mp::Daemon::Daemon(std::unique_ptr<const DaemonConfig> the_config)
    : config{std::move(the_config)}, server{make_server(*config, this)}
{
}

void mp::Daemon::run()
{
    std::cout << "Server listening on " << config->server_address << "\n";
    server->Wait();
}

void mp::Daemon::shutdown()
{
    server->Shutdown();
}

grpc::Status mp::Daemon::connect(grpc::ServerContext* context, const ConnectRequest* request, ConnectReply* response)
{
    response->set_exec_line(config->vm_execute->execute());

    return grpc::Status::OK;
}

grpc::Status mp::Daemon::destroy(grpc::ServerContext* context, const DestroyRequest* request, DestroyReply* response)
{
    return grpc::Status::OK;
}

grpc::Status mp::Daemon::create(grpc::ServerContext* context, const CreateRequest* request, grpc::ServerWriter<CreateReply>* reply)
{
    VirtualMachineDescription desc;
    desc.mem_size = request->mem_size();

    if (request->vm_name().empty())
    {
        desc.vm_name = config->name_generator->make_name();
    }

    VMImageQuery vm_image_query;

    if (request->release().empty())
    {
        vm_image_query.query_string = "xenial";
    }

    std::string image_hash;

    try
    {
        config->image_host->update_image_manifest();
        image_hash = config->image_host->get_image_hash_for_query(vm_image_query.query_string);
    }
    catch (const std::runtime_error& error)
    {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, error.what(), "");
    }

    CreateReply create_reply;
    QObject::connect(config->image_host.get(), &mp::VMImageHost::progress, [=] (int const& percentage) {
        CreateReply create_reply;
        create_reply.set_download_progress(std::to_string(percentage));
        reply->Write(create_reply);
    });

    auto fetcher = config->factory->create_image_fetcher(config->image_host);
    desc.image = fetcher->fetch(vm_image_query);

    desc.cloud_init_config = YAML::Load(mp::base_cloud_init_config);

    std::stringstream ssh_key_line;
    ssh_key_line << "ssh-rsa"
                 << " " << config->ssh_key->as_base64() << " "
                 << "multipass@localhost";

    desc.cloud_init_config["ssh_authorized_keys"].push_back(ssh_key_line.str());

    vms.push_back(config->factory->create_virtual_machine(desc, *this));

    create_reply.set_create_complete("Create setup complete.");
    reply->Write(create_reply);

    create_reply.set_vm_instance_name(desc.vm_name);
    reply->Write(create_reply);

    return grpc::Status::OK;
}

grpc::Status mp::Daemon::list(grpc::ServerContext* context, const ListRequest* request, ListReply* response)
{
    return grpc::Status::OK;
}

grpc::Status mp::Daemon::start(grpc::ServerContext* context, const StartRequest* request, StartReply* response)
{
    return grpc::Status::OK;
}

grpc::Status mp::Daemon::stop(grpc::ServerContext* context, const StopRequest* request, StopReply* response)
{
    return grpc::Status::OK;
}

grpc::Status mp::Daemon::version(grpc::ServerContext* context, const VersionRequest* request, VersionReply* response)
{
    response->set_version(multipass::version_string);
    return grpc::Status::OK;
}

void mp::Daemon::on_shutdown()
{
}

void mp::Daemon::on_resume()
{
}

void mp::Daemon::on_stop()
{
}
