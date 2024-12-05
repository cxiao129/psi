// Copyright 2024 The secretflow authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <spdlog/spdlog.h>

#include <cmath>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "yacl/base/dynamic_bitset.h"
#include "yacl/crypto/rand/rand.h"
#include "yacl/crypto/tools/prg.h"

#pragma once

namespace pir::pps {

#define PIR_OK 1
#define PIR_ABORT 0xFFFFFFFFFFFFFFFF

#define MODULE_ADD(a, b, m) \
  (((a) + (b) >= (m)) ? ((a) + (b) - (m)) : ((a) + (b)))
#define MODULE_SUB(a, b, m) (((a) >= (b)) ? ((a) - (b)) : ((a) + (m) - (b)))

using PIRKey = uint128_t;
using PIREvalMap = std::unordered_map<uint64_t, uint64_t>;

struct PIRPuncKey {
  std::vector<uint128_t> k_;
  uint64_t pos_;
  uint64_t delta_;

  PIRPuncKey() : k_(), pos_(0), delta_(0) {}
};

struct PIRKeyUnion {
  PIRKey k_;
  uint64_t delta_;

  PIRKeyUnion() : k_(0), delta_(0) {}

  PIRKeyUnion(PIRKey k) : k_(k), delta_(0) {}

  PIRKeyUnion(PIRKey k, uint64_t delta) : k_(k), delta_(delta) {}

  bool operator<(const PIRKeyUnion& other) const { return k_ < other.k_; }
};

// For the Node PRG value m, get the random number \in [m]
inline uint64_t LemireTrick(uint128_t v, uint64_t m) {
  return static_cast<uint64_t>(((v >> 64) * static_cast<uint128_t>(m)) >> 64);
}

inline uint64_t LemireTrick(uint64_t v, uint64_t m) {
  return static_cast<uint64_t>(
      (static_cast<uint128_t>(v) * static_cast<uint128_t>(m)) >> 64);
}

class GGMTree {
 public:
  GGMTree() {}
  GGMTree(uint128_t root, uint64_t height) : root_(root), height_(height) {}

  uint128_t GetRoot() { return root_; }
  /***************************************************************
   * Basic length-doubling PRG: {0, 1}^s -> {0, 1}^{2s}
   * Use "yacl/crypto/tools/prg.h", so s = 128
   ***************************************************************/
  void static AESPRG(uint128_t seed, uint128_t& left, uint128_t& right);

  /****************************************************************
   * Construct tree-based PRF of Goldreich, Goldwasser, and Micali.
   * How to construct random functions. J. ACM, 33(4):792–807, 1986.
   * Define G_0(x) || G_1(x) = PRG(x), G_0, G_1 to be the left and right
   * halves of G.
   * For input bits x_0, x_1, x_2, ..., x_n and random seed K:
   * PRF_K = G_{x_n}(G_{x_{n-1}}(...(G_x_0(K))))
   ****************************************************************/
  void Gen(std::vector<uint128_t>& leaf_nodes);

 private:
  uint128_t root_;
  uint32_t height_;
};

// The depth
inline uint32_t Depth(uint64_t size) {
  return static_cast<uint32_t>(std::ceil(std::log2(size)));
}

class PPS {
 public:
  PPS() : universe_size_(0), set_size_(0) {}

  PPS(uint64_t universe_size, uint64_t set_size)
      : universe_size_(universe_size), set_size_(set_size) {
    if (set_size * set_size < universe_size / 2 ||
        set_size * set_size > 2 * universe_size) {
      SPDLOG_ERROR("The set size must in [sqrt(n / 2), sqrt(2 * n)]");
      abort();
    }
  }

  // Generate a uint128_t k.
  PIRKey Gen(uint32_t lambda);

  // Find l that PRFEval(k, l) = i
  void Punc(uint64_t i, PIRKey k, PIRPuncKey& sk_punc);

  void EvalMap(PIRKey k);

  void Eval(PIRKey k, std::set<uint64_t>& set);

  void Eval(PIRKey k, std::unordered_set<uint64_t>& set);

  void Eval(const PIRPuncKey& sk_punc, std::set<uint64_t>& set);

  const PIREvalMap& getMap() const { return map_; }

 private:
  PIREvalMap map_;
  uint64_t universe_size_;
  uint64_t set_size_;
};

}  // namespace pir::pps
