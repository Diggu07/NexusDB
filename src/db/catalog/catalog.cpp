// NexusDB — persistent catalog implementation (R1.3).
//
// See include/db/catalog.h for the on-disk format, ownership rules, and
// validation contract. Page bytes move only through BufferPool handles; new
// pages are allocated only through PageStorage.

#include "db/catalog.h"

#include <cstring>

namespace nexusdb {
namespace {

// Catalog page magic: 'N','C','A','1' ("Nexus Catalog, v1").
constexpr std::uint8_t kMagic0 = 0x4Eu;  // 'N'
constexpr std::uint8_t kMagic1 = 0x43u;  // 'C'
constexpr std::uint8_t kMagic2 = 0x41u;  // 'A'
constexpr std::uint8_t kMagic3 = 0x31u;  // '1'

// Catalog page field offsets (see the layout table in the header).
constexpr std::size_t kOffMagic = 0;
constexpr std::size_t kOffVersion = 4;
constexpr std::size_t kOffReserved = 6;
constexpr std::size_t kOffCount = 8;
constexpr std::size_t kOffNext = 12;
constexpr std::size_t kOffEntries = Catalog::kHeaderSize;

inline Status Bad(const char* message) {
  return Status::Error(DbError::kInvalidArgument, message);
}

inline Status BadState(const char* message) {
  return Status::Error(DbError::kBadState, message);
}

std::uint16_t ReadU16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(p[0]) |
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8));
}

void WriteU16(std::uint8_t* p, std::uint16_t v) {
  p[0] = static_cast<std::uint8_t>(v & 0xFFu);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
}

std::uint32_t ReadU32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

void WriteU32(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v & 0xFFu);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
  p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
  p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
}

std::size_t EntryOffset(std::uint32_t index) {
  return kOffEntries + static_cast<std::size_t>(index) * Catalog::kEntrySize;
}

void InitCatalogPage(Page* page) {
  std::uint8_t* d = page->data();
  std::memset(d, 0, Page::size());
  d[kOffMagic + 0] = kMagic0;
  d[kOffMagic + 1] = kMagic1;
  d[kOffMagic + 2] = kMagic2;
  d[kOffMagic + 3] = kMagic3;
  WriteU16(d + kOffVersion, Catalog::kVersion);
  WriteU16(d + kOffReserved, 0);
  WriteU32(d + kOffCount, 0);
  WriteU32(d + kOffNext, Page::kInvalidId);
  page->MarkDirty();
}

Status ValidateCatalogHeader(const Page& page) {
  const std::uint8_t* d = page.data();
  if (d[kOffMagic + 0] != kMagic0 || d[kOffMagic + 1] != kMagic1 ||
      d[kOffMagic + 2] != kMagic2 || d[kOffMagic + 3] != kMagic3) {
    return Bad("catalog: page has bad magic");
  }
  if (ReadU16(d + kOffVersion) != Catalog::kVersion) {
    return Bad("catalog: unsupported version");
  }
  if (ReadU32(d + kOffCount) > Catalog::kCapacity) {
    return Bad("catalog: page count out of range");
  }
  return Status::Ok();
}

std::uint32_t CatalogCount(const Page& page) {
  return ReadU32(page.data() + kOffCount);
}

std::uint32_t CatalogNext(const Page& page) {
  return ReadU32(page.data() + kOffNext);
}

void SetCatalogCount(Page* page, std::uint32_t count) {
  WriteU32(page->data() + kOffCount, count);
}

void SetCatalogNext(Page* page, Page::PageId next) {
  WriteU32(page->data() + kOffNext, next);
}

// Reads the name out of a 32-byte entry field. Returns false when the field
// holds no NUL terminator (an over-long / malformed name).
bool ReadNameField(const std::uint8_t* field, std::string* out) {
  for (std::size_t i = 0; i < Catalog::kNameFieldSize; ++i) {
    if (field[i] == 0u) {
      out->assign(reinterpret_cast<const char*>(field), i);
      return true;
    }
  }
  return false;
}

void WriteNameField(std::uint8_t* field, const std::string& name) {
  std::memset(field, 0, Catalog::kNameFieldSize);
  std::memcpy(field, name.data(), name.size());
}

void WriteEntry(Page* page, std::uint32_t index, const std::string& name,
                Page::PageId root) {
  std::uint8_t* d = page->data() + EntryOffset(index);
  WriteNameField(d, name);
  WriteU32(d + Catalog::kNameFieldSize, root);
}

bool ReadEntry(const Page& page, std::uint32_t index, std::string* name,
               Page::PageId* root) {
  const std::uint8_t* d = page.data() + EntryOffset(index);
  if (!ReadNameField(d, name)) {
    return false;
  }
  *root = ReadU32(d + Catalog::kNameFieldSize);
  return true;
}

}  // namespace

bool Catalog::IsValidTableName(const std::string& name) {
  if (name.empty() || name.size() > kMaxTableNameLength) {
    return false;
  }
  for (char ch : name) {
    const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                    (ch >= '0' && ch <= '9') || ch == '_';
    if (!ok) {
      return false;
    }
  }
  return true;
}

Status Catalog::Create(PageStorage* storage, BufferPool* pool,
                       std::unique_ptr<Catalog>* out, Page::PageId* out_root) {
  if (storage == nullptr || pool == nullptr || out == nullptr) {
    return Bad("Catalog::Create: null storage, pool, or out");
  }

  Page root_page;
  Status s = storage->AllocatePage(&root_page);
  if (!s.ok()) {
    return s;
  }
  InitCatalogPage(&root_page);
  s = storage->WritePage(root_page);
  if (!s.ok()) {
    return s;
  }

  std::unique_ptr<Catalog> catalog(new Catalog(storage, pool, root_page.id()));
  s = catalog->Load();
  if (!s.ok()) {
    return s;
  }
  if (out_root != nullptr) {
    *out_root = root_page.id();
  }
  *out = std::move(catalog);
  return Status::Ok();
}

Status Catalog::Open(PageStorage* storage, BufferPool* pool, Page::PageId root,
                     std::unique_ptr<Catalog>* out) {
  if (storage == nullptr || pool == nullptr || out == nullptr) {
    return Bad("Catalog::Open: null storage, pool, or out");
  }
  if (root == Page::kInvalidId) {
    return Bad("Catalog::Open: invalid root page id");
  }
  std::unique_ptr<Catalog> catalog(new Catalog(storage, pool, root));
  Status s = catalog->Load();
  if (!s.ok()) {
    return s;
  }
  *out = std::move(catalog);
  return Status::Ok();
}

Status Catalog::Load() {
  entries_.clear();
  last_page_ = Page::kInvalidId;
  last_page_count_ = 0;

  std::uint32_t total_pages = 0;
  Status s = storage_->NumPages(&total_pages);
  if (!s.ok()) {
    return s;
  }
  // No valid chain can be longer than the number of pages in the file; the +2
  // keeps the bound strictly above any legitimate chain.
  const std::uint32_t max_walk = total_pages + 2;

  std::vector<Entry> found;
  Page::PageId page = root_;
  Page::PageId prev = Page::kInvalidId;
  std::uint32_t walked = 0;

  for (;;) {
    if (walked++ > max_walk) {
      return BadState("catalog: chain is cyclic");
    }

    BufferPool::Handle handle;
    s = pool_->Fetch(page, &handle);
    if (!s.ok()) {
      return s;
    }
    s = ValidateCatalogHeader(handle.page());
    if (!s.ok()) {
      return s;
    }

    const std::uint32_t count = CatalogCount(handle.page());
    const std::uint32_t next = CatalogNext(handle.page());
    for (std::uint32_t i = 0; i < count; ++i) {
      std::string name;
      Page::PageId table_root = Page::kInvalidId;
      if (!ReadEntry(handle.page(), i, &name, &table_root)) {
        return BadState("catalog: entry name is not terminated");
      }
      if (!IsValidTableName(name)) {
        return BadState("catalog: entry has an invalid table name");
      }
      for (const Entry& e : found) {
        if (e.name == name) {
          return BadState("catalog: duplicate table name");
        }
      }
      if (table_root == Page::kInvalidId) {
        return BadState("catalog: entry has an invalid table root id");
      }
      found.push_back(Entry{name, table_root});
    }
    last_page_ = page;
    last_page_count_ = count;

    if (next == Page::kInvalidId) {
      break;
    }
    if (next == page || next == prev) {
      return BadState("catalog: chain loops on itself");
    }
    prev = page;
    page = next;
  }

  // Validate that every recorded table root exists and really is a heap table
  // (HeapTable::Open revalidates the table's own directory chain). This is done
  // after the walk so no catalog handle is pinned while a table page is loaded.
  for (const Entry& e : found) {
    if (!storage_->PageExists(e.root)) {
      return BadState("catalog: entry references a missing table page");
    }
    std::unique_ptr<HeapTable> table;
    s = HeapTable::Open(storage_, pool_, e.root, &table);
    if (!s.ok()) {
      return BadState("catalog: entry does not reference a valid table");
    }
  }

  entries_ = std::move(found);
  return Status::Ok();
}

bool Catalog::Exists(const std::string& name) const {
  for (const Entry& e : entries_) {
    if (e.name == name) {
      return true;
    }
  }
  return false;
}

Status Catalog::FindTable(const std::string& name,
                          Page::PageId* out_root) const {
  if (out_root == nullptr) {
    return Bad("Catalog::FindTable: null out_root");
  }
  *out_root = Page::kInvalidId;
  for (const Entry& e : entries_) {
    if (e.name == name) {
      *out_root = e.root;
      return Status::Ok();
    }
  }
  return Status::Error(DbError::kNotFound, "catalog: no such table");
}

Status Catalog::OpenTable(const std::string& name,
                          std::unique_ptr<HeapTable>* out) {
  if (out == nullptr) {
    return Bad("Catalog::OpenTable: null out");
  }
  Page::PageId table_root = Page::kInvalidId;
  Status s = FindTable(name, &table_root);
  if (!s.ok()) {
    return s;
  }
  return HeapTable::Open(storage_, pool_, table_root, out);
}

Status Catalog::CreateTable(const std::string& name,
                            Page::PageId* out_table_root) {
  if (out_table_root != nullptr) {
    *out_table_root = Page::kInvalidId;
  }
  if (!IsValidTableName(name)) {
    return Bad("Catalog::CreateTable: invalid table name");
  }
  if (Exists(name)) {
    return Bad("Catalog::CreateTable: table already exists");
  }

  std::unique_ptr<HeapTable> table;
  Page::PageId table_root = Page::kInvalidId;
  Status s = HeapTable::Create(storage_, pool_, &table, &table_root);
  if (!s.ok()) {
    return s;
  }
  // Record the table only after its root page exists, so no catalog entry can
  // ever point at a table that was not created.
  s = AppendEntry(name, table_root);
  if (!s.ok()) {
    return s;
  }
  if (out_table_root != nullptr) {
    *out_table_root = table_root;
  }
  return Status::Ok();
}

Status Catalog::ListTables(std::vector<std::string>* out) const {
  if (out == nullptr) {
    return Bad("Catalog::ListTables: null out");
  }
  out->clear();
  out->reserve(entries_.size());
  for (const Entry& e : entries_) {
    out->push_back(e.name);
  }
  return Status::Ok();
}

Status Catalog::AppendEntry(const std::string& name, Page::PageId root) {
  if (last_page_count_ < kCapacity) {
    BufferPool::Handle handle;
    Status s = pool_->Fetch(last_page_, &handle);
    if (!s.ok()) {
      return s;
    }
    s = ValidateCatalogHeader(handle.page());
    if (!s.ok()) {
      return s;
    }
    WriteEntry(&handle.page(), last_page_count_, name, root);
    SetCatalogCount(&handle.page(), last_page_count_ + 1);
    handle.MarkDirty();
    ++last_page_count_;
    entries_.push_back(Entry{name, root});
    return Status::Ok();
  }

  // The last catalog page is full: allocate and link a new one, then use it.
  Page new_page;
  Status s = storage_->AllocatePage(&new_page);
  if (!s.ok()) {
    return s;
  }
  InitCatalogPage(&new_page);
  s = storage_->WritePage(new_page);
  if (!s.ok()) {
    return s;
  }

  {
    BufferPool::Handle handle;
    s = pool_->Fetch(last_page_, &handle);
    if (!s.ok()) {
      return s;
    }
    SetCatalogNext(&handle.page(), new_page.id());
    handle.MarkDirty();
  }

  last_page_ = new_page.id();
  last_page_count_ = 0;
  {
    BufferPool::Handle handle;
    s = pool_->Fetch(last_page_, &handle);
    if (!s.ok()) {
      return s;
    }
    WriteEntry(&handle.page(), 0, name, root);
    SetCatalogCount(&handle.page(), 1);
    handle.MarkDirty();
  }
  last_page_count_ = 1;
  entries_.push_back(Entry{name, root});
  return Status::Ok();
}

}  // namespace nexusdb
