#pragma once
// Deterministic RNG (splitmix64 seeding + xoshiro256**). Simulation results
// must be reproducible from (world seed, entity id, date) so that far-LOD
// state can be re-derived instead of stored.

#include <cmath>
#include <cstdint>

namespace rj::sim {

inline uint64_t splitmix64(uint64_t& x) {
  uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

inline uint64_t hashCombine(uint64_t a, uint64_t b) {
  uint64_t x = a ^ (b + 0x9E3779B97F4A7C15ULL + (a << 6) + (a >> 2));
  return splitmix64(x);
}

class Rng {
 public:
  explicit Rng(uint64_t seed) {
    for (auto& v : s_) v = splitmix64(seed);
  }
  uint64_t next() {
    const uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
  }
  double uniform() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }
  double uniform(double lo, double hi) { return lo + (hi - lo) * uniform(); }
  int range(int lo, int hi_inclusive) {
    return lo + static_cast<int>(next() % static_cast<uint64_t>(hi_inclusive - lo + 1));
  }
  bool chance(double p) { return uniform() < p; }
  double normal(double mean, double sd) {
    // Box-Muller.
    double u1 = uniform();
    if (u1 < 1e-300) u1 = 1e-300;
    const double u2 = uniform();
    return mean + sd * std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }

 private:
  static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
  uint64_t s_[4];
};

}  // namespace rj::sim
