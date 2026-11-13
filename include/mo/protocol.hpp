#pragma once
#include <cstdint>
#include <variant>
#include <vector>
namespace mo {
using Bytes = std::vector<uint8_t>;
enum class Error {
  Incomplete,
  BadCrc,
  BadLength,
  UnknownFunction,
  BadProtocol,
  InvalidArgument,
  Io,
  Closed,
  Timeout,
  Exhausted
};
template <class T> using Result = std::variant<T, Error>;
enum class Framing { Tcp, Rtu };
struct Request {
  uint8_t unit = 1, function = 3;
  uint16_t address = 0, count = 1;
  std::vector<uint16_t> values;
};
struct Response {
  uint16_t transaction = 0;
  uint8_t unit = 1, function = 3, exception = 0;
  uint16_t address = 0, count = 0;
  std::vector<uint16_t> values;
};
uint16_t crc16(const Bytes &bytes);
Result<Bytes> encodeRequest(const Request &, Framing, uint16_t transaction = 0);
Result<Bytes> encodeResponse(const Response &, Framing);
Result<Response> decodeResponse(const Bytes &, Framing);
class StreamDecoder {
public:
  explicit StreamDecoder(Framing framing) : framing_(framing) {}
  std::vector<Result<Response>> feed(const Bytes &);

private:
  Framing framing_;
  Bytes buffer_;
};
} // namespace mo
