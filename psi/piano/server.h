#pragma once

#include <spdlog/spdlog.h>

#include <array>
#include <cstdint>
#include <future>
#include <memory>
#include <utility>
#include <vector>

#include "yacl/link/context.h"

#include "psi/piano/serialize.h"
#include "psi/piano/util.h"

namespace psi::piano {

class QueryServiceServer {
 public:
  // Constructor: initializes the server with a database, context, set_size, and
  // chunk_size
  QueryServiceServer(std::vector<uint64_t>& db,
                     std::shared_ptr<yacl::link::Context> context,
                     uint64_t set_size, uint64_t chunk_size);

  // Starts the server to handle incoming requests
  void Start(const std::future<void>& stop_signal);

  // Handles the incoming request based on its type
  void HandleRequest(const yacl::Buffer& request_data);

  // Processes a request to fetch the full database
  void ProcessFetchFullDB();

  // Processes a set parity query and returns the parity and server compute time
  std::pair<std::vector<uint64_t>, uint64_t> ProcessSetParityQuery(
      const std::vector<uint64_t>& indices);

 private:
  // Accesses the database and returns the corresponding entry
  DBEntry DBAccess(uint64_t id);

  // Handles a set parity query and returns the parity
  std::vector<uint64_t> HandleSetParityQuery(
      const std::vector<uint64_t>& indices);

  std::vector<uint64_t> db_;                      // The database
  std::shared_ptr<yacl::link::Context> context_;  // The communication context
  uint64_t set_size_;                             // The size of the set
  uint64_t chunk_size_;                           // The size of each chunk
};

}  // namespace psi::piano
