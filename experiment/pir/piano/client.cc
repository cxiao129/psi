#include "experiment/pir/piano/client.h"

namespace pir::piano {

QueryServiceClient::QueryServiceClient(
    std::shared_ptr<yacl::link::Context> context, const uint64_t entry_num,
    const uint64_t thread_num, const uint64_t entry_size)
    : context_(std::move(context)),
      entry_num_(entry_num),
      thread_num_(thread_num),
      entry_size_(entry_size) {
  Initialize();
  InitializeLocalSets();
}

void QueryServiceClient::Initialize() {
  // Set the computational security parameter to 128
  master_key_ = SecureRandKey();
  long_key_ = GetLongKey(master_key_);

  // Q = sqrt(n) * log(k) * α(κ)
  // Maximum number of queries supported by a single preprocessing
  // Let α(κ) be any super-constant function, i.e., α(κ) = w(1)
  // Chosen log(log(κ)): grows slowly but surely > any constant as κ → ∞
  total_query_num_ = static_cast<uint64_t>(
      std::sqrt(static_cast<double>(entry_num_)) * kStatisticalSecurityLn *
      std::log(kStatisticalSecurityLn));

  std::tie(chunk_size_, set_size_) = GenChunkParams(entry_num_);

  // M1 = sqrt(n) * log(k) * α(κ)
  // The probability that the client cannot find a set that contains the online
  // query index is negligible in κ
  primary_set_num_ = total_query_num_;

  // if primary_set_num_ is not a multiple of thread_num_ then we need to add
  // some padding
  primary_set_num_ =
      (primary_set_num_ + thread_num_ - 1) / thread_num_ * thread_num_;

  // M2 = log2(k) * log(k) * α(κ)
  // The probability that the client runs out of hints in a backup group is
  // negligible in κ
  backup_set_num_per_chunk_ = static_cast<uint64_t>(
      static_cast<double>(kStatisticalSecurityLog2) * kStatisticalSecurityLn *
      std::log(kStatisticalSecurityLn));

  backup_set_num_per_chunk_ =
      (backup_set_num_per_chunk_ + thread_num_ - 1) / thread_num_ * thread_num_;

  // set_size == chunk_number
  total_backup_set_num_ = backup_set_num_per_chunk_ * set_size_;
}

void QueryServiceClient::InitializeLocalSets() {
  primary_sets_.clear();
  primary_sets_.reserve(primary_set_num_);
  local_backup_sets_.clear();
  local_backup_sets_.reserve(total_backup_set_num_);
  local_cache_.clear();
  local_miss_elements_.clear();
  uint32_t tag_counter = 0;

  // Initialize primary_sets_
  for (uint64_t j = 0; j < primary_set_num_; j++) {
    primary_sets_.emplace_back(tag_counter, DBEntry::ZeroEntry(entry_size_), 0,
                               false);
    tag_counter += 1;
  }

  // Initialize local_backup_sets_
  for (uint64_t i = 0; i < total_backup_set_num_; ++i) {
    local_backup_sets_.emplace_back(tag_counter,
                                    DBEntry::ZeroEntry(entry_size_));
    tag_counter += 1;
  }

  local_backup_set_groups_.clear();
  local_backup_set_groups_.reserve(set_size_);
  local_replacement_groups_.clear();
  local_replacement_groups_.reserve(set_size_);

  // Initialize local_backup_set_groups_ and local_replacement_groups_
  for (uint64_t i = 0; i < set_size_; i++) {
    std::vector<std::reference_wrapper<LocalBackupSet>> backup_sets;
    for (uint64_t j = 0; j < backup_set_num_per_chunk_; j++) {
      backup_sets.emplace_back(
          local_backup_sets_[(i * backup_set_num_per_chunk_) + j]);
    }
    LocalBackupSetGroup backup_group(0, backup_sets);
    local_backup_set_groups_.emplace_back(std::move(backup_group));

    std::vector<uint64_t> indices(backup_set_num_per_chunk_);
    std::vector<DBEntry> values(backup_set_num_per_chunk_);
    LocalReplacementGroup replacement_group(0, indices, values);
    local_replacement_groups_.emplace_back(std::move(replacement_group));
  }
}

void QueryServiceClient::FetchFullDB() {
  context_->SendAsync(context_->NextRank(), SerializeFetchFullDB(1),
                      "FetchFullDB");

  for (uint64_t i = 0; i < set_size_; i++) {
    auto db_chunk =
        DeserializeDBChunk(context_->Recv(context_->NextRank(), "DBChunk"));

    std::vector<bool> hit_map(chunk_size_, false);

    // Use multiple threads to parallelize the computation for the chunk
    std::vector<std::thread> threads;
    std::mutex hit_map_mutex;

    // Make sure all sets are covered
    const uint64_t primary_set_per_thread =
        (primary_set_num_ + thread_num_ - 1) / thread_num_;
    const uint64_t backup_set_per_thread =
        (total_backup_set_num_ + thread_num_ - 1) / thread_num_;

    for (uint64_t tid = 0; tid < thread_num_; tid++) {
      uint64_t start_index = tid * primary_set_per_thread;
      uint64_t end_index =
          std::min(start_index + primary_set_per_thread, primary_set_num_);

      uint64_t start_index_backup = tid * backup_set_per_thread;
      uint64_t end_index_backup = std::min(
          start_index_backup + backup_set_per_thread, total_backup_set_num_);

      threads.emplace_back([&, start_index, end_index, start_index_backup,
                            end_index_backup] {
        // Update the parities for the primary hints
        for (uint64_t j = start_index; j < end_index; j++) {
          const auto tmp =
              PRFEvalWithLongKeyAndTag(long_key_, primary_sets_[j].tag, i);
          const auto offset = tmp & (chunk_size_ - 1);
          {
            std::lock_guard<std::mutex> lock(hit_map_mutex);
            hit_map[offset] = true;
          }
          primary_sets_[j].parity.XorFromRaw(&db_chunk[offset * entry_size_]);
        }

        // Update the parities for the backup hints
        for (uint64_t j = start_index_backup; j < end_index_backup; j++) {
          // Skip if backup set belongs to chunk i
          if (j < i * backup_set_num_per_chunk_ ||
              j >= (i + 1) * backup_set_num_per_chunk_) {
            const auto tmp = PRFEvalWithLongKeyAndTag(
                long_key_, local_backup_sets_[j].tag, i);
            const auto offset = tmp & (chunk_size_ - 1);
            local_backup_sets_[j].parity_after_puncture.XorFromRaw(
                &db_chunk[offset * entry_size_]);
          }
        }
      });
    }

    for (auto& thread : threads) {
      if (thread.joinable()) {
        thread.join();
      }
    }

    // If any element is not hit, then it is a local miss. We will save it in
    // the local miss cache. Most of the time, the local miss cache will be
    // empty.
    for (uint64_t j = 0; j < chunk_size_; j++) {
      if (!hit_map[j]) {
        std::vector<uint8_t> entry_slice(entry_size_);
        std::memcpy(entry_slice.data(), &db_chunk[j * entry_size_],
                    entry_size_ * sizeof(uint8_t));
        const auto entry = DBEntry::DBEntryFromSlice(entry_slice);
        local_miss_elements_[j + (i * chunk_size_)] = entry;
      }
    }

    // Store the replacement
    yacl::crypto::Prg<uint64_t> prg(yacl::crypto::SecureRandU64());
    for (uint64_t k = 0; k < backup_set_num_per_chunk_; k++) {
      // Generate a random offset between 0 and chunk_size_ - 1
      const auto offset = prg() & (chunk_size_ - 1);
      local_replacement_groups_[i].indices[k] = offset + i * chunk_size_;
      std::vector<uint8_t> entry_slice(entry_size_);
      std::memcpy(entry_slice.data(), &db_chunk[offset * entry_size_],
                  entry_size_ * sizeof(uint8_t));
      local_replacement_groups_[i].values[k] =
          DBEntry::DBEntryFromSlice(entry_slice);
    }
  }
}

// Store results of sqrt(n) recent queries, serve duplicates locally while
// masking with a random distinct query
void QueryServiceClient::SendDummySet() const {
  yacl::crypto::Prg<uint64_t> prg(yacl::crypto::SecureRandU64());
  std::vector<uint64_t> rand_set(set_size_);
  for (uint64_t i = 0; i < set_size_; i++) {
    rand_set[i] = prg() % chunk_size_ + i * chunk_size_;
  }

  // Send the random dummy set to the server
  context_->SendAsync(context_->NextRank(), SerializeSetParityQuery(rand_set),
                      "SetParityQuery");

  auto parity_query_response = DeserializeSetParityResponse(
      context_->Recv(context_->NextRank(), "SetParityResponse"));
}

DBEntry QueryServiceClient::OnlineSingleQuery(const uint64_t x) {
  // Make sure x is not in the local cache
  if (local_cache_.find(x) != local_cache_.end()) {
    SendDummySet();
    return local_cache_[x];
  }

  // 1. Query x: the client first finds a local set that contains x
  // 2. The client expands the set, replace the chunk(x)-th element to a
  // replacement
  // 3. The client sends the edited set to the server and gets the parity
  // 4. The client recovers the answer
  uint64_t hit_set_id = std::numeric_limits<uint64_t>::max();

  const uint64_t query_offset = x % chunk_size_;
  const uint64_t chunk_id = x / chunk_size_;

  for (uint64_t i = 0; i < primary_set_num_; i++) {
    const auto& set = primary_sets_[i];
    if (const bool is_programmed_match =
            set.is_programmed &&
            chunk_id == (set.programmed_point / chunk_size_);
        !is_programmed_match &&
        PRFSetWithShortTag{set.tag}.MemberTestWithLongKey(
            long_key_, chunk_id, query_offset, chunk_size_)) {
      hit_set_id = i;
      break;
    }
  }

  DBEntry val = DBEntry::ZeroEntry(entry_size_);

  if (hit_set_id == std::numeric_limits<uint64_t>::max()) {
    if (local_miss_elements_.find(x) == local_miss_elements_.end()) {
      SPDLOG_ERROR("No hit set found for {}", x);
    } else {
      val = local_miss_elements_[x];
      local_cache_[x] = val;
    }

    SendDummySet();
    return val;
  }

  // Expand the set
  const PRFSetWithShortTag set{primary_sets_[hit_set_id].tag};
  auto expanded_set = set.ExpandWithLongKey(long_key_, set_size_, chunk_size_);

  // Manually program the set if the flag is set before
  if (primary_sets_[hit_set_id].is_programmed) {
    const uint64_t programmed_chunk_id =
        primary_sets_[hit_set_id].programmed_point / chunk_size_;
    expanded_set[programmed_chunk_id] =
        primary_sets_[hit_set_id].programmed_point;
  }

  // Edit the set by replacing the chunk(x)-th element with a replacement
  const uint64_t next_available = local_replacement_groups_[chunk_id].consumed;
  if (next_available == backup_set_num_per_chunk_) {
    SPDLOG_ERROR("No replacement available for {}", x);
    SendDummySet();
    return val;
  }

  // Consume one replacement
  const uint64_t replace_index =
      local_replacement_groups_[chunk_id].indices[next_available];
  const DBEntry replace_value =
      local_replacement_groups_[chunk_id].values[next_available];
  local_replacement_groups_[chunk_id].consumed++;
  expanded_set[chunk_id] = replace_index;

  // Send the edited set to the server
  context_->SendAsync(context_->NextRank(),
                      SerializeSetParityQuery(expanded_set), "SetParityQuery");

  const auto parity = DeserializeSetParityResponse(
      context_->Recv(context_->NextRank(), "SetParityResponse"));

  // Recover the answer
  val = primary_sets_[hit_set_id].parity;  // The parity of the hit set
  val.XorFromRaw(parity.data());           // XOR the parity of the edited set
  val.Xor(replace_value);                  // XOR the replacement value

  // Update the local cache
  local_cache_[x] = val;

  // Refresh phase
  if (local_backup_set_groups_[chunk_id].consumed ==
      backup_set_num_per_chunk_) {
    SPDLOG_WARN("No backup set available for {}", x);
    return val;
  }

  const DBEntry original_value = val;
  const uint64_t consumed = local_backup_set_groups_[chunk_id].consumed;
  primary_sets_[hit_set_id].tag =
      local_backup_set_groups_[chunk_id].sets[consumed].get().tag;
  // Backup set doesn't XOR the chunk(x)-th element in preprocessing
  val.Xor(local_backup_set_groups_[chunk_id]
              .sets[consumed]
              .get()
              .parity_after_puncture);
  primary_sets_[hit_set_id].parity = val;
  primary_sets_[hit_set_id].is_programmed = true;
  // For load balancing, the chunk(x)-th element needs to be preserved
  primary_sets_[hit_set_id].programmed_point = x;
  local_backup_set_groups_[chunk_id].consumed++;

  return original_value;
}

std::vector<DBEntry> QueryServiceClient::OnlineMultipleQueries(
    const std::vector<uint64_t>& queries) {
  std::vector<DBEntry> results;
  results.reserve(queries.size());

  for (const auto& x : queries) {
    DBEntry result = OnlineSingleQuery(x);
    results.push_back(result);
  }

  return results;
}

}  // namespace pir::piano