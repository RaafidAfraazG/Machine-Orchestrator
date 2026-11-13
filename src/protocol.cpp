#include "mo/protocol.hpp"
#include <algorithm>
namespace mo {
namespace {
void put(Bytes &b, uint16_t v) {
  b.push_back(v >> 8);
  b.push_back(v & 255);
}
uint16_t word(const Bytes &b, size_t i) {
  return static_cast<uint16_t>((b[i] << 8) | b[i + 1]);
}
bool known(uint8_t f) { return f == 3 || f == 6 || f == 16; }
Bytes frame(Bytes p, uint8_t unit, Framing f, uint16_t id) {
  Bytes b;
  if (f == Framing::Tcp) {
    put(b, id);
    put(b, 0);
    put(b, static_cast<uint16_t>(p.size() + 1));
  }
  b.push_back(unit);
  b.insert(b.end(), p.begin(), p.end());
  if (f == Framing::Rtu) {
    auto c = crc16(b);
    b.push_back(c & 255);
    b.push_back(c >> 8);
  }
  return b;
}
} // namespace
uint16_t crc16(const Bytes &b) {
  uint16_t c = 0xffff;
  for (auto v : b) {
    c ^= v;
    for (int j = 0; j < 8; ++j)
      c = (c & 1) ? (c >> 1) ^ 0xa001 : c >> 1;
  }
  return c;
}
Result<Bytes> encodeRequest(const Request &r, Framing f, uint16_t id) {
  if (!known(r.function))
    return Error::UnknownFunction;
  if (r.unit > 247 || (r.function == 3 && (r.count == 0 || r.count > 125)) ||
      (r.function == 6 && r.values.size() != 1) ||
      (r.function == 16 && (r.values.empty() || r.values.size() > 123 ||
                            r.count != r.values.size())))
    return Error::InvalidArgument;
  auto n = r.function == 3 ? r.count : r.function == 6 ? 1 : r.values.size();
  if (static_cast<uint32_t>(r.address) + n > 65536)
    return Error::InvalidArgument;
  Bytes p{r.function};
  put(p, r.address);
  if (r.function == 6)
    put(p, r.values[0]);
  else {
    put(p, r.count);
    if (r.function == 16) {
      p.push_back(static_cast<uint8_t>(r.values.size() * 2));
      for (auto v : r.values)
        put(p, v);
    }
  }
  return frame(p, r.unit, f, id);
}
Result<Bytes> encodeResponse(const Response &r, Framing f) {
  if (!known(r.function))
    return Error::UnknownFunction;
  Bytes p{static_cast<uint8_t>(r.function | (r.exception ? 0x80 : 0))};
  if (r.exception)
    p.push_back(r.exception);
  else if (r.function == 3) {
    if (r.values.empty() || r.values.size() > 125)
      return Error::InvalidArgument;
    p.push_back(static_cast<uint8_t>(r.values.size() * 2));
    for (auto v : r.values)
      put(p, v);
  } else {
    put(p, r.address);
    if (r.function == 6) {
      if (r.values.size() != 1)
        return Error::InvalidArgument;
      put(p, r.values[0]);
    } else {
      if (r.count == 0 || r.count > 123)
        return Error::InvalidArgument;
      put(p, r.count);
    }
  }
  return frame(p, r.unit, f, r.transaction);
}
Result<Response> decodeResponse(const Bytes &b, Framing f) {
  size_t start = f == Framing::Tcp ? 7 : 1, end;
  if (b.size() < (f == Framing::Tcp ? 8u : 4u))
    return Error::Incomplete;
  if (f == Framing::Tcp) {
    if (word(b, 2) != 0)
      return Error::BadProtocol;
    auto len = word(b, 4);
    if (len < 3 || len > 254)
      return Error::BadLength;
    end = 6 + len;
    if (b.size() < end)
      return Error::Incomplete;
    if (b.size() != end)
      return Error::BadLength;
  } else {
    end = b.size() - 2;
    Bytes body(b.begin(), b.end() - 2);
    if (crc16(body) != static_cast<uint16_t>(b[end] | (b[end + 1] << 8)))
      return Error::BadCrc;
  }
  Response r;
  r.transaction = f == Framing::Tcp ? word(b, 0) : 0;
  r.unit = b[start - 1];
  r.function = b[start] & 0x7f;
  if (!known(r.function))
    return Error::UnknownFunction;
  if (b[start] & 0x80) {
    if (end - start != 2)
      return Error::BadLength;
    r.exception = b[start + 1];
    return r;
  }
  if (r.function == 3) {
    if (end - start < 2)
      return Error::BadLength;
    auto n = b[start + 1];
    if (!n || n % 2 || n > 250 || end - start != static_cast<size_t>(n) + 2)
      return Error::BadLength;
    for (size_t i = start + 2; i < end; i += 2)
      r.values.push_back(word(b, i));
    r.count = static_cast<uint16_t>(r.values.size());
  } else {
    if (end - start != 5)
      return Error::BadLength;
    r.address = word(b, start + 1);
    auto v = word(b, start + 3);
    if (r.function == 6) {
      r.values.push_back(v);
      r.count = 1;
    } else {
      if (!v || v > 123)
        return Error::BadLength;
      r.count = v;
    }
  }
  return r;
}
std::vector<Result<Response>> StreamDecoder::feed(const Bytes &chunk) {
  buffer_.insert(buffer_.end(), chunk.begin(), chunk.end());
  std::vector<Result<Response>> out;
  while (!buffer_.empty()) {
    size_t n = 0;
    if (framing_ == Framing::Tcp) {
      if (buffer_.size() < 6)
        break;
      auto len = word(buffer_, 4);
      if (len < 3 || len > 254) {
        out.emplace_back(Error::BadLength);
        buffer_.clear();
        break;
      }
      n = 6 + len;
    } else {
      if (buffer_.size() < 2)
        break;
      auto f = buffer_[1];
      if (!known(f & 0x7f)) {
        out.emplace_back(Error::UnknownFunction);
        buffer_.clear();
        break;
      }
      if (f & 0x80)
        n = 5;
      else if (f == 3) {
        if (buffer_.size() < 3)
          break;
        n = 5 + buffer_[2];
      } else
        n = 8;
    }
    if (buffer_.size() < n)
      break;
    Bytes b(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(n));
    buffer_.erase(buffer_.begin(),
                  buffer_.begin() + static_cast<std::ptrdiff_t>(n));
    out.push_back(decodeResponse(b, framing_));
  }
  return out;
}
} // namespace mo
