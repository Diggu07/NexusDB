// NexusDB — database engine error handling (Digvijay: Database Engine).
//
// Explicit Status returns; no exceptions cross the DB storage API.
// Mirrors the spirit of the C container layer (nexus_rc_t codes) in C++.

#ifndef NEXUSDB_COMMON_ERROR_H
#define NEXUSDB_COMMON_ERROR_H

#include <string>
#include <utility>

namespace nexusdb {

// Database engine error codes.
enum class DbError {
  kOk = 0,
  kInvalidArgument,  // bad path, null output pointer, invalid page id
  kNotFound,         // page id beyond the end of the database file
  kIoError,          // open/seek/read/write/flush failure, short I/O
  kBadState,         // storage closed or never successfully opened
};

// Lightweight result type returned by fallible DB operations.
struct Status {
  DbError code = DbError::kOk;
  std::string message;

  bool ok() const { return code == DbError::kOk; }

  static Status Ok() { return Status{}; }

  static Status Error(DbError code, std::string message) {
    Status s;
    s.code = code;
    s.message = std::move(message);
    return s;
  }
};

inline const char* DbErrorString(DbError code) {
  switch (code) {
    case DbError::kOk:
      return "ok";
    case DbError::kInvalidArgument:
      return "invalid argument";
    case DbError::kNotFound:
      return "not found";
    case DbError::kIoError:
      return "I/O error";
    case DbError::kBadState:
      return "bad state";
    default:
      return "unknown error";
  }
}

}  // namespace nexusdb

#endif  // NEXUSDB_COMMON_ERROR_H
