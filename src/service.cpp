/*
    Copyright (c) 2015 Evgeny Safronov <division494@gmail.com>
    Copyright (c) 2011-2015 Other contributors as noted in the AUTHORS file.
    This file is part of Cocaine.
    Cocaine is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.
    Cocaine is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Lesser General Public License for more details.
    You should have received a copy of the GNU Lesser General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "cocaine/framework/service.hpp"

#include "cocaine/framework/detail/basic_session.hpp"
#include "cocaine/framework/detail/log.hpp"
#include "cocaine/framework/detail/resolver.hpp"
#include "cocaine/framework/trace.hpp"

namespace ph = std::placeholders;

using namespace cocaine::framework;
using namespace cocaine::framework::detail;

namespace {

task<void>::future_type
on_resolve(task<resolver_t::result_t>::future_move_type future, uint version, std::shared_ptr<session_t> session) {
    auto info = future.get();
    if (version != info.version) {
        return make_ready_future<void>::error(version_mismatch(version, info.version));
    }

    return session->connect(info.endpoints);
}

void
on_connect(task<void>::future_move_type future) {
    try {
        future.get();
        CF_DBG("<< connected");
    } catch (const std::exception& err) {
        CF_DBG("<< failed to connect: %s", err.what());
        throw;
    }
}

} // namespace

class basic_service_t::impl {
public:
    std::string name;
    uint version;
    scheduler_t& scheduler;
    std::shared_ptr<serialized_resolver_t> resolver;
    std::mutex mutex;

    impl(std::string name, uint version, endpoints_t locations, scheduler_t& scheduler) :
        name(std::move(name)),
        version(version),
        scheduler(scheduler),
        resolver(std::make_shared<serialized_resolver_t>(std::move(locations), scheduler))
    {}
};

basic_service_t::basic_service_t(internal_logger_t logger_, std::string name, uint version, endpoints_t locations, scheduler_t& scheduler) :
    d(new impl(std::move(name), version, std::move(locations), scheduler)),
    session(std::make_shared<session_t>(scheduler)),
    scheduler(scheduler),
    logger(std::move(logger_))
{}

basic_service_t::basic_service_t(basic_service_t&& other) :
    d(std::move(other.d)),
    session(std::move(other.session)),
    scheduler(other.scheduler),
    logger(std::move(other.logger))
{}

basic_service_t::~basic_service_t() {}

const std::string&
basic_service_t::name() const noexcept {
    return d->name;
}

uint
basic_service_t::version() const noexcept {
    return d->version;
}

auto basic_service_t::hard_shutdown(bool policy) -> void {
    session->hard_shutdown(policy);
}

cocaine::framework::future<void>
basic_service_t::connect() {
    CF_CTX("SC");
    CF_DBG(">> connecting ...");

    std::lock_guard<std::mutex> lock(d->mutex);

    // Internally the session manages with connection state itself. On any network error it
    // should drop its internal state and return false.
    if (session->connected()) {
        CF_DBG("already connected");
        return make_ready_future<void>::value();
    }

    return d->resolver->resolve(d->name)
        .then(trace::wrap(trace_t::bind(&::on_resolve, ph::_1, d->version, session)))
        .then(trace::wrap(trace_t::bind(&::on_connect, ph::_1)));
}

boost::optional<session_t::endpoint_type>
basic_service_t::endpoint() const {
    return session->endpoint();
}

basic_service_t::native_handle_type
basic_service_t::native_handle() const {
    return session->native_handle();
}
