#include "cocaine/framework/service.hpp"

#include "cocaine/framework/resolver.hpp"

using namespace cocaine::framework;

class basic_service_t::impl {
public:
    std::string name;
    uint version;
    scheduler_t& scheduler;
    resolver_t resolver;
    std::mutex mutex;

    impl(std::string name, uint version, scheduler_t& scheduler) :
        name(std::move(name)),
        version(version),
        scheduler(scheduler),
        resolver(scheduler)
    {}
};

basic_service_t::basic_service_t(std::string name, uint version, scheduler_t& scheduler) :
    d(new impl(std::move(name), version, scheduler)),
    sess(std::make_shared<session<>>(scheduler))
{}

basic_service_t::~basic_service_t() {}

auto basic_service_t::name() const -> const std::string& {
    return d->name;
}

auto basic_service_t::connect() -> future_t<void> {
    // TODO: Connector, which manages future queue and returns future<shared_ptr<session<T>>> for 'name'.
    CF_CTX("service '%s'", name());
    CF_CTX("connect");
    CF_DBG("connecting ...");

    // TODO: Make async.
    std::lock_guard<std::mutex> lock(d->mutex);

    // Internally the session manages with connection state itself. On any network error it
    // should drop its internal state and return false.
    if (sess->connected()) {
        CF_DBG("already connected");
        return make_ready_future<void>::value();
    }

    try {
        const auto info = d->resolver.resolve(d->name).get();
        if (d->version != info.version) {
            return make_ready_future<void>::error(std::runtime_error("version mismatch"));
        }

        sess->connect(info.endpoints).get();
        CF_DBG("connected");
        return make_ready_future<void>::value();
    } catch (const std::runtime_error& err) {
        CF_DBG("failed to connect: %s", err.what());
        return make_ready_future<void>::error(err);
    }
}
