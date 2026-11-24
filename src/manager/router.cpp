// NexusDB — command Router implementation (Digvijay: Manager/Router, R3).
#include "manager/router.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <utility>

#include "container/container.h"

namespace nexusdb {
namespace cli {

int ExitCodeFor(DbError code) {
  switch (code) {
    case DbError::kOk:
      return kOk;
    case DbError::kInvalidArgument:
      return kInvalid;
    case DbError::kNotFound:
      return kNotFound;
    case DbError::kIoError:
      return kIoError;
    case DbError::kBadState:
      return kBadState;
    case DbError::kBufferFull:
      return kBufferFull;
    case DbError::kOutOfMemory:
      return kOutOfMemory;
  }
  return kError;
}

namespace {

int HexVal(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

bool HexDecode(const std::string& text, std::vector<std::uint8_t>* out) {
  if (text.size() % 2 != 0)
    return false;
  out->clear();
  out->reserve(text.size() / 2);
  for (std::size_t i = 0; i < text.size(); i += 2) {
    const int hi = HexVal(text[i]);
    const int lo = HexVal(text[i + 1]);
    if (hi < 0 || lo < 0)
      return false;
    out->push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }
  return true;
}

std::string HexEncode(const std::uint8_t* data, std::size_t len) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (std::size_t i = 0; i < len; ++i) {
    out.push_back(kHex[(data[i] >> 4) & 0xF]);
    out.push_back(kHex[data[i] & 0xF]);
  }
  return out;
}

bool ParseUint(const std::string& text, std::uint64_t max, std::uint64_t* out);

bool IsPrintableUtf8(const std::uint8_t* data, std::size_t len) {
  std::size_t i = 0;
  while (i < len) {
    const std::uint8_t b0 = data[i];
    if (b0 < 0x20 || b0 == 0x7F)
      return false;
    if (b0 < 0x80) {
      ++i;
      continue;
    }

    std::size_t width = 0;
    std::uint32_t cp = 0;
    if ((b0 & 0xE0) == 0xC0) {
      if (b0 < 0xC2)
        return false;
      width = 2;
      cp = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
      width = 3;
      cp = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
      if (b0 > 0xF4)
        return false;
      width = 4;
      cp = b0 & 0x07;
    } else {
      return false;
    }
    if (i + width > len)
      return false;

    for (std::size_t k = 1; k < width; ++k) {
      const std::uint8_t b = data[i + k];
      if ((b & 0xC0) != 0x80)
        return false;
      cp = (cp << 6) | (b & 0x3F);
    }
    if ((width == 2 && cp < 0x80) || (width == 3 && cp < 0x800) ||
        (width == 4 && cp < 0x10000))
      return false;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
      return false;
    if (cp >= 0x80 && cp <= 0x9F)
      return false;
    if (cp == 0x2028 || cp == 0x2029)
      return false;
    i += width;
  }
  return true;
}

std::string QuoteScanText(const std::uint8_t* data, std::size_t len) {
  std::string out;
  out.reserve(len + 2);
  out.push_back('"');
  for (std::size_t i = 0; i < len; ++i) {
    if (data[i] == '"' || data[i] == '\\')
      out.push_back('\\');
    out.push_back(static_cast<char>(data[i]));
  }
  out.push_back('"');
  return out;
}

std::string FormatScanLine(unsigned int page, unsigned int slot,
                           std::size_t len, const std::uint8_t* data,
                           bool verbose) {
  char buf[80];
  std::snprintf(buf, sizeof(buf), "page=%u slot=%u len=%zu", page, slot, len);
  std::string out = buf;
  const bool printable = IsPrintableUtf8(data, len);
  if (printable)
    out += " text=" + QuoteScanText(data, len);
  if (verbose || !printable)
    out += " hex=" + HexEncode(data, len);
  return out;
}

bool ScanField(const std::string& line, const char* key, std::string* value) {
  const std::string prefix = std::string(key) + "=";
  const std::size_t at = line.find(prefix);
  if (at == std::string::npos)
    return false;
  const std::size_t start = at + prefix.size();
  std::size_t end = line.find(' ', start);
  if (end == std::string::npos)
    end = line.size();
  *value = line.substr(start, end - start);
  return true;
}

std::string FormatEngineScanLine(const std::string& line, bool verbose) {
  std::string page_text;
  std::string slot_text;
  std::string len_text;
  std::string hex_text;
  if (!ScanField(line, "page", &page_text) ||
      !ScanField(line, "slot", &slot_text) ||
      !ScanField(line, "len", &len_text) ||
      !ScanField(line, "hex", &hex_text))
    return line;

  std::uint64_t page = 0;
  std::uint64_t slot = 0;
  std::uint64_t len = 0;
  if (!ParseUint(page_text, std::numeric_limits<unsigned int>::max(), &page) ||
      !ParseUint(slot_text, 0xFFFF, &slot) ||
      !ParseUint(len_text, std::numeric_limits<std::size_t>::max(), &len))
    return line;

  std::vector<std::uint8_t> data;
  if (!HexDecode(hex_text, &data) || data.size() != len)
    return line;
  return FormatScanLine(static_cast<unsigned int>(page),
                        static_cast<unsigned int>(slot), data.size(),
                        data.data(), verbose);
}

std::string FormatEngineScanOutput(const std::string& body, bool verbose) {
  std::string out;
  std::size_t start = 0;
  while (start < body.size()) {
    const std::size_t end = body.find('\n', start);
    const std::string line =
        body.substr(start, end == std::string::npos ? std::string::npos
                                                    : end - start);
    out += FormatEngineScanLine(line, verbose);
    if (end == std::string::npos)
      break;
    out.push_back('\n');
    start = end + 1;
  }
  return out;
}

// Parses an unsigned decimal integer, rejecting empty input, non-digit
// characters, and any value above `max`. Uses 64-bit arithmetic with an
// explicit overflow guard so it never wraps on a 32-bit `unsigned long` (where
// `value * 10` would otherwise silently truncate and accept out-of-range page
// ids or memory limits).
bool ParseUint(const std::string& text, std::uint64_t max, std::uint64_t* out) {
  if (text.empty())
    return false;
  std::uint64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9')
      return false;
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > max / 10)
      return false;
    value *= 10;
    if (digit > max - value)
      return false;
    value += digit;
  }
  *out = value;
  return true;
}

// Largest accepted --memory limit (2^48-1 bytes, the value accepted before unit
// suffixes were added). Anything above is rejected instead of overflowing the
// runtime's arithmetic.
constexpr std::uint64_t kMaxMemoryBytes = 0xFFFFFFFFFFFFULL;

// Accepts a plain byte count or a binary-suffixed value: 512, 512B, 64K, 64KB,
// 256M, 256MB, 1G, 1GB (case-insensitive). 0 means "no limit".
#ifdef _WIN32
bool ParseMemory(const std::string& text, unsigned long long* out) {
#else
bool ParseMemory(const std::string& text, unsigned long* out) {
#endif
  if (text.empty())
    return false;
  std::size_t i = 0;
  std::uint64_t value = 0;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
    const std::uint64_t digit = static_cast<std::uint64_t>(text[i] - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
      return false;
    value = value * 10 + digit;
    ++i;
  }
  if (i == 0)
    return false;

  std::string suffix = text.substr(i);
  for (char& c : suffix)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  std::uint64_t multiplier = 1;
  if (suffix.empty() || suffix == "b") {
    multiplier = 1;
  } else if (suffix == "k" || suffix == "kb") {
    multiplier = 1024ULL;
  } else if (suffix == "m" || suffix == "mb") {
    multiplier = 1024ULL * 1024ULL;
  } else if (suffix == "g" || suffix == "gb") {
    multiplier = 1024ULL * 1024ULL * 1024ULL;
  } else {
    return false;
  }

  if (multiplier != 0 && value > kMaxMemoryBytes / multiplier)
    return false;
  value *= multiplier;
#ifndef _WIN32
  if (value > std::numeric_limits<unsigned long>::max())
    return false;
  *out = static_cast<unsigned long>(value);
#else
  *out = static_cast<unsigned long long>(value);
#endif
  return true;
}

bool IsHelpFlag(const std::string& s) { return s == "--help" || s == "-h"; }

int Usage(std::string* error, const std::string& message) {
  if (error != nullptr)
    *error = message;
  return kUsage;
}

int Invalid(std::string* error, const std::string& message) {
  if (error != nullptr)
    *error = message;
  return kInvalid;
}

int MapStatus(const Status& s, Result* result) {
  result->exit_code = ExitCodeFor(s.code);
  result->output = s.message;
  return result->exit_code;
}

// Resolves the engine launched by `create` when the user did not pass
// --engine/--no-engine: $NEXUSDB_DB_ENGINE first, then a sibling `db_engine`
// next to the CLI binary. An empty result means "use the built-in placeholder".
std::string ResolveDefaultEngine(const std::string& program_path) {
  const char* env = std::getenv("NEXUSDB_DB_ENGINE");
  if (env != nullptr && env[0] != '\0')
    return std::string(env);
  if (program_path.empty())
    return std::string();
  std::error_code ec;
  const std::filesystem::path prog(program_path);
  const std::filesystem::path dir =
      prog.has_parent_path() ? prog.parent_path() : std::filesystem::path(".");
  const std::filesystem::path candidate = dir / "db_engine";
  if (std::filesystem::exists(candidate, ec) &&
      !std::filesystem::is_directory(candidate, ec))
    return candidate.string();
  return std::string();
}

int ParseCreateOptions(const std::vector<std::string>& args, std::size_t start,
                       Command* out, std::string* error) {
  for (std::size_t i = start; i < args.size(); ++i) {
    const std::string& flag = args[i];
    if (flag == "--no-engine") {
      out->engine.clear();
      out->engine_explicit = true;
      continue;
    }
    if (flag == "--isolate" || flag == "--namespace") {
      out->isolate_namespaces = true;
      continue;
    }
    if (flag != "--memory" && flag != "--cpu" && flag != "--cpu-percent" &&
        flag != "--engine")
      return Usage(error, "unknown flag: " + flag);
    if (i + 1 >= args.size())
      return Usage(error, "missing value for " + flag);
    const std::string& val = args[++i];
    if (flag == "--memory") {
#ifdef _WIN32
      unsigned long long mem = 0;
#else
      unsigned long mem = 0;
#endif
      if (!ParseMemory(val, &mem))
        return Invalid(error, "invalid --memory value: " + val);
      out->memory = mem;
    } else if (flag == "--cpu" || flag == "--cpu-percent") {
      std::uint64_t v = 0;
      if (!ParseUint(val, 100, &v))
        return Invalid(error, "invalid " + flag + " value (0..100): " + val);
      out->cpu = static_cast<unsigned int>(v);
    } else {
      if (val.empty())
        return Invalid(error, "empty --engine path");
      out->engine = val;
      out->engine_explicit = true;
    }
  }
  return kOk;
}

bool IsDbKind(CommandKind kind) {
  switch (kind) {
    case CommandKind::kDbInit:
    case CommandKind::kTableCreate:
    case CommandKind::kTableList:
    case CommandKind::kInsert:
    case CommandKind::kGet:
    case CommandKind::kUpdate:
    case CommandKind::kDelete:
    case CommandKind::kScan:
      return true;
    default:
      return false;
  }
}

// Translates a parsed database command into the engine control protocol.
std::string EngineCommandFor(const Command& cmd) {
  switch (cmd.kind) {
    case CommandKind::kDbInit:
      return "init";
    case CommandKind::kTableCreate:
      return "table create " + cmd.table;
    case CommandKind::kTableList:
      return "table list";
    case CommandKind::kInsert:
      return "insert " + cmd.table + " " +
             HexEncode(cmd.data.data(), cmd.data.size());
    case CommandKind::kGet:
      return "get " + cmd.table + " " + std::to_string(cmd.page) + " " +
             std::to_string(cmd.slot);
    case CommandKind::kUpdate:
      return "update " + cmd.table + " " + std::to_string(cmd.page) + " " +
             std::to_string(cmd.slot) + " " +
             HexEncode(cmd.data.data(), cmd.data.size());
    case CommandKind::kDelete:
      return "delete " + cmd.table + " " + std::to_string(cmd.page) + " " +
             std::to_string(cmd.slot);
    case CommandKind::kScan:
      return "scan " + cmd.table;
    default:
      return std::string();
  }
}

// Executes a database command against the RUNNING engine over its control
// socket, keeping its output identical to the direct (stopped-container) path.
Result DispatchViaEngine(Manager* mgr, const Command& cmd) {
  Result result;
  std::string response;
  const Status s = mgr->EngineRequest(cmd.name, EngineCommandFor(cmd), &response);
  if (!s.ok())
    return (MapStatus(s, &result), result);

  const std::size_t nl = response.find('\n');
  const std::string first =
      response.substr(0, nl == std::string::npos ? response.size() : nl);
  const std::string body =
      (nl == std::string::npos) ? std::string() : response.substr(nl + 1);

  if (first.rfind("OK", 0) == 0) {
    const std::string payload = first.size() > 2 ? first.substr(3) : std::string();
    result.exit_code = kOk;
    result.output = body.empty() ? payload : body;
    if (cmd.kind == CommandKind::kScan)
      result.output = FormatEngineScanOutput(result.output, cmd.scan_verbose);

    if (cmd.kind == CommandKind::kDbInit) {
      result.output = "initialized database " + cmd.name;
    } else if (cmd.kind == CommandKind::kTableCreate) {
      result.output = "created table " + cmd.name + "." + cmd.table;
    } else if (cmd.kind == CommandKind::kInsert) {
      result.output = "inserted " + result.output;
    } else if (cmd.kind == CommandKind::kTableList && result.output.empty()) {
      result.output = "no tables";
    } else if (cmd.kind == CommandKind::kScan && result.output.empty()) {
      result.output = "no records";
    }
    return result;
  }

  if (first.rfind("ERR ", 0) == 0) {
    const std::string rest = first.substr(4);
    const std::size_t sp = rest.find(' ');
    const std::string code_text =
        (sp == std::string::npos) ? rest : rest.substr(0, sp);
    const std::string message =
        (sp == std::string::npos) ? std::string() : rest.substr(sp + 1);
    std::uint64_t code = 1;
    (void)ParseUint(code_text, 100, &code);
    result.exit_code = (code == kInvalid || code == kNotFound)
                           ? static_cast<int>(code)
                           : kError;
    result.output = message;
    return result;
  }

  result.exit_code = kError;
  result.output = "unexpected engine response: " + first;
  return result;
}

int PrintExecResponse(const std::string& name, const std::string& response,
                      Result* out) {
  const std::size_t nl = response.find('\n');
  const std::string first =
      response.substr(0, nl == std::string::npos ? response.size() : nl);
  const std::string body =
      (nl == std::string::npos) ? std::string() : response.substr(nl + 1);

  if (first.rfind("OK", 0) == 0) {
    const std::string payload = first.size() > 2 ? first.substr(3) : std::string();
    out->exit_code = kOk;
    out->output = body.empty() ? payload : body;
    if (!out->output.empty() && out->output.back() == '\n')
      out->output.pop_back();
    return kOk;
  }
  if (first.rfind("ERR ", 0) == 0) {
    const std::string rest = first.substr(4);
    const std::size_t sp = rest.find(' ');
    const std::string code_text =
        (sp == std::string::npos) ? rest : rest.substr(0, sp);
    const std::string message =
        (sp == std::string::npos) ? std::string() : rest.substr(sp + 1);
    std::uint64_t code = 1;
    (void)ParseUint(code_text, 100, &code);
    out->exit_code = (code == kInvalid || code == kNotFound)
                         ? static_cast<int>(code)
                         : kError;
    out->output = message;
    return out->exit_code;
  }
  out->exit_code = kError;
  out->output = "engine " + name + ": unexpected response: " + first;
  return out->exit_code;
}

}  // namespace

std::string FormatScanRecord(unsigned int page, unsigned int slot,
                             const std::vector<std::uint8_t>& data,
                             bool verbose) {
  return FormatScanLine(page, slot, data.size(), data.data(), verbose);
}

const char* HelpText() {
  return
      "NexusDB — minimal end-to-end database CLI\n"
      "\n"
      "Usage:\n"
      "  nexusdb help\n"
      "  nexusdb create <name> [--memory <size>] [--cpu <percent>]"
      " [--engine <path> | --no-engine]\n"
      "  nexusdb start|stop|status|delete <name>\n"
      "  nexusdb resources <name>\n"
      "  nexusdb list\n"
      "  nexusdb exec <name> [<command> [args...]]\n"
      "  nexusdb db init <name>\n"
      "  nexusdb table create <name> <table>\n"
      "  nexusdb table list <name>\n"
      "  nexusdb insert <name> <table> \"<text>\"\n"
      "  nexusdb insert <name> <table> --hex <hex-data>\n"
      "  nexusdb get <name> <table> <page-id> <slot-id>\n"
      "  nexusdb update <name> <table> <page-id> <slot-id> \"<text>\"\n"
      "  nexusdb update <name> <table> <page-id> <slot-id> --hex <hex-data>\n"
      "  nexusdb delete <name> <table> <page-id> <slot-id>\n"
      "  nexusdb scan <name> <table> [--verbose]\n"
      "\n"
      "The long forms `nexusdb container ...` and `nexusdb db ...` also work\n"
      "and remain stable for scripts.\n"
      "\n"
      "Memory sizes accept plain bytes or binary suffixes: B, K/KB, M/MB, G/GB\n"
      "(e.g. 256M, 1G). 0 means unlimited. CPU is a percentage (0..100).\n"
      "\n"
      "Record input is UTF-8 text by default; its exact bytes are stored. Use\n"
      "--hex for binary/hexadecimal input (even length, no auto-detection).\n"
      "get always returns hexadecimal. scan prints readable UTF-8 text when\n"
      "the bytes are printable, otherwise hexadecimal; --verbose prints both.\n"
      "The database for <name> lives in $NEXUSDB_DATA_ROOT/<name>\n"
      "(default ./nexusdb_data).\n"
      "When the container is RUNNING the CLI talks to its db_engine over\n"
      "<data-dir>/nexusdb.sock; when it is stopped the CLI opens the database\n"
      "directly. `exec <name>` with no command is an interactive request loop.\n"
      "\n"
      "Exit codes: 0 ok, 1 operational error, 2 usage, 3 invalid argument,"
      " 4 not found, 5 I/O error, 6 bad state, 7 buffer full, 8 out of memory.\n";
}

const char* HelpTopic(const std::string& name) {
  if (name == "create") {
    return
        "usage: nexusdb create <name> [--memory <size>] [--cpu <percent>]"
        " [--engine <path> | --no-engine] [--isolate]\n"
        "\n"
        "  --memory <size>   limit in bytes or with B/K/KB/M/MB/G/GB suffix;"
        " 0 = unlimited\n"
        "  --cpu <percent>   CPU quota percentage, 0..100; 0 = unlimited\n"
        "  --engine <path>   engine executable (default: $NEXUSDB_DB_ENGINE,"
        " else ./db_engine)\n"
        "  --no-engine       do not launch an engine (placeholder workload)\n"
        "  --isolate         run workload in PID/mount/net/UTS/IPC namespaces\n";
  }
  if (name == "status") {
    return
        "usage: nexusdb status <name>\n"
        "\n"
        "Prints the container state, id, PID, cgroup path and configured limits.\n"
        "Use `nexusdb resources <name>` for live cgroup usage.\n";
  }
  if (name == "resources") {
    return
        "usage: nexusdb resources <name>\n"
        "\n"
        "Prints a live cgroup snapshot: attachment, memory use/limit/pressure,\n"
        "cumulative CPU time, CPU limit, PSI, and OOM counters. Unavailable\n"
        "values are labelled honestly instead of being reported as zero.\n";
  }
  if (name == "list") {
    return
        "usage: nexusdb list\n"
        "\n"
        "Lists every persisted container: NAME, STATE, PID, MEMORY, CPU, ENGINE.\n";
  }
  if (name == "exec") {
    return
        "usage: nexusdb exec <name> [<command> [args...]]\n"
        "\n"
        "Sends one control request to the RUNNING container's engine, or starts\n"
        "an interactive request loop when no command is given. Examples:\n"
        "  nexusdb exec demo ping\n"
        "  nexusdb exec demo stats\n"
        "  nexusdb exec demo\n";
  }
  if (name == "insert") {
    return
        "usage: nexusdb insert <name> <table> \"<text>\"\n"
        "       nexusdb insert <name> <table> --hex <hex-data>\n"
        "\n"
        "Stores the exact UTF-8 text bytes of <text> by default. Pass --hex to supply\n"
        "binary data as an even-length hexadecimal string; no auto-detection is\n"
        "performed, so \"48656c6c6f\" without --hex is stored as literal text.\n";
  }
  if (name == "update") {
    return
        "usage: nexusdb update <name> <table> <page-id> <slot-id> \"<text>\"\n"
        "       nexusdb update <name> <table> <page-id> <slot-id> --hex <hex-data>\n"
        "\n"
        "Replaces a record with the exact UTF-8 bytes of <text> by default.\n"
        "Pass --hex for even-length hexadecimal binary input.\n";
  }
  if (name == "scan") {
    return
        "usage: nexusdb scan <name> <table> [--verbose]\n"
        "\n"
        "Prints each record with its page, slot and byte length. Printable UTF-8\n"
        "payloads are shown as text; binary or non-printable payloads are shown\n"
        "as hexadecimal. --verbose shows text (when available) and hexadecimal.\n";
  }
  if (name == "start")
    return "usage: nexusdb start <name>\n";
  if (name == "stop")
    return "usage: nexusdb stop <name>\n";
  if (name == "delete")
    return "usage: nexusdb delete <name>\n";
  return HelpText();
}

int ParseArgs(const std::vector<std::string>& args, Command* out,
              std::string* error) {
  if (out == nullptr)
    return Invalid(error, "internal: null command");
  if (args.empty())
    return Usage(error, "no command given (try `nexusdb help`)");

  const std::string& cmd = args[0];
  if (cmd == "help" || cmd == "--help" || cmd == "-h") {
    out->kind = CommandKind::kHelp;
    out->help_topic = args.size() > 1 ? args[1] : std::string();
    return kOk;
  }

  // `<command> --help` / `<command> -h`
  if (args.size() == 2 && IsHelpFlag(args[1])) {
    out->kind = CommandKind::kHelp;
    out->help_topic = cmd;
    return kOk;
  }

  // Short forms: create/start/stop/status/delete/list/exec.
  if (cmd == "create") {
    if (args.size() < 2)
      return Usage(error, "usage: nexusdb create <name> [options]");
    out->kind = CommandKind::kContainerCreate;
    out->name = args[1];
    return ParseCreateOptions(args, 2, out, error);
  }
  if (cmd == "start" || cmd == "stop" || cmd == "status") {
    if (args.size() != 2)
      return Usage(error, "usage: nexusdb " + cmd + " <name>");
    out->name = args[1];
    out->kind = (cmd == "start")   ? CommandKind::kContainerStart
                : (cmd == "stop")  ? CommandKind::kContainerStop
                                   : CommandKind::kContainerStatus;
    return kOk;
  }
  if (cmd == "resources") {
    if (args.size() != 2)
      return Usage(error, "usage: nexusdb resources <name>");
    out->kind = CommandKind::kContainerResources;
    out->name = args[1];
    return kOk;
  }
  // `delete <name>` deletes a container; `delete <name> <table> <page> <slot>`
  // deletes a record (handled below). Disambiguate on the argument count.
  if (cmd == "delete" && args.size() == 2) {
    out->kind = CommandKind::kContainerDelete;
    out->name = args[1];
    return kOk;
  }
  if (cmd == "list") {
    if (args.size() != 1)
      return Usage(error, "usage: nexusdb list");
    out->kind = CommandKind::kContainerList;
    return kOk;
  }
  if (cmd == "exec") {
    if (args.size() < 2)
      return Usage(error, "usage: nexusdb exec <name> [<command> [args...]]");
    out->kind = CommandKind::kContainerExec;
    out->name = args[1];
    for (std::size_t i = 2; i < args.size(); ++i) {
      if (i != 2)
        out->request.push_back(' ');
      out->request += args[i];
    }
    return kOk;
  }

  // container <sub> ...
  if (cmd == "container") {
    if (args.size() < 2)
      return Usage(error,
                   "usage: nexusdb container "
                    "<create|start|stop|status|delete|list|resources> <name>");
    const std::string& sub = args[1];
    if (sub == "create") {
      if (args.size() < 3)
        return Usage(error,
                      "usage: nexusdb container create <name> [--memory <size>] "
                      "[--cpu <percent>] [--engine <path> | --no-engine] [--isolate]");
      out->kind = CommandKind::kContainerCreate;
      out->name = args[2];
      return ParseCreateOptions(args, 3, out, error);
    }
    if (sub == "list") {
      if (args.size() != 2)
        return Usage(error, "usage: nexusdb container list");
      out->kind = CommandKind::kContainerList;
      return kOk;
    }
    if (sub == "start" || sub == "stop" || sub == "status" || sub == "delete" ||
        sub == "resources") {
      if (args.size() != 3)
        return Usage(error, "usage: nexusdb container " + sub + " <name>");
      out->name = args[2];
      out->kind = (sub == "start")      ? CommandKind::kContainerStart
                  : (sub == "stop")     ? CommandKind::kContainerStop
                  : (sub == "status")   ? CommandKind::kContainerStatus
                  : (sub == "resources") ? CommandKind::kContainerResources
                                         : CommandKind::kContainerDelete;
      return kOk;
    }
    return Usage(error, "unknown container subcommand: " + sub);
  }

  // db init <name>
  if (cmd == "db") {
    if (args.size() != 3 || args[1] != "init")
      return Usage(error, "usage: nexusdb db init <name>");
    out->kind = CommandKind::kDbInit;
    out->name = args[2];
    return kOk;
  }

  // table create|list
  if (cmd == "table") {
    if (args.size() < 2)
      return Usage(error, "usage: nexusdb table <create|list> ...");
    if (args[1] == "create") {
      if (args.size() != 4)
        return Usage(error, "usage: nexusdb table create <name> <table>");
      out->kind = CommandKind::kTableCreate;
      out->name = args[2];
      out->table = args[3];
      return kOk;
    }
    if (args[1] == "list") {
      if (args.size() != 3)
        return Usage(error, "usage: nexusdb table list <name>");
      out->kind = CommandKind::kTableList;
      out->name = args[2];
      return kOk;
    }
    return Usage(error, "unknown table subcommand: " + args[1]);
  }

  // insert <name> <table> <hex>
  if (cmd == "insert") {
    if (args.size() != 4)
      return Usage(error,
                   "usage: nexusdb insert <name> <table> \"<text>\" | --hex "
                   "<hex-data>");
    out->kind = CommandKind::kInsert;
    out->name = args[1];
    out->table = args[2];
    if (!HexDecode(args[3], &out->data))
      return Invalid(error, "malformed hex data: " + args[3]);
    if (out->data.empty())
      return Invalid(error, "record data must be non-empty");
    return kOk;
  }

  // get <name> <table> <page> <slot>
  if (cmd == "get") {
    if (args.size() != 5)
      return Usage(error,
                   "usage: nexusdb get <name> <table> <page-id> <slot-id>");
    out->kind = CommandKind::kGet;
    out->name = args[1];
    out->table = args[2];
    std::uint64_t v = 0;
    if (!ParseUint(args[3], static_cast<std::uint64_t>(Page::kInvalidId) - 1,
                   &v))
      return Invalid(error, "invalid page id: " + args[3]);
    out->page = static_cast<Page::PageId>(v);
    if (!ParseUint(args[4], 0xFFFF, &v))
      return Invalid(error, "invalid slot id: " + args[4]);
    out->slot = static_cast<std::uint16_t>(v);
    return kOk;
  }

  // update <name> <table> <page> <slot> <hex>
  if (cmd == "update") {
    if (args.size() != 6)
      return Usage(error,
                    "usage: nexusdb update <name> <table> <page-id> <slot-id> "
                    "\"<text>\" | --hex <hex-data>");
    out->kind = CommandKind::kUpdate;

    out->name = args[1];
    out->table = args[2];
    std::uint64_t v = 0;
    if (!ParseUint(args[3], static_cast<std::uint64_t>(Page::kInvalidId) - 1,
                   &v))
      return Invalid(error, "invalid page id: " + args[3]);
    out->page = static_cast<Page::PageId>(v);
    if (!ParseUint(args[4], 0xFFFF, &v))
      return Invalid(error, "invalid slot id: " + args[4]);
    out->slot = static_cast<std::uint16_t>(v);
    if (!HexDecode(args[5], &out->data))
      return Invalid(error, "malformed hex data: " + args[5]);
    if (out->data.empty())
      return Invalid(error, "record data must be non-empty");
    return kOk;
  }

  // delete <name> <table> <page> <slot>
  if (cmd == "delete") {
    if (args.size() != 5)
      return Usage(error,
                   "usage: nexusdb delete <name> <table> <page-id> <slot-id>");
    out->kind = CommandKind::kDelete;
    out->name = args[1];
    out->table = args[2];
    std::uint64_t v = 0;
    if (!ParseUint(args[3], static_cast<std::uint64_t>(Page::kInvalidId) - 1,
                   &v))
      return Invalid(error, "invalid page id: " + args[3]);
    out->page = static_cast<Page::PageId>(v);
    if (!ParseUint(args[4], 0xFFFF, &v))
      return Invalid(error, "invalid slot id: " + args[4]);
    out->slot = static_cast<std::uint16_t>(v);
    return kOk;
  }

  // scan <name> <table> [--verbose]
  if (cmd == "scan") {
    std::vector<std::string> positional;
    bool verbose = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
      if (args[i] == "--verbose") {
        verbose = true;
      } else if (args[i].rfind("--", 0) == 0) {
        return Usage(error, "unknown scan option: " + args[i]);
      } else {
        positional.push_back(args[i]);
      }
    }
    if (positional.size() != 2)
      return Usage(error, "usage: nexusdb scan <name> <table> [--verbose]");
    out->kind = CommandKind::kScan;
    out->name = positional[0];
    out->table = positional[1];
    out->scan_verbose = verbose;
    return kOk;
  }

  return Usage(error, "unknown command: " + cmd);
}

Result Dispatch(Manager* mgr, const Command& cmd) {
  Result result;
  if (mgr == nullptr) {
    result.exit_code = kError;
    result.output = "internal: null manager";
    return result;
  }

  // Database commands on a RUNNING container must go through the engine's
  // control socket: the engine holds the files open, so opening them directly
  // would race its page writes (and be refused by Manager::OpenDb).
  if (IsDbKind(cmd.kind) && mgr->ContainerIsRunning(cmd.name)) {
    Result routed = DispatchViaEngine(mgr, cmd);
    if (routed.exit_code != kOk && !mgr->EngineAlive(cmd.name) &&
        mgr->MarkContainerStopped(cmd.name).ok()) {
      return Dispatch(mgr, cmd);
    }
    return routed;
  }

  switch (cmd.kind) {
    case CommandKind::kHelp:
      result.exit_code = kOk;
      result.output = HelpTopic(cmd.help_topic);
      return result;

    case CommandKind::kContainerCreate: {
      std::string engine = cmd.engine;
      if (!cmd.engine_explicit)
        engine = ResolveDefaultEngine(cmd.program_path);
      const Status s = mgr->ContainerCreate(
          cmd.name, cmd.memory, cmd.cpu, engine, cmd.isolate_namespaces);
      if (!s.ok())
        return (MapStatus(s, &result), result);
      result.output = "created container " + cmd.name;
      return result;
    }
    case CommandKind::kContainerStart: {
      const Status s = mgr->ContainerStart(cmd.name);
      if (!s.ok())
        return (MapStatus(s, &result), result);
      result.output = "started container " + cmd.name;
      return result;
    }
    case CommandKind::kContainerStop: {
      const Status s = mgr->ContainerStop(cmd.name);
      if (!s.ok())
        return (MapStatus(s, &result), result);
      result.output = "stopped container " + cmd.name;
      return result;
    }
    case CommandKind::kContainerDelete: {
      const Status s = mgr->ContainerDelete(cmd.name);
      if (!s.ok())
        return (MapStatus(s, &result), result);
      result.output = "deleted container " + cmd.name;
      return result;
    }
    case CommandKind::kContainerStatus: {
      std::string text;
      const Status s = mgr->ContainerStatus(cmd.name, &text);
      if (!s.ok())
        return (MapStatus(s, &result), result);
      result.output = std::move(text);
      return result;
    }
    case CommandKind::kContainerResources: {
      std::string text;
      const Status s = mgr->ContainerResources(cmd.name, &text);
      if (!s.ok())
        return (MapStatus(s, &result), result);
      result.output = std::move(text);
      return result;
    }
    case CommandKind::kContainerList: {
      std::vector<Manager::ContainerInfo> infos;
      const Status s = mgr->ListContainers(&infos);
      if (!s.ok())
        return (MapStatus(s, &result), result);
      if (infos.empty()) {
        result.output = "no containers";
        return result;
      }
      char line[256];
      std::snprintf(line, sizeof(line), "%-20s %-10s %-8s %-14s %-6s %s\n",
                    "NAME", "STATE", "PID", "MEMORY", "CPU", "ENGINE");
      result.output = line;
      for (const Manager::ContainerInfo& info : infos) {
        const char* state = "UNKNOWN";
        switch (info.state) {
          case NEXUS_CREATED:
            state = "CREATED";
            break;
          case NEXUS_RUNNING:
            state = "RUNNING";
            break;
          case NEXUS_STOPPED:
            state = "STOPPED";
            break;
          case NEXUS_DELETED:
            state = "DELETED";
            break;
          default:
            break;
        }
        char cpu[16];
        std::snprintf(cpu, sizeof(cpu), "%u%%", info.cpu_percent);
        std::snprintf(line, sizeof(line), "%-20s %-10s %-8ld %-14lu %-6s %s\n",
                      info.name.c_str(), state, info.pid, info.memory_bytes, cpu,
                      info.engine.c_str());
        result.output += line;
      }
      return result;
    }
    case CommandKind::kContainerExec: {
      if (!mgr->ContainerIsRunning(cmd.name)) {
        result.exit_code = kError;
        result.output = "container " + cmd.name + " is not RUNNING";
        return result;
      }
      if (!cmd.request.empty()) {
        std::string response;
        const Status s = mgr->EngineRequest(cmd.name, cmd.request, &response);
        if (!s.ok())
          return (MapStatus(s, &result), result);
        (void)PrintExecResponse(cmd.name, response, &result);
        return result;
      }
      // Interactive loop: one request per line until EOF or `quit`.
      result.exit_code = kOk;
      std::string line;
      while (std::getline(std::cin, line)) {
        if (line.empty())
          continue;
        std::string response;
        const Status s = mgr->EngineRequest(cmd.name, line, &response);
        if (!s.ok())
          return (MapStatus(s, &result), result);
        Result one;
        (void)PrintExecResponse(cmd.name, response, &one);
        if (!one.output.empty()) {
          std::fputs(one.output.c_str(), stdout);
          if (one.output.back() != '\n')
            std::fputc('\n', stdout);
          std::fflush(stdout);
        }
        if (line == "quit")
          break;
        if (one.exit_code != kOk)
          result.exit_code = one.exit_code;
      }
      return result;
    }

    case CommandKind::kDbInit: {
      const Status s = mgr->DbInit(cmd.name);
      if (!s.ok())
        return (MapStatus(s, &result), result);
      result.output = "initialized database " + cmd.name;
      return result;
    }
    case CommandKind::kTableCreate: {
      const Status s = mgr->TableCreate(cmd.name, cmd.table);
      if (!s.ok())
        return (MapStatus(s, &result), result);
      result.output = "created table " + cmd.name + "." + cmd.table;
      return result;
    }
    case CommandKind::kTableList: {
      std::vector<std::string> tables;
      const Status s = mgr->TableList(cmd.name, &tables);
      if (!s.ok())
        return (MapStatus(s, &result), result);
      if (tables.empty()) {
        result.output = "no tables";
      } else {
        for (std::size_t i = 0; i < tables.size(); ++i) {
          if (i != 0)
            result.output.push_back('\n');
          result.output += tables[i];
        }
      }
      return result;
    }
    case CommandKind::kInsert: {
      Page::PageId page = 0;
      std::uint16_t slot = 0;
      const Status s = mgr->Insert(cmd.name, cmd.table, cmd.data.data(),
                                   cmd.data.size(), &page, &slot);
      if (!s.ok())
        return (MapStatus(s, &result), result);
      char buf[64];
      std::snprintf(buf, sizeof(buf), "inserted page=%u slot=%u", page, slot);
      result.output = buf;
      return result;
    }
    case CommandKind::kGet: {
      std::vector<std::uint8_t> data;
      const Status s = mgr->Get(cmd.name, cmd.table, cmd.page, cmd.slot, &data);
      if (!s.ok())
        return (MapStatus(s, &result), result);
      char buf[64];
      std::snprintf(buf, sizeof(buf), "page=%u slot=%u len=%zu hex=", cmd.page,
                    cmd.slot, data.size());
      result.output = std::string(buf) + HexEncode(data.data(), data.size());
      return result;
    }
    case CommandKind::kUpdate: {
      const Status s = mgr->Update(cmd.name, cmd.table, cmd.page, cmd.slot,
                                   cmd.data.data(), cmd.data.size());
      if (!s.ok())
        return (MapStatus(s, &result), result);
      char buf[64];
      std::snprintf(buf, sizeof(buf), "updated page=%u slot=%u", cmd.page,
                    cmd.slot);
      result.output = buf;
      return result;
    }
    case CommandKind::kDelete: {
      const Status s = mgr->Delete(cmd.name, cmd.table, cmd.page, cmd.slot);
      if (!s.ok())
        return (MapStatus(s, &result), result);
      char buf[64];
      std::snprintf(buf, sizeof(buf), "deleted page=%u slot=%u", cmd.page,
                    cmd.slot);
      result.output = buf;
      return result;
    }
    case CommandKind::kScan: {
      std::vector<ScanRecord> records;
      const Status s = mgr->Scan(cmd.name, cmd.table, &records);
      if (!s.ok())
        return (MapStatus(s, &result), result);
      if (records.empty()) {
        result.output = "no records";
        return result;
      }
      for (const ScanRecord& r : records) {
        result.output += FormatScanLine(r.page, r.slot, r.data.size(),
                                        r.data.data(), cmd.scan_verbose);
        result.output.push_back('\n');
      }
      return result;
    }
  }
  result.exit_code = kUsage;
  result.output = "unknown command";
  return result;
}

Result Run(Manager* mgr, const std::vector<std::string>& args,
           const std::string& program_path) {
  Command cmd;
  cmd.program_path = program_path;
  std::string error;
  const int rc = ParseArgs(args, &cmd, &error);
  if (rc != kOk) {
    Result result;
    result.exit_code = rc;
    result.output = error;
    return result;
  }
  return Dispatch(mgr, cmd);
}

}  // namespace cli
}  // namespace nexusdb
