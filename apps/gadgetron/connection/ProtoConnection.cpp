
#include "ProtoConnection.h"

#include <typeindex>
#include <iostream>
#include <sstream>
#include <string>
#include <boost/dll/shared_library.hpp>
#include <boost/dll.hpp>
#include <boost/range/algorithm/transform.hpp>

#include "log.h"
#include "gadgetron_config.h"

#include "readers/Primitives.h"
#include "Response.h"
#include "Reader.h"
#include "Writer.h"

#include "Server.h"
#include "Config.h"

#include "Writers.h"
#include "Handlers.h"
#include "StreamConnection.h"

namespace {

    using namespace Gadgetron::Core;
    using namespace Gadgetron::Core::Readers;
    using namespace Gadgetron::Server::Connection;
    using namespace Gadgetron::Server::Connection::Writers;
    using namespace Gadgetron::Server::Connection::Handlers;

    using Header = Gadgetron::Core::Context::Header;

    std::string read_filename_from_stream(std::istream &stream) {
        char buffer[1024];
        read_into(stream, buffer);
        return std::string(buffer);
    }

    class ConfigHandler : public Handler {
    public:
        explicit ConfigHandler(std::function<void(Config)> callback)
        : callback(std::move(callback)) {}

        void handle_callback(std::istream &config_stream) {
            callback(parse_config(config_stream));
        }

    private:
        std::function<void(Config)> callback;
    };

    class ConfigReferenceHandler : public ConfigHandler {
    public:
        ConfigReferenceHandler(
                std::function<void(Config)> &callback,
                const Context::Paths &paths
        ) : ConfigHandler(callback), paths(paths) {}

        void handle(std::istream &stream) override {
            boost::filesystem::path filename = paths.gadgetron_home / GADGETRON_CONFIG_PATH / read_filename_from_stream(stream);

            GDEBUG_STREAM("Reading config file: " << filename << std::endl);

            std::ifstream config_stream(filename.string());
            handle_callback(config_stream);
        }

    private:
        const Context::Paths &paths;
    };

    class ConfigStringHandler : public ConfigHandler {
    public:
        explicit ConfigStringHandler(std::function<void(Config)> &callback)
        : ConfigHandler(callback) {}

        void handle(std::istream &stream) override {
            std::stringstream config_stream(read_string_from_stream<uint32_t>(stream));
            handle_callback(config_stream);
        }
    };


};

namespace Gadgetron::Server::Connection {

    std::map<uint16_t, std::unique_ptr<Handler>> ProtoConnection::prepare_handlers(bool &closed) {

        std::map<uint16_t, std::unique_ptr<Handler>> handlers{};

        std::function<void(boost::optional<Config>)> deliver = [&](auto config) {
            closed = true;
            channels.input->close();
            promise.set_value(config);
        };

        std::function<void()> close_callback = [&, deliver]() {
            deliver(boost::none);
        };

        std::function<void(Config)> config_callback = [&, deliver](Config config) {
            deliver(boost::make_optional(config));
        };

        handlers[FILENAME] = std::make_unique<ConfigReferenceHandler>(config_callback, paths);
        handlers[CONFIG]   = std::make_unique<ConfigStringHandler>(config_callback);
        handlers[HEADER]   = std::make_unique<ErrorProducingHandler>("Received ISMRMRD header before config file.");
        handlers[QUERY]    = std::make_unique<QueryHandler>(channels.output);
        handlers[CLOSE]    = std::make_unique<CloseHandler>(close_callback);

        return handlers;
    }

    ProtoConnection::ProtoConnection(std::iostream &stream, Context::Paths paths)
    : Connection(stream), paths(std::move(paths)) {
        channels.input = channels.output = std::make_shared<MessageChannel>();
    }

    boost::optional<Config> ProtoConnection::process(std::iostream &stream, const Context::Paths &paths) {

        ProtoConnection connection{stream, paths};
        auto future = connection.promise.get_future();

        connection.start();
        connection.join();

        return future.get();
    }
}