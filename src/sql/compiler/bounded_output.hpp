#pragma once
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <streambuf>
#include <string>

namespace navidog::sql::compiler {
// Shared by model and procedural reports. The legacy MSVCRT streambuf ABI
// needs explicit seek overrides; no seeks are supported by this append sink.
class BoundedOutputBuffer final : public std::streambuf {
  std::size_t maximum_, sincePoll_ = 0;
  std::function<bool()> stop_;
  std::string error_;
  std::streamsize xsputn(const char *data, std::streamsize length) override {
    if (length < 0 || std::size_t(length) > remaining())
      throw std::length_error(error_);
    sincePoll_ += std::size_t(length);
    if (sincePoll_ >= 4096) {
      sincePoll_ = 0;
      if (stop_ && stop_())
        throw std::runtime_error("model serialization stopped");
    }
    value.append(data, std::size_t(length));
    return length;
  }
  pos_type seekoff(off_type, std::ios_base::seekdir,
                   std::ios_base::openmode) override {
    return pos_type(off_type(-1));
  }
  pos_type seekpos(pos_type, std::ios_base::openmode) override {
    return pos_type(off_type(-1));
  }
  int_type overflow(int_type ch) override {
    if (traits_type::eq_int_type(ch, traits_type::eof()))
      return traits_type::not_eof(ch);
    const char byte = traits_type::to_char_type(ch);
    xsputn(&byte, 1);
    return ch;
  }

public:
  std::string value;
  explicit BoundedOutputBuffer(
      std::size_t maximum, std::function<bool()> stop = {},
      std::string error = "model JSON size limit exceeded")
      : maximum_(maximum), stop_(std::move(stop)), error_(std::move(error)) {
    value.reserve(std::min<std::size_t>(maximum, 4096));
  }
  std::size_t remaining() const { return maximum_ - value.size(); }
};
} // namespace navidog::sql::compiler
