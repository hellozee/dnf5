/*
Copyright Contributors to the libdnf project.

This file is part of libdnf: https://github.com/rpm-software-management/libdnf/

Libdnf is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

Libdnf is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with libdnf.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "needs_restarting.hpp"

#include <libdnf5-cli/argument_parser.hpp>
#include <libdnf5-cli/output/changelogs.hpp>
#include <libdnf5/conf/const.hpp>
#include <libdnf5/conf/option_string.hpp>
#include <libdnf5/rpm/package.hpp>
#include <libdnf5/rpm/package_query.hpp>
#include <libdnf5/utils/bgettext/bgettext-mark-domain.h>
#include <utils/string.hpp>

#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace dnf5 {

using namespace libdnf5::cli;

void NeedsRestartingCommand::set_parent_command() {
    auto * arg_parser_parent_cmd = get_session().get_argument_parser().get_root_command();
    auto * arg_parser_this_cmd = get_argument_parser_command();
    arg_parser_parent_cmd->register_command(arg_parser_this_cmd);
}

void NeedsRestartingCommand::set_argument_parser() {
    auto & ctx = get_context();
    auto & parser = ctx.get_argument_parser();

    auto & cmd = *get_argument_parser_command();
    cmd.set_description("Determine whether system or systemd services need restarting");

    services_option =
        dynamic_cast<libdnf5::OptionBool *>(parser.add_init_value(std::make_unique<libdnf5::OptionBool>(false)));

    auto * services_arg = parser.add_new_named_arg("services");
    services_arg->set_long_name("services");
    services_arg->set_short_name('s');
    services_arg->set_description("List systemd services started before their dependencies were updated");
    services_arg->set_const_value("true");
    services_arg->link_value(services_option);
    cmd.register_named_arg(services_arg);
}

void NeedsRestartingCommand::configure() {
    auto & context = get_context();
    context.set_load_system_repo(true);

    context.set_load_available_repos(Context::LoadAvailableRepos::ENABLED);
    context.base.get_config().get_optional_metadata_types_option().add(
        libdnf5::Option::Priority::RUNTIME, libdnf5::OPTIONAL_METADATA_TYPES);

    context.base.get_config().get_optional_metadata_types_option().add_item(
        libdnf5::Option::Priority::RUNTIME, libdnf5::METADATA_TYPE_UPDATEINFO);
}

time_t get_boot_time() {
    time_t proc_1_boot_time = 0;
    struct stat proc_1_stat = {};
    if (stat("/proc/1", &proc_1_stat) == 0) {
        proc_1_boot_time = proc_1_stat.st_mtime;
    }

    time_t uptime_boot_time = 0;
    std::ifstream uptime_stream{"/proc/uptime"};
    if (uptime_stream.is_open()) {
        double uptime = 0;
        uptime_stream >> uptime;
        if (uptime > 0) {
            uptime_boot_time = std::time(nullptr) - static_cast<time_t>(uptime);
        }
    }

    return std::max(proc_1_boot_time, uptime_boot_time);
}

libdnf5::rpm::PackageSet recursive_dependencies(
    const libdnf5::rpm::Package & package, libdnf5::rpm::PackageQuery & installed) {
    libdnf5::rpm::PackageSet dependencies{package.get_base()};
    dependencies.add(package);

    std::vector<libdnf5::rpm::Package> stack;
    stack.emplace_back(package);

    while (!stack.empty()) {
        const auto & current = stack.back();
        stack.pop_back();

        libdnf5::rpm::PackageQuery query{installed};
        query.filter_provides(current.get_requires());

        for (const auto & dependency : query) {
            if (!dependencies.contains(dependency)) {
                stack.emplace_back(dependency);
            }
        }

        dependencies |= query;
    }

    return dependencies;
}

void NeedsRestartingCommand::system_needs_restarting(Context & ctx) {
    const auto boot_time = get_boot_time();

    libdnf5::rpm::PackageQuery base_query{ctx.base};

    libdnf5::rpm::PackageQuery installed{base_query};
    installed.filter_installed();

    libdnf5::rpm::PackageQuery reboot_suggested{installed};
    reboot_suggested.filter_reboot_suggested();

    std::vector<libdnf5::rpm::Package> need_reboot = {};
    for (const auto & pkg : reboot_suggested) {
        if (pkg.get_install_time() > static_cast<unsigned long long>(boot_time)) {
            need_reboot.push_back(pkg);
        }
    }

    if (need_reboot.empty()) {
        std::cout << "No core libraries or services have been updated since boot-up.\n"
                  << "Reboot should not be necessary.\n";
    } else {
        std::cout << "Core libraries or services have been updated since boot-up:\n";
        for (const auto & pkg : need_reboot) {
            std::cout << "  * " << pkg.get_name() << '\n';
        }
        std::cout << "\nReboot is required to fully utilize these updates.\n"
                  << "More information: https://access.redhat.com/solutions/27943\n";
        throw libdnf5::cli::SilentCommandExitError(1);
    }
}

void NeedsRestartingCommand::services_need_restarting(Context & ctx) {
    std::unique_ptr<sdbus::IConnection> connection;
    try {
        connection = sdbus::createSystemBusConnection();
    } catch (const sdbus::Error & ex) {
        throw libdnf5::cli::CommandExitError(1, M_("Couldn't connect to D-Bus: {}"), ex.what());
    }

    connection->enterEventLoopAsync();
    auto systemd_proxy = sdbus::createProxy("org.freedesktop.systemd1", "/org/freedesktop/systemd1");

    const std::string manager_interface{"org.freedesktop.systemd1.Manager"};

    std::vector<sdbus::Struct<
        std::string,
        std::string,
        std::string,
        std::string,
        std::string,
        std::string,
        sdbus::ObjectPath,
        uint32_t,
        std::string,
        sdbus::ObjectPath>>
        units;
    systemd_proxy->callMethod("ListUnits").onInterface(manager_interface).storeResultsTo(units);

    const std::string unit_interface{"org.freedesktop.systemd1.Unit"};

    struct Service {
        std::string name;
        uint64_t start_timestamp_us;
    };

    std::unordered_map<std::string, Service> unit_file_to_service;

    for (const auto & unit : units) {
        // See ListUnits here:
        // https://www.freedesktop.org/wiki/Software/systemd/dbus/
        const auto unit_name = std::get<0>(unit);

        // Only consider service units. Skip timers, targets, etc.
        if (!libdnf5::utils::string::ends_with(unit_name, ".service")) {
            continue;
        }

        const auto unit_object_path = std::get<6>(unit);
        auto unit_proxy = sdbus::createProxy("org.freedesktop.systemd1", unit_object_path);

        // Only consider active (running) services
        const auto active_state = unit_proxy->getProperty("ActiveState").onInterface(unit_interface);
        if (std::string{active_state} != "active") {
            continue;
        }

        // FragmentPath is the path to the unit file that defines the service
        const auto fragment_path = unit_proxy->getProperty("FragmentPath").onInterface(unit_interface);
        const auto start_timestamp_us = unit_proxy->getProperty("ActiveEnterTimestamp").onInterface(unit_interface);

        unit_file_to_service.insert(std::make_pair(fragment_path, Service{unit_name, start_timestamp_us}));
    }

    // Iterate over each file from each installed package and check whether it
    // is a unit file for a running service. This is much faster than running
    // filter_file on each unit file.
    libdnf5::rpm::PackageQuery base_query{ctx.base};

    libdnf5::rpm::PackageQuery installed{base_query};
    installed.filter_installed();

    std::vector<std::string> service_names;
    for (const auto & package : installed) {
        for (const auto & file : package.get_files()) {
            const auto & service_pair = unit_file_to_service.find(file);
            if (service_pair != unit_file_to_service.end()) {
                // If the file is a unit file for a running service
                const auto & service = service_pair->second;

                // Recursively get all dependencies of the package that
                // provides the service (and include the package itself)
                const auto & deps = recursive_dependencies(package, installed);
                for (const auto & dep : deps) {
                    // If any dependency (or the package itself) has been
                    // updated since the service started, recommend restarting
                    // of that service
                    const uint64_t install_timestamp_us = 1000L * 1000L * dep.get_install_time();
                    if (install_timestamp_us > service.start_timestamp_us) {
                        service_names.emplace_back(service.name);
                        break;
                    }
                }
                break;
            }
        }
    }

    if (!service_names.empty()) {
        for (const auto & service_name : service_names) {
            std::cout << service_name << '\n';
        }
        throw libdnf5::cli::SilentCommandExitError(1);
    }
}

void NeedsRestartingCommand::run() {
    auto & ctx = get_context();

    if (services_option->get_value()) {
        services_need_restarting(ctx);
    } else {
        system_needs_restarting(ctx);
    }
}

}  // namespace dnf5
