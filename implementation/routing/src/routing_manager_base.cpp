// Copyright (C) 2014-2016 Bayerische Motoren Werke Aktiengesellschaft (BMW AG)
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <iomanip>

#include "../../utility/include/utility.hpp"
#include "../../utility/include/byteorder.hpp"
#include "../include/routing_manager_base.hpp"
#include "../../logging/include/logger.hpp"
#include "../../endpoints/include/local_client_endpoint_impl.hpp"
#include "../../endpoints/include/local_server_endpoint_impl.hpp"
#include "../include/routing_manager_impl.hpp"

namespace vsomeip {

routing_manager_base::routing_manager_base(routing_manager_host *_host) :
        host_(_host),
        io_(host_->get_io()),
        client_(host_->get_client()),
        configuration_(host_->get_configuration()),
        serializer_(std::make_shared<serializer>())
#ifdef USE_DLT
        , tc_(tc::trace_connector::get())
#endif
{
    for (int i = 0; i < VSOMEIP_MAX_DESERIALIZER; ++i) {
        deserializers_.push(std::make_shared<deserializer>());
    }
}

routing_manager_base::~routing_manager_base() {
}

boost::asio::io_service & routing_manager_base::get_io() {
    return (io_);
}

client_t routing_manager_base::get_client() const {
    return client_;
}

void routing_manager_base::init() {
    serializer_->create_data(configuration_->get_max_message_size_local());
}

bool routing_manager_base::offer_service(client_t _client, service_t _service,
            instance_t _instance, major_version_t _major,
            minor_version_t _minor) {
    (void)_client;

    // Remote route (incoming only)
    auto its_info = find_service(_service, _instance);
    if (its_info) {
        if (!its_info->is_local()) {
            return false;
        } else if (its_info->get_major() == _major
                && its_info->get_minor() == _minor) {
            its_info->set_ttl(DEFAULT_TTL);
        } else {
            host_->on_error(error_code_e::SERVICE_PROPERTY_MISMATCH);
            return false;
        }
    } else {
        its_info = create_service_info(_service, _instance, _major, _minor,
                DEFAULT_TTL, true);
    }
    return true;
}

void routing_manager_base::stop_offer_service(client_t _client, service_t _service,
            instance_t _instance, major_version_t _major, minor_version_t _minor) {
    (void)_client;
    (void)_major;
    (void)_minor;
    {
        std::lock_guard<std::mutex> its_lock(events_mutex_);
        auto its_events_service = events_.find(_service);
        if (its_events_service != events_.end()) {
            auto its_events_instance = its_events_service->second.find(_instance);
            if (its_events_instance != its_events_service->second.end()) {
                for (auto &e : its_events_instance->second)
                    e.second->unset_payload();
            }
        }
    }
    {
        std::lock_guard<std::mutex> its_lock(eventgroup_clients_mutex_);
        auto its_service = eventgroup_clients_.find(_service);
        if (its_service != eventgroup_clients_.end()) {
            auto its_instance = its_service->second.find(_instance);
            if (its_instance != its_service->second.end()) {
                its_instance->second.clear();
            }
        }
    }
}

void routing_manager_base::request_service(client_t _client, service_t _service,
            instance_t _instance, major_version_t _major,
            minor_version_t _minor, bool _use_exclusive_proxy) {
    (void)_use_exclusive_proxy;

    auto its_info = find_service(_service, _instance);
    if (its_info) {
        if ((_major == its_info->get_major()
                || DEFAULT_MAJOR == its_info->get_major())
                && (_minor <= its_info->get_minor()
                    || DEFAULT_MINOR == its_info->get_minor()
                    || _minor == ANY_MINOR)) {
            its_info->add_client(_client);
        } else {
            host_->on_error(error_code_e::SERVICE_PROPERTY_MISMATCH);
        }
    }
}

void routing_manager_base::release_service(client_t _client, service_t _service,
            instance_t _instance) {
    auto its_info = find_service(_service, _instance);
    if (its_info) {
        its_info->remove_client(_client);
    }
}

void routing_manager_base::register_event(client_t _client, service_t _service, instance_t _instance,
            event_t _event, const std::set<eventgroup_t> &_eventgroups, bool _is_field,
            std::chrono::milliseconds _cycle, bool _change_resets_cycle,
            epsilon_change_func_t _epsilon_change_func,
            bool _is_provided, bool _is_shadow, bool _is_cache_placeholder) {
    std::shared_ptr<event> its_event = find_event(_service, _instance, _event);
    if (its_event) {
        if(!its_event->is_cache_placeholder()) {
            if (its_event->is_field() == _is_field) {
                if (_is_provided) {
                    its_event->set_provided(true);
                }
                if (_is_shadow && _is_provided) {
                    its_event->set_shadow(_is_shadow);
                }
                for (auto eg : _eventgroups) {
                    its_event->add_eventgroup(eg);
                }
            } else {
                VSOMEIP_ERROR << "Event registration update failed. "
                        "Specified arguments do not match existing registration.";
            }
        } else {
            // the found event was a placeholder for caching.
            // update it with the real values
            if(!_is_field) {
                // don't cache payload for non-fields
                its_event->unset_payload(true);
            }
            if (_is_shadow && _is_provided) {
                its_event->set_shadow(_is_shadow);
            }
            its_event->set_field(_is_field);
            its_event->set_provided(_is_provided);
            its_event->set_cache_placeholder(false);
            std::shared_ptr<serviceinfo> its_service = find_service(_service, _instance);
            if (its_service) {
                its_event->set_version(its_service->get_major());
            }
            if (_eventgroups.size() == 0) { // No eventgroup specified
                std::set<eventgroup_t> its_eventgroups;
                its_eventgroups.insert(_event);
                its_event->set_eventgroups(its_eventgroups);
            } else {
                its_event->set_eventgroups(_eventgroups);
            }

            its_event->set_epsilon_change_function(_epsilon_change_func);
            its_event->set_change_resets_cycle(_change_resets_cycle);
            its_event->set_update_cycle(_cycle);
        }
    } else {
        its_event = std::make_shared<event>(this, _is_shadow);
        its_event->set_service(_service);
        its_event->set_instance(_instance);
        its_event->set_event(_event);
        its_event->set_field(_is_field);
        its_event->set_provided(_is_provided);
        its_event->set_cache_placeholder(_is_cache_placeholder);
        std::shared_ptr<serviceinfo> its_service = find_service(_service, _instance);
        if (its_service) {
            its_event->set_version(its_service->get_major());
        }

        if (_eventgroups.size() == 0) { // No eventgroup specified
            std::set<eventgroup_t> its_eventgroups;
            its_eventgroups.insert(_event);
            its_event->set_eventgroups(its_eventgroups);
        } else {
            its_event->set_eventgroups(_eventgroups);
        }

        its_event->set_epsilon_change_function(_epsilon_change_func);
        its_event->set_change_resets_cycle(_change_resets_cycle);
        its_event->set_update_cycle(_cycle);
    }

    if(!_is_cache_placeholder) {
        its_event->add_ref(_client, _is_provided);
    }

    for (auto eg : _eventgroups) {
        std::shared_ptr<eventgroupinfo> its_eventgroup_info
            = find_eventgroup(_service, _instance, eg);
        if (!its_eventgroup_info) {
            its_eventgroup_info = std::make_shared<eventgroupinfo>();
            std::lock_guard<std::mutex> its_lock(eventgroups_mutex_);
            eventgroups_[_service][_instance][eg] = its_eventgroup_info;
        }
        its_eventgroup_info->add_event(its_event);
    }

    std::lock_guard<std::mutex> its_lock(events_mutex_);
    events_[_service][_instance][_event] = its_event;
}

void routing_manager_base::unregister_event(client_t _client, service_t _service, instance_t _instance,
            event_t _event, bool _is_provided) {
    (void)_client;
    std::lock_guard<std::mutex> its_lock(events_mutex_);
    auto found_service = events_.find(_service);
    if (found_service != events_.end()) {
        auto found_instance = found_service->second.find(_instance);
        if (found_instance != found_service->second.end()) {
            auto found_event = found_instance->second.find(_event);
            if (found_event != found_instance->second.end()) {
                auto its_event = found_event->second;
                its_event->remove_ref(_client, _is_provided);
                if (!its_event->has_ref()) {
                    auto its_eventgroups = its_event->get_eventgroups();
                    for (auto eg : its_eventgroups) {
                        std::shared_ptr<eventgroupinfo> its_eventgroup_info
                            = find_eventgroup(_service, _instance, eg);
                        if (its_eventgroup_info) {
                            its_eventgroup_info->remove_event(its_event);
                            if (0 == its_eventgroup_info->get_events().size()) {
                                remove_eventgroup_info(_service, _instance, eg);
                            }
                        }
                    }
                    found_instance->second.erase(_event);
                } else if (_is_provided) {
                    its_event->set_provided(false);
                }
            }
        }
    }
}

std::shared_ptr<event> routing_manager_base::get_event(
       service_t _service, instance_t _instance, event_t _event) const {
    std::shared_ptr<event> its_event;
    std::lock_guard<std::mutex> its_lock(events_mutex_);
    auto found_service = events_.find(_service);
    if (found_service != events_.end()) {
        auto found_instance = found_service->second.find(_instance);
        if (found_instance != found_service->second.end()) {
            auto found_event = found_instance->second.find(_event);
            if (found_event != found_instance->second.end()) {
                return found_event->second;
            }
        }
    }
    return its_event;
}


std::set<std::shared_ptr<event>> routing_manager_base::find_events(
        service_t _service, instance_t _instance,
        eventgroup_t _eventgroup) const {
    std::lock_guard<std::mutex> its_lock(eventgroups_mutex_);
    std::set<std::shared_ptr<event> > its_events;
    auto found_service = eventgroups_.find(_service);
    if (found_service != eventgroups_.end()) {
        auto found_instance = found_service->second.find(_instance);
        if (found_instance != found_service->second.end()) {
            auto found_eventgroup = found_instance->second.find(_eventgroup);
            if (found_eventgroup != found_instance->second.end()) {
                return (found_eventgroup->second->get_events());
            }
        }
    }
    return (its_events);
}

void routing_manager_base::subscribe(client_t _client, service_t _service,
            instance_t _instance, eventgroup_t _eventgroup,
            major_version_t _major,
            subscription_type_e _subscription_type) {

    (void) _major;
    (void) _subscription_type;

    bool inserted = insert_subscription(_service, _instance, _eventgroup, _client);
    if (inserted) {
        auto its_eventgroup = find_eventgroup(_service, _instance, _eventgroup);
        if (its_eventgroup) {
            std::set<std::shared_ptr<event> > its_events
                = its_eventgroup->get_events();
            for (auto e : its_events) {
                if (e->is_field())
                    e->notify_one(_client, true); // TODO: use _flush to send all events together!
            }
        }
    }
}

void routing_manager_base::unsubscribe(client_t _client, service_t _service,
            instance_t _instance, eventgroup_t _eventgroup) {
    std::lock_guard<std::mutex> its_lock(eventgroup_clients_mutex_);
    auto its_service = eventgroup_clients_.find(_service);
    if (its_service != eventgroup_clients_.end()) {
        auto its_instance = its_service->second.find(_instance);
        if (its_instance != its_service->second.end()) {
            auto its_group = its_instance->second.find(_eventgroup);
            if (its_group != its_instance->second.end()) {
                its_group->second.erase(_client);
            }
        }
    }
}

void routing_manager_base::notify(service_t _service, instance_t _instance,
            event_t _event, std::shared_ptr<payload> _payload,
            bool _force, bool _flush) {
    std::shared_ptr<event> its_event = find_event(_service, _instance, _event);
    if (its_event) {
        its_event->set_payload(_payload, _force, _flush);
    } else {
        VSOMEIP_WARNING << "Attempt to update the undefined event/field ["
            << std::hex << _service << "." << _instance << "." << _event
            << "]";
    }
}

void routing_manager_base::notify_one(service_t _service, instance_t _instance,
            event_t _event, std::shared_ptr<payload> _payload,
            client_t _client, bool _force, bool _flush) {
    std::shared_ptr<event> its_event = find_event(_service, _instance, _event);
    if (its_event) {
        // Event is valid for service/instance
        bool found_eventgroup(false);
        // Iterate over all groups of the event to ensure at least
        // one valid eventgroup for service/instance exists.
        for (auto its_group : its_event->get_eventgroups()) {
            auto its_eventgroup = find_eventgroup(_service, _instance, its_group);
            if (its_eventgroup) {
                // Eventgroup is valid for service/instance
                found_eventgroup = true;
                break;
            }
        }
        if (found_eventgroup) {
            its_event->set_payload(_payload, _client, _force, _flush);
        }
    } else {
        VSOMEIP_WARNING << "Attempt to update the undefined event/field ["
            << std::hex << _service << "." << _instance << "." << _event
            << "]";
    }
}

bool routing_manager_base::send(client_t its_client,
        std::shared_ptr<message> _message,
        bool _flush) {
    bool is_sent(false);
    if (utility::is_request(_message->get_message_type())) {
        _message->set_client(its_client);
    }
    std::lock_guard<std::mutex> its_lock(serialize_mutex_);
    if (serializer_->serialize(_message.get())) {
        is_sent = send(its_client, serializer_->get_data(),
                serializer_->get_size(), _message->get_instance(),
                _flush, _message->is_reliable());
        serializer_->reset();
    } else {
        VSOMEIP_ERROR << "Failed to serialize message. Check message size!";
    }
    return (is_sent);
}

// ********************************* PROTECTED **************************************
std::shared_ptr<serviceinfo> routing_manager_base::create_service_info(
        service_t _service, instance_t _instance, major_version_t _major,
        minor_version_t _minor, ttl_t _ttl, bool _is_local_service) {
    std::lock_guard<std::mutex> its_lock(services_mutex_);
    std::shared_ptr<serviceinfo> its_info =
            std::make_shared<serviceinfo>(_major, _minor, _ttl, _is_local_service);
    services_[_service][_instance] = its_info;
    return its_info;
}

std::shared_ptr<serviceinfo> routing_manager_base::find_service(
        service_t _service, instance_t _instance) const {
    std::shared_ptr<serviceinfo> its_info;
    std::lock_guard<std::mutex> its_lock(services_mutex_);
    auto found_service = services_.find(_service);
    if (found_service != services_.end()) {
        auto found_instance = found_service->second.find(_instance);
        if (found_instance != found_service->second.end()) {
            its_info = found_instance->second;
        }
    }
    return (its_info);
}

void routing_manager_base::clear_service_info(service_t _service, instance_t _instance,
        bool _reliable) {
    std::shared_ptr<serviceinfo> its_info(find_service(_service, _instance));
    if (!its_info) {
        return;
    }

    std::lock_guard<std::mutex> its_lock(services_mutex_);

    // Clear service_info and service_group
    std::shared_ptr<endpoint> its_empty_endpoint;
    if (!its_info->get_endpoint(!_reliable)) {
        if (1 >= services_[_service].size()) {
            services_.erase(_service);
        } else {
            services_[_service].erase(_instance);
        }
    } else {
        its_info->set_endpoint(its_empty_endpoint, _reliable);
    }
}

services_t routing_manager_base::get_services() const {
    services_t its_offers;
    std::lock_guard<std::mutex> its_lock(services_mutex_);
    for (auto s : services_) {
        for (auto i : s.second) {
            its_offers[s.first][i.first] = i.second;
        }
    }
    return (its_offers);
}

bool routing_manager_base::is_available(service_t _service, instance_t _instance,
        major_version_t _major) {
    bool available(false);
    std::lock_guard<std::mutex> its_lock(local_services_mutex_);
    auto its_service = local_services_.find(_service);
    if (its_service != local_services_.end()) {
        auto its_instance = its_service->second.find(_instance);
        if (its_instance != its_service->second.end()) {
            if (std::get<0>(its_instance->second) == _major) {
                available = true;
            }
        }
    }
    return available;
}

client_t routing_manager_base::find_local_client(service_t _service, instance_t _instance) {
    std::lock_guard<std::mutex> its_lock(local_services_mutex_);
    client_t its_client(VSOMEIP_ROUTING_CLIENT);
    auto its_service = local_services_.find(_service);
    if (its_service != local_services_.end()) {
        auto its_instance = its_service->second.find(_instance);
        if (its_instance != its_service->second.end()) {
            its_client = std::get<2>(its_instance->second);
        }
    }
    return its_client;
}

std::shared_ptr<endpoint> routing_manager_base::create_local(client_t _client) {
    std::lock_guard<std::mutex> its_lock(local_endpoint_mutex_);

    std::stringstream its_path;
    its_path << VSOMEIP_BASE_PATH << std::hex << _client;

#ifdef WIN32
    boost::asio::ip::address address = boost::asio::ip::address::from_string("127.0.0.1");
    int port = VSOMEIP_INTERNAL_BASE_PORT + _client;
    VSOMEIP_DEBUG<< "Connecting to ["
        << std::hex << _client << "] at " << port;
#else
    VSOMEIP_DEBUG << "Client [" << std::hex << get_client() << "] is connecting to ["
            << std::hex << _client << "] at " << its_path.str();
#endif
    std::shared_ptr<local_client_endpoint_impl> its_endpoint = std::make_shared<
        local_client_endpoint_impl>(shared_from_this(),
#ifdef WIN32
        boost::asio::ip::tcp::endpoint(address, port)
#else
        boost::asio::local::stream_protocol::endpoint(its_path.str())
#endif
    , io_, configuration_->get_max_message_size_local());

    if (_client != VSOMEIP_ROUTING_CLIENT) {
        // VSOMEIP_ROUTING_CLIENT must not be added here.
        // The routing master should not get an end-point when calling find_local
        // with VSOMEIP_ROUTING_CLIENT as parameter
        // as it indicates remote communication!

        local_endpoints_[_client] = std::static_pointer_cast<endpoint>(its_endpoint);
        if (routing_manager_impl *rm_impl = dynamic_cast<routing_manager_impl*>(this)) {
            its_endpoint->register_error_callback(
                    std::bind(&routing_manager_impl::on_clientendpoint_error,
                            rm_impl, _client));
        } else {
            its_endpoint->register_error_callback(
                    std::bind(&routing_manager_base::on_clientendpoint_error, this,
                            _client));
        }
    }

    return (its_endpoint);
}

std::shared_ptr<endpoint> routing_manager_base::find_local(client_t _client) {
    std::lock_guard<std::mutex> its_lock(local_endpoint_mutex_);
    std::shared_ptr<endpoint> its_endpoint;
    auto found_endpoint = local_endpoints_.find(_client);
    if (found_endpoint != local_endpoints_.end()) {
        its_endpoint = found_endpoint->second;
    }
    return (its_endpoint);
}

std::shared_ptr<endpoint> routing_manager_base::find_or_create_local(client_t _client) {
    std::shared_ptr<endpoint> its_endpoint(find_local(_client));
    if (!its_endpoint) {
        its_endpoint = create_local(_client);
        its_endpoint->start();
    }
    return (its_endpoint);
}

void routing_manager_base::remove_local(client_t _client) {
    std::shared_ptr<endpoint> its_endpoint(find_local(_client));
    if (its_endpoint) {
        its_endpoint->stop();
        VSOMEIP_DEBUG << "Client [" << std::hex << get_client() << "] is closing connection to ["
                      << std::hex << _client << "]";
        std::lock_guard<std::mutex> its_lock(local_endpoint_mutex_);
        local_endpoints_.erase(_client);
    }
    {
        std::lock_guard<std::mutex> its_lock(local_services_mutex_);
        // Finally remove all services that are implemented by the client.
        std::set<std::pair<service_t, instance_t>> its_services;
        for (auto& s : local_services_) {
            for (auto& i : s.second) {
                if (std::get<2>(i.second) == _client)
                    its_services.insert({ s.first, i.first });
            }
        }

        for (auto& si : its_services) {
            local_services_[si.first].erase(si.second);
            if (local_services_[si.first].size() == 0)
                local_services_.erase(si.first);
        }
    }
    // delete client's subscriptions if he didn't unsubscribe properly
    {
        std::lock_guard<std::mutex> its_lock(eventgroup_clients_mutex_);
        for (auto service_it = eventgroup_clients_.begin();
                service_it != eventgroup_clients_.end();) {
            for (auto instance_it = service_it->second.begin();
                    instance_it != service_it->second.end();) {
                for (auto eventgroup_it = instance_it->second.begin();
                        eventgroup_it != instance_it->second.end();) {
                    if (eventgroup_it->second.erase(_client) > 0) {
                        if (eventgroup_it->second.size() == 0) {
                            eventgroup_it = instance_it->second.erase(eventgroup_it);
                        } else {
                            ++eventgroup_it;
                        }
                    } else {
                        ++eventgroup_it;
                    }
                }

                if (instance_it->second.size() == 0) {
                    instance_it = service_it->second.erase(instance_it);
                } else {
                    ++instance_it;
                }
            }

            if (service_it->second.size() == 0) {
                service_it = eventgroup_clients_.erase(service_it);
            } else {
                ++service_it;
            }
        }
    }
}

std::shared_ptr<endpoint> routing_manager_base::find_local(service_t _service,
        instance_t _instance) {
    return find_local(find_local_client(_service, _instance));
}

std::unordered_set<client_t> routing_manager_base::get_connected_clients() {
    std::lock_guard<std::mutex> its_lock(local_endpoint_mutex_);
    std::unordered_set<client_t> clients;
    for (auto its_client : local_endpoints_) {
        clients.insert(its_client.first);
    }
    return clients;
}

std::shared_ptr<event> routing_manager_base::find_event(service_t _service,
        instance_t _instance, event_t _event) const {
    std::shared_ptr<event> its_event;
    std::lock_guard<std::mutex> its_lock(events_mutex_);
    auto find_service = events_.find(_service);
    if (find_service != events_.end()) {
        auto find_instance = find_service->second.find(_instance);
        if (find_instance != find_service->second.end()) {
            auto find_event = find_instance->second.find(_event);
            if (find_event != find_instance->second.end()) {
                its_event = find_event->second;
            }
        }
    }
    return (its_event);
}

std::shared_ptr<eventgroupinfo> routing_manager_base::find_eventgroup(
        service_t _service, instance_t _instance,
        eventgroup_t _eventgroup) const {
    std::lock_guard<std::mutex> its_lock(eventgroups_mutex_);

    std::shared_ptr<eventgroupinfo> its_info(nullptr);
    auto found_service = eventgroups_.find(_service);
    if (found_service != eventgroups_.end()) {
        auto found_instance = found_service->second.find(_instance);
        if (found_instance != found_service->second.end()) {
            auto found_eventgroup = found_instance->second.find(_eventgroup);
            if (found_eventgroup != found_instance->second.end()) {
                its_info = found_eventgroup->second;
                std::shared_ptr<serviceinfo> its_service_info
                    = find_service(_service, _instance);
                if (its_service_info) {
                    std::string its_multicast_address;
                    uint16_t its_multicast_port;
                    if (configuration_->get_multicast(_service, _instance,
                            _eventgroup,
                            its_multicast_address, its_multicast_port)) {
                        try {
                            its_info->set_multicast(
                                    boost::asio::ip::address::from_string(
                                            its_multicast_address),
                                    its_multicast_port);
                        }
                        catch (...) {
                            VSOMEIP_ERROR << "Eventgroup ["
                                << std::hex << std::setw(4) << std::setfill('0')
                                << _service << "." << _instance << "." << _eventgroup
                                << "] is configured as multicast, but no valid "
                                       "multicast address is configured!";
                        }
                    }
                    its_info->set_major(its_service_info->get_major());
                    its_info->set_ttl(its_service_info->get_ttl());
                    its_info->set_threshold(configuration_->get_threshold(
                            _service, _instance, _eventgroup));
                }
            }
        }
    }
    return (its_info);
}

void routing_manager_base::remove_eventgroup_info(service_t _service,
        instance_t _instance, eventgroup_t _eventgroup) {
    std::lock_guard<std::mutex> its_lock(eventgroups_mutex_);
    auto found_service = eventgroups_.find(_service);
    if (found_service != eventgroups_.end()) {
        auto found_instance = found_service->second.find(_instance);
        if (found_instance != found_service->second.end()) {
            found_instance->second.erase(_eventgroup);
        }
    }
}

std::set<client_t> routing_manager_base::find_local_clients(service_t _service,
        instance_t _instance, eventgroup_t _eventgroup) {
    std::set<client_t> its_clients;
    std::lock_guard<std::mutex> its_lock(eventgroup_clients_mutex_);
    auto found_service = eventgroup_clients_.find(_service);
    if (found_service != eventgroup_clients_.end()) {
        auto found_instance = found_service->second.find(_instance);
        if (found_instance != found_service->second.end()) {
            auto found_eventgroup = found_instance->second.find(_eventgroup);
            if (found_eventgroup != found_instance->second.end()) {
                its_clients = found_eventgroup->second;
            }
        }
    }
    return (its_clients);
}

bool routing_manager_base::send_local_notification(client_t _client,
        const byte_t *_data, uint32_t _size, instance_t _instance,
        bool _flush, bool _reliable) {
#ifdef USE_DLT
    bool has_local(false);
#endif
    bool has_remote(false);
    method_t its_method = VSOMEIP_BYTES_TO_WORD(_data[VSOMEIP_METHOD_POS_MIN],
            _data[VSOMEIP_METHOD_POS_MAX]);
    service_t its_service = VSOMEIP_BYTES_TO_WORD(
            _data[VSOMEIP_SERVICE_POS_MIN], _data[VSOMEIP_SERVICE_POS_MAX]);
    std::shared_ptr<event> its_event = find_event(its_service, _instance, its_method);
    if (its_event && !its_event->is_shadow()) {
        std::vector< byte_t > its_data;

        for (auto its_group : its_event->get_eventgroups()) {
            // local
            auto its_local_clients = find_local_clients(its_service, _instance, its_group);
            for (auto its_local_client : its_local_clients) {
                if (its_local_client == VSOMEIP_ROUTING_CLIENT) {
                    has_remote = true;
                    continue;
                }
#ifdef USE_DLT
                else {
                    has_local = true;
                }
#endif
                std::shared_ptr<endpoint> its_local_target = find_local(its_local_client);
                if (its_local_target) {
                    send_local(its_local_target, _client, _data, _size,
                            _instance, _flush, _reliable, VSOMEIP_SEND);
                }
            }
        }
    }
#ifdef USE_DLT
    // Trace the message if a local client but will _not_ be forwarded to the routing manager
    if (has_local && !has_remote) {
        uint16_t its_data_size
            = uint16_t(_size > USHRT_MAX ? USHRT_MAX : _size);

        tc::trace_header its_header;
        if (its_header.prepare(nullptr, true))
            tc_->trace(its_header.data_, VSOMEIP_TRACE_HEADER_SIZE,
                    _data, its_data_size);
    }
#endif
    return has_remote;
}

bool routing_manager_base::send_local(
        std::shared_ptr<endpoint>& _target, client_t _client,
        const byte_t *_data, uint32_t _size, instance_t _instance,
        bool _flush, bool _reliable, uint8_t _command) const {

    client_t sender = get_client();
    size_t additional_size = 0;
    if (_command == VSOMEIP_NOTIFY_ONE) {
        additional_size +=sizeof(client_t);
    }
    std::vector<byte_t> its_command(
            VSOMEIP_COMMAND_HEADER_SIZE + _size + sizeof(instance_t)
                    + sizeof(bool) + sizeof(bool) + additional_size);
    its_command[VSOMEIP_COMMAND_TYPE_POS] = _command;
    std::memcpy(&its_command[VSOMEIP_COMMAND_CLIENT_POS], &sender,
            sizeof(client_t));
    std::memcpy(&its_command[VSOMEIP_COMMAND_SIZE_POS_MIN], &_size,
            sizeof(_size));
    std::memcpy(&its_command[VSOMEIP_COMMAND_PAYLOAD_POS], _data,
            _size);
    std::memcpy(&its_command[VSOMEIP_COMMAND_PAYLOAD_POS + _size],
            &_instance, sizeof(instance_t));
    std::memcpy(&its_command[VSOMEIP_COMMAND_PAYLOAD_POS + _size
            + sizeof(instance_t)], &_flush, sizeof(bool));
    std::memcpy(&its_command[VSOMEIP_COMMAND_PAYLOAD_POS + _size
            + sizeof(instance_t) + sizeof(bool)], &_reliable, sizeof(bool));
    if (_command == VSOMEIP_NOTIFY_ONE) {
        // Add target client
        std::memcpy(&its_command[VSOMEIP_COMMAND_PAYLOAD_POS + _size
                + sizeof(instance_t) + sizeof(bool) + sizeof(bool)], &_client, sizeof(client_t));
    }

    return _target->send(&its_command[0], uint32_t(its_command.size()));
}

bool routing_manager_base::insert_subscription(
        service_t _service, instance_t _instance, eventgroup_t _eventgroup,
        client_t _client) {
    std::lock_guard<std::mutex> its_lock(eventgroup_clients_mutex_);
    auto found_service = eventgroup_clients_.find(_service);
    if (found_service != eventgroup_clients_.end()) {
        auto found_instance = found_service->second.find(_instance);
        if (found_instance != found_service->second.end()) {
            auto found_eventgroup = found_instance->second.find(_eventgroup);
            if (found_eventgroup != found_instance->second.end()) {
                if (found_eventgroup->second.find(_client)
                        != found_eventgroup->second.end())
                    return false;
            }
        }
    }

    eventgroup_clients_[_service][_instance][_eventgroup].insert(_client);
    return true;
}

void routing_manager_base::on_clientendpoint_error(client_t _client) {
    VSOMEIP_ERROR << "Client endpoint error for application: " << std::hex
            << std::setfill('0') << std::setw(4) << _client;
    remove_local(_client);
}

std::shared_ptr<deserializer> routing_manager_base::get_deserializer() {
    std::unique_lock<std::mutex> its_lock(deserializer_mutex_);
    while (deserializers_.empty()) {
        VSOMEIP_INFO << std::hex << "client " << get_client() <<
                "routing_manager_base::get_deserializer ~> all in use!";
        deserializer_condition_.wait(its_lock);
        VSOMEIP_INFO << std::hex << "client " << get_client() <<
                        "routing_manager_base::get_deserializer ~> wait finished!";
    }
    auto deserializer = deserializers_.front();
    deserializers_.pop();
    return deserializer;
}

void routing_manager_base::put_deserializer(std::shared_ptr<deserializer> _deserializer) {
    {
        std::lock_guard<std::mutex> its_lock(deserializer_mutex_);
        deserializers_.push(_deserializer);
    }
    deserializer_condition_.notify_one();
}

#ifndef WIN32
bool routing_manager_base::check_credentials(client_t _client, uid_t _uid, gid_t _gid) {
    return configuration_->check_credentials(_client, _uid, _gid);
}
#endif

void routing_manager_base::send_pending_subscriptions(service_t _service,
        instance_t _instance, major_version_t _major) {
    for (auto &ps : pending_subscriptions_) {
        if (ps.service_ == _service &&
                ps.instance_ == _instance && ps.major_ == _major) {
            send_subscribe(client_, ps.service_, ps.instance_,
                    ps.eventgroup_, ps.major_, ps.subscription_type_);
        }
    }
}

void routing_manager_base::remove_pending_subscription(service_t _service,
        instance_t _instance) {
    auto it = pending_subscriptions_.begin();
    while (it != pending_subscriptions_.end()) {
        if (it->service_ == _service
         && it->instance_ == _instance) {
            break;
        }
        it++;
    }
    if (it != pending_subscriptions_.end()) pending_subscriptions_.erase(it);
}


} // namespace vsomeip
