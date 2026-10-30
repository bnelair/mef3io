// mef3io — exception hierarchy. Public API throws these; the (future) C ABI
// shim converts them to error codes for MATLAB.
#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mef3io {

class MefError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Structural violation: bad magic, wrong size, misaligned field.
class FormatError : public MefError {
 public:
  using MefError::MefError;
};

// CRC mismatch on a header or body.
class CrcError : public MefError {
 public:
  using MefError::MefError;
};

// Missing/invalid password, or insufficient access level for encrypted data.
class PasswordError : public MefError {
 public:
  using MefError::MefError;
};

// Filesystem / IO failure.
class IoError : public MefError {
 public:
  using MefError::MefError;
};

// A multi-channel write that failed while publishing: the channels in
// `written()` ARE on disk and valid, every other channel of the call was
// rolled back (SessionWriter::write_int32_many). A failure earlier in the
// call throws the underlying error instead, and nothing was written.
class PartialWriteError : public IoError {
 public:
  PartialWriteError(const std::string& what, std::vector<std::string> written)
      : IoError(what), written_(std::move(written)) {}
  const std::vector<std::string>& written() const { return written_; }

 private:
  std::vector<std::string> written_;
};

// Illegal write (e.g. start before existing end, fs/ufact mismatch on append).
class WriteConflictError : public MefError {
 public:
  using MefError::MefError;
};

}  // namespace mef3io
