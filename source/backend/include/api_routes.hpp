#pragma once
// Registers every /api/* route. Kept out of main.cpp so that stays a
// plain wiring/startup file.

#include "autoloader.hpp"
#include "config.hpp"
#include "http_server.hpp"
#include "payload_registry.hpp"

namespace prospero {

void RegisterApiRoutes(HttpServer *server, const Config &cfg, PayloadRegistry *registry,
                        Autoloader *autoloader);

} // namespace prospero
