#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>

namespace recorder {

template <typename T>
class SpscRingBuffer {
 public:
  explicit SpscRingBuffer(size_t capacity)
      : _capacity(next_power_of_2(capacity)),
        _mask(_capacity - 1),
        _data(std::make_unique<T[]>(_capacity)) {}

  SpscRingBuffer(const SpscRingBuffer&) = delete;
  SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

  size_t capacity() const noexcept { return _capacity; }
  size_t read_available() const noexcept { return _write_pos.load() - _read_pos.load(); }
  size_t write_available() const noexcept { return _capacity - read_available(); }

  size_t write(const T* data, size_t count) noexcept {
    const size_t wp = _write_pos.load();
    const size_t rp = _read_pos.load();
    const size_t available = _capacity - (wp - rp);
    const size_t n = std::min(available, count);
    if (n == 0) return 0;

    const size_t idx = wp & _mask;
    const size_t first = std::min(n, _capacity - idx);
    const size_t second = n - first;

    std::memcpy(&_data[idx], data, first * sizeof(T));
    if (second > 0) {
      std::memcpy(&_data[0], data + first, second * sizeof(T));
    }

    _write_pos.store(wp + n);
    return n;
  }

  size_t read(T* data, size_t count) noexcept {
    const size_t rp = _read_pos.load();
    const size_t wp = _write_pos.load();
    const size_t available = wp - rp;
    const size_t n = std::min(available, count);
    if (n == 0) return 0;

    const size_t idx = rp & _mask;
    const size_t first = std::min(n, _capacity - idx);
    const size_t second = n - first;

    std::memcpy(data, &_data[idx], first * sizeof(T));
    if (second > 0) {
      std::memcpy(data + first, &_data[0], second * sizeof(T));
    }

    _read_pos.store(rp + n);
    return n;
  }

 private:
  static size_t next_power_of_2(size_t n) noexcept {
    if (n == 0) return 1;
    --n;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    if constexpr (sizeof(size_t) > 4) {
      n |= n >> 32;
    }
    return n + 1;
  }

  const size_t _capacity;
  const size_t _mask;
  std::unique_ptr<T[]> _data;

  alignas(64) std::atomic<size_t> _write_pos{0};
  alignas(64) std::atomic<size_t> _read_pos{0};
};

}  // namespace recorder
