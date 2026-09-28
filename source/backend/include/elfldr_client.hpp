#pragma once
// Talks the elfldr wire protocol (127.0.0.1:9021), same as every loader
// in the ecosystem — lets us launch bundled payloads instead of reimplementing them.

#include <cstdint>
#include <string>

namespace prospero {

class ElfldrClient {
  public:
    ElfldrClient(std::string host, uint16_t port);

    // Streams an ELF file to elfldr, which spawns it as a detached process.
    bool LaunchFile(const std::string &local_path, std::string *err) const;

    // Just opens+closes a connection — for the sidebar's live-status check.
    bool Reachable(std::string *err) const;

  private:
    bool Connect(int *out_fd, std::string *err) const;

    std::string host_;
    uint16_t port_;
};

} // namespace prospero
