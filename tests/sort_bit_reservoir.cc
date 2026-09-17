/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks sort bit reader values, failure offsets and protected input tails.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */

#include <everett/sort_codec.h>

#include <exception>
#include <array>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

namespace {
  using namespace everett;
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();

  void require(bool condition, std::string const & message) {
    if (!condition) throw std::runtime_error(message);
  }

  // Keep the oracle on the original, stateless profile decoders. In particular,
  // a failed count can consume some input even though a failed field cannot.
  struct baseline_reader {
    bit_view data;
    std::uint64_t at = 0;
    std::uint64_t position() const { return at; }
    std::uint64_t remaining() const { return data.size() - at; }
    bool empty() const { return remaining() == 0; }
    std::uint64_t read_bits(unsigned width) { return profile_detail::read_fixed(data, at, width); }
    bit_view take_bits(std::uint64_t count) {
      if (count > remaining()) throw std::invalid_argument("truncated sort payload");
      auto result = data.subview(at, count);
      at += count;
      return result;
    }
    template <class Code> std::uint64_t read_count() {
      return profile_detail::read_backspace<sort_codec_detail::count_policy<Code>>(data, at);
    }
  };

  template <class Reader> void skip(Reader & in, std::uint64_t count) {
    if constexpr (requires { in.skip_bits(count); }) in.skip_bits(count);
    else (void)in.take_bits(count);
  }

  template <class T> struct outcome {
    std::optional<T> value;
    std::type_index exception = typeid(void);
    std::string message;
  };

  template <class F> auto observe(F && action) {
    outcome<decltype(action())> result;
    try { result.value.emplace(action()); }
    catch (std::exception const & error) {
      result.exception = typeid(error);
      result.message = error.what();
    }
    return result;
  }

  bool equal(std::uint64_t a, std::uint64_t b) { return a == b; }
  bool equal(bit_view a, bit_view b) {
    if (a.size() != b.size() || a.offset() != b.offset() ||
        a.storage().data() != b.storage().data() || a.storage().size() != b.storage().size()) return false;
    // Check borrowed payload bits individually, independently of load_bits.
    for (std::uint64_t bit = 0; bit != a.size(); ++bit) if (a.at(bit) != b.at(bit)) return false;
    return true;
  }

  struct differential_reader {
    sort_bit_reader actual;
    baseline_reader expected;
    std::string context;

    explicit differential_reader(bit_view data, std::string description)
      : actual(data), expected{data}, context(std::move(description)) { state(); }

    void state() const {
      require(actual.position() == expected.position(), context + ": position");
      require(actual.remaining() == expected.remaining(), context + ": remaining");
      require(actual.empty() == expected.empty(), context + ": empty");
    }

    template <class A, class E> auto compare(A && action, E && reference) {
      auto wanted = observe(reference);
      auto found = observe(action);
      require(found.exception == wanted.exception, context + ": exception type");
      require(found.message == wanted.message, context + ": exception message");
      require(found.value.has_value() == wanted.value.has_value(), context + ": success");
      if (found.value) require(equal(*found.value, *wanted.value), context + ": return value");
      state();
      return found;
    }

    auto read_bits(unsigned width) {
      return compare([&] { return actual.read_bits(width); }, [&] { return expected.read_bits(width); });
    }
    auto take_bits(std::uint64_t count) {
      return compare([&] { return actual.take_bits(count); }, [&] { return expected.take_bits(count); });
    }
    void skip_bits(std::uint64_t count) {
      compare([&] { skip(actual, count); return std::uint64_t{}; },
        [&] { skip(expected, count); return std::uint64_t{}; });
    }
    template <class Code> auto read_count() {
      return compare([&] { return actual.template read_count<Code>(); },
        [&] { return expected.template read_count<Code>(); });
    }
    void drain() {
      while (!actual.empty()) read_bits(unsigned(std::min<std::uint64_t>(13, actual.remaining())));
      read_bits(0);
      take_bits(0);
      skip_bits(0);
    }
  };

  std::string label(char const * name, std::uint64_t a, std::uint64_t b, std::uint64_t c = 0) {
    return std::string(name) + " " + std::to_string(a) + "/" + std::to_string(b) + "/" + std::to_string(c);
  }

  void append_noise(sort_bit_writer & out, std::uint64_t count, std::mt19937_64 & random) {
    while (count) {
      auto width = unsigned(std::min<std::uint64_t>(count, 64));
      out.write_bits(random() & profile_detail::low_mask(width), width);
      count -= width;
    }
  }

  template <class F> void count_codes(F && f) {
    f.template operator()<exponential_golomb<0>>();
    f.template operator()<exponential_golomb<1>>();
    f.template operator()<exponential_golomb<3>>();
    f.template operator()<exponential_golomb<63>>();
    f.template operator()<golomb<1>>();
    f.template operator()<golomb<3>>();
    f.template operator()<golomb<7>>();
    f.template operator()<golomb<maximum>>();
  }

  template <class Code> std::string code_name() {
    using policy = sort_codec_detail::count_policy<Code>;
    auto name = policy::backspace_code == bit_backspace_code::exponential_golomb ? "exp" : "golomb";
    return std::string(name) + std::to_string(policy::backspace_parameter);
  }

  void fixed_fields() {
    std::mt19937_64 random(0x413510b5u);
    std::array<std::byte, 32> data;
    for (auto & byte : data) byte = std::byte(random());
    for (unsigned offset = 0; offset != 80; ++offset) {
      for (unsigned width = 0; width != 66; ++width) {
        for (auto length : {0u, width ? width - 1 : 0u, width, width + 1, 128u}) {
          differential_reader in(bit_view(data, length, offset), label("field", offset, width, length));
          in.read_bits(width);
          in.drain();
        }
      }
    }

    // Warm a cache, then take or skip across either side of each word boundary.
    for (unsigned offset = 0; offset != 16; ++offset) {
      for (unsigned first = 0; first != 65; ++first) {
        for (auto count : {0u, 1u, 7u, 8u, 31u, 63u, 64u, 65u, 127u, 128u}) {
          differential_reader in(bit_view(data, 240, offset), label("take/skip", offset, first, count));
          in.read_bits(first);
          in.take_bits(count);
          in.skip_bits(65);
          in.read_bits(65);
          in.take_bits(maximum);
          in.skip_bits(maximum);
          in.drain();
        }
      }
    }
  }

  template <class Code> std::vector<std::uint64_t> valid_values() {
    using policy = sort_codec_detail::count_policy<Code>;
    std::vector<std::uint64_t> values{0, 1, 2, 3, 7, 8, 15, 16, 31, 63, 64, 65, 127};
    if constexpr (policy::backspace_code == bit_backspace_code::exponential_golomb) {
      for (unsigned bit = 8; bit != 64; ++bit) {
        auto value = std::uint64_t{1} << bit;
        values.push_back(value - 1);
        values.push_back(value);
        values.push_back(value + 1);
      }
      values.push_back(maximum - 1);
      values.push_back(maximum);
    } else {
      constexpr auto modulus = policy::backspace_parameter;
      constexpr auto cutoff = profile_detail::golomb_cutoff<policy>();
      for (std::uint64_t quotient : {0u, 1u, 2u, 7u, 63u, 64u, 65u, 127u}) {
        if (quotient > maximum / modulus) continue;
        for (auto remainder : {std::uint64_t{0}, modulus - 1, cutoff ? cutoff - 1 : 0, cutoff}) {
          if (remainder < modulus && remainder <= maximum - quotient * modulus)
            values.push_back(quotient * modulus + remainder);
        }
      }
    }
    return values;
  }

  template <class Code> void valid_counts() {
    std::mt19937_64 random(0x55b0b39u);
    for (auto value : valid_values<Code>()) {
      for (unsigned offset = 0; offset != 8; ++offset) {
        auto warmup = unsigned((value % 71 + offset * 13) % 71);
        bit_string data;
        sort_bit_writer out(data);
        append_noise(out, offset + warmup, random);
        auto start = out.position();
        out.template write_count<Code>(value);
        auto width = out.position() - start;
        if constexpr (std::is_same_v<Code, exponential_golomb<0>>) {
          if (value == maximum) require(width == 129, "maximum count must occupy 129 bits");
        }
        append_noise(out, 73, random);
        auto view = data.view().subview(offset, data.bit_size - offset);
        // Every proper prefix is tested, including a missing unary terminator,
        // suffix truncation and a truncated fixed parameter remainder.
        for (std::uint64_t length = 0; length <= width; ++length) {
          differential_reader in(view.prefix(warmup + length),
            code_name<Code>() + " " + label("prefix", value, offset, length));
          while (in.actual.position() < warmup)
            in.read_bits(unsigned(std::min<std::uint64_t>(7, warmup - in.actual.position())));
          auto result = in.template read_count<Code>();
          require(result.value.has_value() == (length == width), in.context + ": complete code extent");
          if (result.value) require(*result.value == value, in.context + ": encoded count");
          in.drain();
        }
        differential_reader in(view, code_name<Code>() + " " + label("suffix", value, offset));
        in.read_bits(unsigned(std::min(warmup, 1u)));
        in.skip_bits(warmup ? warmup - 1 : 0);
        auto result = in.template read_count<Code>();
        require(result.value && *result.value == value, in.context + ": encoded count");
        in.drain();
      }
    }
  }

  void malformed_counts() {
    // A 65th unary zero overflows; a 64-zero prefix with a nonzero suffix
    // overflows after all 129 bits. Continue reading after each failure.
    std::vector<bit_string> cases;
    for (unsigned size = 0; size != 141; ++size) cases.push_back(bit_string::from_bits(std::string(size, '0')));
    cases.push_back(bit_string::from_bits(std::string(64, '0') + "1" + std::string(63, '0') + "1"));
    cases.push_back(bit_string::from_bits(std::string(64, '0') + "1" + std::string(64, '1')));
    cases.push_back(bit_string::from_bits("001")); // Golomb<UINT64_MAX> quotient overflow.
    cases.push_back(bit_string::from_bits("01" + std::string(62, '0') + "10")); // Its final sum overflows.
    for (auto const & data : cases) count_codes([&]<class Code> {
      for (unsigned offset = 0; offset != 8; ++offset) {
        bit_string framed;
        sort_bit_writer out(framed);
        out.write_bits(profile_detail::low_mask(offset), offset);
        out.append(data.view());
        differential_reader in(framed.view().subview(offset, data.bit_size),
          code_name<Code>() + " " + label("malformed", data.bit_size, offset));
        in.template read_count<Code>();
        in.read_bits(0);
        in.template read_count<Code>();
        in.drain();
      }
    });

    // A valid exponential quotient may still overflow the parameterized code.
    count_codes([&]<class Code> {
      using policy = sort_codec_detail::count_policy<Code>;
      if constexpr (policy::backspace_code == bit_backspace_code::exponential_golomb &&
          policy::backspace_parameter != 0) {
        bit_string data;
        sort_bit_writer out(data);
        out.write_count((maximum >> policy::backspace_parameter) + 1);
        out.write_bits(0, unsigned(policy::backspace_parameter));
        out.write_bits(0xabcdef, 24);
        differential_reader in(data.view(), code_name<Code>() + " quotient overflow");
        auto result = in.template read_count<Code>();
        require(!result.value, in.context + ": expected overflow");
        in.drain();
      }
    });
  }

  void random_operations() {
    std::mt19937_64 random(0x9f06a13539ull);
    for (unsigned trial = 0; trial != 2400; ++trial) {
      auto offset = random() % 80;
      auto length = random() % 769;
      std::vector<std::byte> bytes((offset + length + 7) / 8);
      for (auto & byte : bytes) byte = std::byte(random());
      differential_reader in(bit_view(bytes, length, offset), label("random", trial, offset, length));
      for (unsigned operation = 0; operation != 48; ++operation) {
        switch (random() % 13) {
          case 0: case 1: case 2: in.read_bits(unsigned(random() % 66)); break;
          case 3: in.take_bits(random() % 161); break;
          case 4: in.skip_bits(random() % 161); break;
          case 5: in.read_count<exponential_golomb<0>>(); break;
          case 6: in.read_count<exponential_golomb<1>>(); break;
          case 7: in.read_count<exponential_golomb<3>>(); break;
          case 8: in.read_count<exponential_golomb<63>>(); break;
          case 9: in.read_count<golomb<1>>(); break;
          case 10: in.read_count<golomb<3>>(); break;
          case 11: in.read_count<golomb<7>>(); break;
          default: in.read_count<golomb<maximum>>(); break;
        }
      }
      in.drain();
    }
  }

  struct guarded_page {
    std::size_t page;
    std::byte * base;
    guarded_page() : page(static_cast<std::size_t>(::sysconf(_SC_PAGESIZE))), base(nullptr) {
      require(page != 0 && page != static_cast<std::size_t>(-1), "page size");
      auto mapping = ::mmap(nullptr, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
      require(mapping != MAP_FAILED, "guard mmap");
      base = static_cast<std::byte *>(mapping);
    }
    guarded_page(guarded_page const &) = delete;
    guarded_page & operator=(guarded_page const &) = delete;
    ~guarded_page() { if (::munmap(base, page * 3)) std::terminate(); }
    std::byte * end() const { return base + page * 2; }
    void fill(unsigned pattern) {
      require(::mprotect(base + page, page, PROT_READ | PROT_WRITE) == 0, "guard writable fixture");
      std::mt19937_64 random(0xc512efa9u);
      for (std::size_t i = 0; i != page; ++i)
        base[page + i] = std::byte(pattern == 0 ? 0 : pattern == 1 ? 255 : unsigned(random()));
      require(::mprotect(base + page, page, PROT_READ) == 0, "guard read-only fixture");
    }
  };

  void guarded_tails() {
    guarded_page memory;
    // Empty views may point inside an inaccessible mapping, including a
    // nonempty backing span: no zero-width operation may load a byte.
    for (unsigned offset = 0; offset != 9; ++offset) {
      differential_reader in(bit_view(std::span<std::byte const>(memory.end(), 1), 0, offset),
        label("inaccessible empty", offset, 0));
      in.drain();
      in.read_bits(1);
      in.read_bits(65);
      in.take_bits(1);
      in.skip_bits(1);
      count_codes([&]<class Code> { in.template read_count<Code>(); });
    }
    differential_reader empty(bit_view(std::span<std::byte const>(memory.end(), 0), 0), "inaccessible zero span");
    empty.drain();
    empty.read_bits(1);
    empty.take_bits(1);
    empty.skip_bits(1);
    count_codes([&]<class Code> { empty.template read_count<Code>(); });

    // Skipping a payload only moves its bit address, even if the payload has
    // inaccessible backing pages. Borrowing a view must be equally lazy.
    bit_view hidden(std::span<std::byte const>(memory.end(), memory.page), 1024, 3);
    sort_bit_reader lazy(hidden);
    require(lazy.read_bits(0) == 0, "inaccessible zero-width field");
    skip(lazy, 129);
    auto borrowed = lazy.take_bits(257);
    require(borrowed.size() == 257 && borrowed.offset() == 132 &&
      borrowed.storage().data() == hidden.storage().data(), "inaccessible borrowed view");
    skip(lazy, 1024 - 386);
    require(lazy.empty() && lazy.position() == 1024, "inaccessible payload skip");

    for (unsigned pattern = 0; pattern != 3; ++pattern) {
      memory.fill(pattern);
      // The last available byte borders PROT_NONE. Vary every address residue,
      // initial bit offset and number of unused bits in that final byte.
      for (unsigned bytes = 1; bytes != 41; ++bytes) {
        auto storage = std::span<std::byte const>(memory.end() - bytes, bytes);
        for (unsigned offset = 0; offset != 8; ++offset) {
          for (unsigned tail = 0; tail != 8; ++tail) {
            if (offset + tail > bytes * 8) continue;
            bit_view data(storage, bytes * 8 - offset - tail, offset);
            auto context = label("guard", bytes, offset, tail) + " pattern " + std::to_string(pattern);
            for (unsigned width = 0; width != 66; ++width) {
              differential_reader in(data, context + " width " + std::to_string(width));
              in.read_bits(width);
              in.skip_bits(std::min<std::uint64_t>(63, in.actual.remaining()));
              in.take_bits(std::min<std::uint64_t>(65, in.actual.remaining()));
              in.drain();
            }
            count_codes([&]<class Code> {
              differential_reader in(data, context + " " + code_name<Code>());
              in.template read_count<Code>();
              in.read_bits(7);
              in.template read_count<Code>();
              in.drain();
            });
          }
        }
      }
    }
  }
}

int main() {
  try {
    fixed_fields();
    count_codes([]<class Code> { valid_counts<Code>(); });
    malformed_counts();
    random_operations();
    guarded_tails();
    std::cout << "sort bit reservoir: differential values, failures, mixtures and guarded tails passed\n";
  } catch (std::exception const & error) {
    std::cerr << "sort bit reservoir: " << error.what() << '\n';
    return 1;
  }
}
