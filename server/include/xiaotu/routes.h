#pragma once

namespace httplib {
class Server;
}

namespace xiaotu {
struct Services;
}

namespace xiaotu {

void register_all_routes(httplib::Server& server, Services& services);

}  // namespace xiaotu
