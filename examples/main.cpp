#include <cocaine/framework/dispatcher.hpp>
#include <cocaine/framework/services/storage.hpp>

#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <cstdlib>

class App1 {
    struct on_event1 :
        public cocaine::framework::handler<App1>,
        public std::enable_shared_from_this<on_event1>
    {
        on_event1(App1 *a) :
            cocaine::framework::handler<App1>(a)
        {
            // pass
        }

        void
        on_chunk(const char *chunk,
                 size_t size)
        {
            auto generator = app()->m_storage->read(
                "namespace",
                "key." + cocaine::framework::unpack<std::string>(chunk, size)
            );
            generator.then(std::bind(&on_event1::send_resp, shared_from_this(), std::placeholders::_1));
        }

        void
        send_resp(cocaine::framework::generator<std::string>& g) {
            response()->write(g.next());
            response()->close();
        }
    };
    friend class on_event1;

public:
    void
    init(cocaine::framework::dispatcher_t *d) {
        m_log = d->service_manager()->get_system_logger();
        m_storage = d->service_manager()->get_service<cocaine::framework::storage_service_t>("storage");

        d->on<on_event1>("event1", this);
        d->on("event2", this, &App1::on_event2);

        COCAINE_LOG_WARNING(m_log, "test log");
    }

    void
    on_event2(const std::string& event,
              const std::vector<std::string>& /*request*/,
              cocaine::framework::response_ptr response)
    {
        response->write("on_event2:" + event);
    }

private:
    std::shared_ptr<cocaine::framework::logger_t> m_log;
    std::shared_ptr<cocaine::framework::storage_service_t> m_storage;
};

int
main(int argc,
     char *argv[])
{
    return cocaine::framework::run<App1>(argc, argv);
}
