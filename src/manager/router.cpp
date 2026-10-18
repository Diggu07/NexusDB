// NexusDB — command Router implementation (Digvijay: Manager/Router, R3).
#include "manager/router.h"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <utility>

namespace nexusdb {
namespace cli {
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
  switch (s.code) {
    case DbError::kOk:
      break;
    case DbError::kInvalidArgument:
      result->exit_code = kInvalid;
      break;
    case DbError::kNotFound:
      result->exit_code = kNotFound;
      break;
    default:
      result->exit_code = kError;
      break;
  }
  result->output = s.message;
  return result->exit_code;
}

}  // namespace

const char* HelpText() {
  return
      "NexusDB — minimal end-to-end database CLI\n"
      "\n"
      "Usage:\n"
      "  nexusdb help\n"
      "  nexusdb container create <name> [--memory <bytes>] [--cpu <percent>]"
      " [--engine <path>]\n"
      "  nexusdb container start <name>\n"
      "  nexusdb container stop <name>\n"
      "  nexusdb container status <name>\n"
      "  nexusdb container delete <name>\n"
      "  nexusdb db init <name>\n"
      "  nexusdb table create <name> <table>\n"
      "  nexusdb table list <name>\n"
      "  nexusdb insert <name> <table> <hex-data>\n"
      "  nexusdb get <name> <table> <page-id> <slot-id>\n"
      "  nexusdb update <name> <table> <page-id> <slot-id> <hex-data>\n"
      "  nexusdb delete <name> <table> <page-id> <slot-id>\n"
      "  nexusdb scan <name> <table>\n"
      "\n"
      "Records are supplied/returned as hexadecimal (even length). The database\n"
      "for <name> lives in $NEXUSDB_DATA_ROOT/<name> (default ./nexusdb_data).\n"
      "A running container holds the database open; stop it before database\n"
      "commands (there is no client/server protocol).\n"
      "\n"
      "Exit codes: 0 ok, 1 operational error, 2 usage, 3 invalid argument,"
      " 4 not found.\n";
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
    return kOk;
  }

  // container <sub> ...
  if (cmd == "container") {
    if (args.size() < 2)
      return Usage(error, "usage: nexusdb container <create|start|stop|status|delete> <name>");
    const std::string& sub = args[1];
    if (sub == "create") {
      if (args.size() < 3)
        return Usage(error, "usage: nexusdb container create <name> [--memory <bytes>] [--cpu <percent>] [--engine <path>]");
      out->kind = CommandKind::kContainerCreate;
      out->name = args[2];
      for (std::size_t i = 3; i < args.size(); ++i) {
        const std::string& flag = args[i];
        if (i + 1 >= args.size())
          return Usage(error, "missing value for " + flag);
        const std::string& val = args[++i];
        if (flag == "--memory") {
          const std::uint64_t max_memory = 0xFFFFFFFFFFFFULL;
          std::uint64_t v = 0;
          if (!ParseUint(val, max_memory, &v) ||
              v > static_cast<std::uint64_t>(
                      std::numeric_limits<unsigned long>::max()))
            return Invalid(error, "invalid --memory value: " + val);
          out->memory = static_cast<unsigned long>(v);
        } else if (flag == "--cpu") {
          std::uint64_t v = 0;
          if (!ParseUint(val, 100, &v))
            return Invalid(error, "invalid --cpu value (0..100): " + val);
          out->cpu = static_cast<unsigned int>(v);
        } else if (flag == "--engine") {
          out->engine = val;
        } else {
          return Usage(error, "unknown flag: " + flag);
        }
      }
      return kOk;
    }
    if (sub == "start" || sub == "stop" || sub == "status" || sub == "delete") {
      if (args.size() != 3)
        return Usage(error, "usage: nexusdb container " + sub + " <name>");
      out->name = args[2];
      out->kind = (sub == "start")   ? CommandKind::kContainerStart
                  : (sub == "stop")  ? CommandKind::kContainerStop
                  : (sub == "status") ? CommandKind::kContainerStatus
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
      return Usage(error, "usage: nexusdb insert <name> <table> <hex-data>");
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
      return Usage(error, "usage: nexusdb get <name> <table> <page-id> <slot-id>");
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
      return Usage(error, "usage: nexusdb update <name> <table> <page-id> <slot-id> <hex-data>");
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
      return Usage(error, "usage: nexusdb delete <name> <table> <page-id> <slot-id>");
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

  // scan <name> <table>
  if (cmd == "scan") {
    if (args.size() != 3)
      return Usage(error, "usage: nexusdb scan <name> <table>");
    out->kind = CommandKind::kScan;
    out->name = args[1];
    out->table = args[2];
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

  switch (cmd.kind) {
    case CommandKind::kHelp:
      result.exit_code = kOk;
      result.output = HelpText();
      return result;

    case CommandKind::kContainerCreate: {
      const Status s =
          mgr->ContainerCreate(cmd.name, cmd.memory, cmd.cpu, cmd.engine);
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
        char buf[64];
        std::snprintf(buf, sizeof(buf), "page=%u slot=%u len=%zu hex=", r.page,
                      r.slot, r.data.size());
        result.output += buf;
        result.output += HexEncode(r.data.data(), r.data.size());
        result.output.push_back('\n');
      }
      return result;
    }
  }
  result.exit_code = kUsage;
  result.output = "unknown command";
  return result;
}

Result Run(Manager* mgr, const std::vector<std::string>& args) {
  Command cmd;
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
