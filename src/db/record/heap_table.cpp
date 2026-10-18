// NexusDB — heap table implementation (R1.2).
//
// See include/db/heap_table.h for the page organization, ownership rules and
// failure contract. All record bytes move through BufferPool handles; only new
// page allocation goes through PageStorage.

#include "db/heap_table.h"

#include <cstring>

namespace nexusdb {
namespace {

// Directory page magic: 'N','H','T','1' ("Nexus Heap Table, v1").
constexpr std::uint8_t kDirMagic0 = 0x4Eu;  // 'N'
constexpr std::uint8_t kDirMagic1 = 0x48u;  // 'H'
constexpr std::uint8_t kDirMagic2 = 0x54u;  // 'T'
constexpr std::uint8_t kDirMagic3 = 0x31u;  // '1'

// Directory page field offsets (see the layout table in the header).
constexpr std::size_t kOffMagic = 0;
constexpr std::size_t kOffVersion = 4;
constexpr std::size_t kOffReserved = 6;
constexpr std::size_t kOffCount = 8;
constexpr std::size_t kOffNext = 12;
constexpr std::size_t kOffIds = HeapTable::kDirectoryHeaderSize;

inline Status Bad(const char* message) {
  return Status::Error(DbError::kInvalidArgument, message);
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

void InitDirectoryPage(Page* page) {
  std::uint8_t* d = page->data();
  std::memset(d, 0, Page::size());
  d[kOffMagic + 0] = kDirMagic0;
  d[kOffMagic + 1] = kDirMagic1;
  d[kOffMagic + 2] = kDirMagic2;
  d[kOffMagic + 3] = kDirMagic3;
  WriteU16(d + kOffVersion, HeapTable::kDirectoryVersion);
  WriteU16(d + kOffReserved, 0);
  WriteU32(d + kOffCount, 0);
  WriteU32(d + kOffNext, Page::kInvalidId);
  page->MarkDirty();
}

Status ValidateDirectoryHeader(const Page& page) {
  const std::uint8_t* d = page.data();
  if (d[kOffMagic + 0] != kDirMagic0 || d[kOffMagic + 1] != kDirMagic1 ||
      d[kOffMagic + 2] != kDirMagic2 || d[kOffMagic + 3] != kDirMagic3) {
    return Bad("heap table: directory page has bad magic");
  }
  if (ReadU16(d + kOffVersion) != HeapTable::kDirectoryVersion) {
    return Bad("heap table: unsupported directory version");
  }
  if (ReadU32(d + kOffCount) > HeapTable::kDirectoryCapacity) {
    return Bad("heap table: directory page count out of range");
  }
  return Status::Ok();
}

std::uint32_t DirectoryCount(const Page& page) {
  return ReadU32(page.data() + kOffCount);
}

std::uint32_t DirectoryNext(const Page& page) {
  return ReadU32(page.data() + kOffNext);
}

Page::PageId DirectoryIdAt(const Page& page, std::uint32_t index) {
  return ReadU32(page.data() + kOffIds +
                 static_cast<std::size_t>(index) * HeapTable::kDirectoryIdSize);
}

void SetDirectoryCount(Page* page, std::uint32_t count) {
  WriteU32(page->data() + kOffCount, count);
}

void SetDirectoryNext(Page* page, Page::PageId next) {
  WriteU32(page->data() + kOffNext, next);
}

void SetDirectoryIdAt(Page* page, std::uint32_t index, Page::PageId id) {
  WriteU32(page->data() + kOffIds +
               static_cast<std::size_t>(index) * HeapTable::kDirectoryIdSize,
           id);
}

// Bridges a HeapTable::ScanFn to SlottedPage::Scan, carrying the page id and
// recording whether the visitor asked to stop.
struct ScanBridge {
  Page::PageId page;
  HeapTable::ScanFn fn;
  void* ctx;
  bool stopped;
};

bool ScanBridgeCb(SlottedPage::SlotId slot, const std::uint8_t* data,
                  std::size_t len, void* vctx) {
  ScanBridge* b = static_cast<ScanBridge*>(vctx);
  HeapTable::RecordId id;
  id.page = b->page;
  id.slot = slot;
  if (!b->fn(id, data, len, b->ctx)) {
    b->stopped = true;
    return false;
  }
  return true;
}

}  // namespace

Status HeapTable::Create(PageStorage* storage, BufferPool* pool,
                         std::unique_ptr<HeapTable>* out,
                         Page::PageId* out_root) {
  if (storage == nullptr || pool == nullptr || out == nullptr) {
    return Bad("HeapTable::Create: null storage, pool, or out");
  }

  Page root_page;
  Status s = storage->AllocatePage(&root_page);
  if (!s.ok()) {
    return s;
  }
  InitDirectoryPage(&root_page);
  s = storage->WritePage(root_page);
  if (!s.ok()) {
    return s;
  }

  std::unique_ptr<HeapTable> table(
      new HeapTable(storage, pool, root_page.id()));
  s = table->LoadDirectory();
  if (!s.ok()) {
    return s;
  }
  if (out_root != nullptr) {
    *out_root = root_page.id();
  }
  *out = std::move(table);
  return Status::Ok();
}

Status HeapTable::Open(PageStorage* storage, BufferPool* pool,
                       Page::PageId root, std::unique_ptr<HeapTable>* out) {
  if (storage == nullptr || pool == nullptr || out == nullptr) {
    return Bad("HeapTable::Open: null storage, pool, or out");
  }
  if (root == Page::kInvalidId) {
    return Bad("HeapTable::Open: invalid root page id");
  }
  std::unique_ptr<HeapTable> table(new HeapTable(storage, pool, root));
  Status s = table->LoadDirectory();
  if (!s.ok()) {
    return s;
  }
  *out = std::move(table);
  return Status::Ok();
}

Status HeapTable::LoadDirectory() {
  data_pages_.clear();
  last_dir_ = Page::kInvalidId;
  last_dir_count_ = 0;
  last_data_page_ = Page::kInvalidId;

  std::uint32_t total_pages = 0;
  Status s = storage_->NumPages(&total_pages);
  if (!s.ok()) {
    return s;
  }
  // No valid chain can be longer than the number of pages in the file; the
  // +2 keeps the bound strictly above any legitimate chain.
  const std::uint32_t max_walk = total_pages + 2;

  Page::PageId dir = root_;
  Page::PageId prev = Page::kInvalidId;
  std::uint32_t walked = 0;

  for (;;) {
    if (walked++ > max_walk) {
      return Status::Error(DbError::kBadState,
                           "heap table: directory chain is cyclic");
    }

    BufferPool::Handle handle;
    s = pool_->Fetch(dir, &handle);
    if (!s.ok()) {
      return s;
    }
    s = ValidateDirectoryHeader(handle.page());
    if (!s.ok()) {
      return s;
    }

    const std::uint32_t count = DirectoryCount(handle.page());
    const std::uint32_t next = DirectoryNext(handle.page());
    for (std::uint32_t i = 0; i < count; ++i) {
      const Page::PageId pid = DirectoryIdAt(handle.page(), i);
      if (pid == Page::kInvalidId || pid == root_) {
        return Status::Error(DbError::kBadState,
                             "heap table: invalid data page id in directory");
      }
      if (!storage_->PageExists(pid)) {
        return Status::Error(DbError::kBadState,
                             "heap table: directory references a missing page");
      }
      data_pages_.push_back(pid);
    }
    last_dir_ = dir;
    last_dir_count_ = count;

    if (next == Page::kInvalidId) {
      break;
    }
    if (next == dir || next == prev) {
      return Status::Error(DbError::kBadState,
                           "heap table: directory chain loops on itself");
    }
    prev = dir;
    dir = next;
  }

  last_data_page_ =
      data_pages_.empty() ? Page::kInvalidId : data_pages_.back();
  return Status::Ok();
}

bool HeapTable::IsDataPage(Page::PageId page) const {
  for (Page::PageId id : data_pages_) {
    if (id == page) {
      return true;
    }
  }
  return false;
}

Status HeapTable::AllocateDataPage(Page::PageId* out) {
  Page page;
  Status s = storage_->AllocatePage(&page);
  if (!s.ok()) {
    return s;
  }
  SlottedPage::Init(&page);
  // Persist the SlottedPage header now so a later BufferPool::Fetch reads a
  // valid slotted page rather than the zeroes AllocatePage wrote.
  s = storage_->WritePage(page);
  if (!s.ok()) {
    return s;
  }
  *out = page.id();
  return Status::Ok();
}

Status HeapTable::AppendDataPageId(Page::PageId id) {
  if (last_dir_count_ < kDirectoryCapacity) {
    BufferPool::Handle handle;
    Status s = pool_->Fetch(last_dir_, &handle);
    if (!s.ok()) {
      return s;
    }
    s = ValidateDirectoryHeader(handle.page());
    if (!s.ok()) {
      return s;
    }
    SetDirectoryIdAt(&handle.page(), last_dir_count_, id);
    SetDirectoryCount(&handle.page(), last_dir_count_ + 1);
    handle.MarkDirty();
    ++last_dir_count_;
    data_pages_.push_back(id);
    return Status::Ok();
  }

  // The last directory page is full: allocate and link a new one, then use it.
  Page new_dir;
  Status s = storage_->AllocatePage(&new_dir);
  if (!s.ok()) {
    return s;
  }
  InitDirectoryPage(&new_dir);
  s = storage_->WritePage(new_dir);
  if (!s.ok()) {
    return s;
  }

  {
    BufferPool::Handle handle;
    s = pool_->Fetch(last_dir_, &handle);
    if (!s.ok()) {
      return s;
    }
    SetDirectoryNext(&handle.page(), new_dir.id());
    handle.MarkDirty();
  }

  last_dir_ = new_dir.id();
  last_dir_count_ = 0;
  {
    BufferPool::Handle handle;
    s = pool_->Fetch(last_dir_, &handle);
    if (!s.ok()) {
      return s;
    }
    SetDirectoryIdAt(&handle.page(), 0, id);
    SetDirectoryCount(&handle.page(), 1);
    handle.MarkDirty();
  }
  last_dir_count_ = 1;
  data_pages_.push_back(id);
  return Status::Ok();
}

Status HeapTable::TryInsertIntoPage(Page::PageId page, const std::uint8_t* data,
                                    std::size_t len, RecordId* out) {
  BufferPool::Handle handle;
  Status s = pool_->Fetch(page, &handle);
  if (!s.ok()) {
    return s;
  }
  SlottedPage sp(&handle.page());
  SlottedPage::SlotId slot = SlottedPage::kInvalidSlot;
  s = sp.Insert(data, len, &slot);  // marks the page dirty on success
  if (!s.ok()) {
    return s;
  }
  out->page = page;
  out->slot = slot;
  return Status::Ok();
}

Status HeapTable::Insert(const std::uint8_t* data, std::size_t len,
                         RecordId* out) {
  if (out != nullptr) {
    *out = RecordId();
  }
  if (storage_ == nullptr || pool_ == nullptr) {
    return Bad("HeapTable::Insert: table is not attached to storage/pool");
  }
  if (data == nullptr || len == 0) {
    return Bad("HeapTable::Insert: null or empty record");
  }
  if (len > SlottedPage::kMaxRecordSize) {
    return Status::Error(DbError::kBufferFull,
                         "record is larger than a page can ever hold");
  }

  // Try the most recently used page first, then any other page with room, then
  // allocate. kBufferFull from a page means "no room here"; any other error is
  // fatal (propagated unchanged).
  if (last_data_page_ != Page::kInvalidId) {
    Status s = TryInsertIntoPage(last_data_page_, data, len, out);
    if (s.ok()) {
      return s;
    }
    if (s.code != DbError::kBufferFull) {
      return s;
    }
  }
  for (Page::PageId page : data_pages_) {
    if (page == last_data_page_) {
      continue;
    }
    Status s = TryInsertIntoPage(page, data, len, out);
    if (s.ok()) {
      last_data_page_ = page;
      return s;
    }
    if (s.code != DbError::kBufferFull) {
      return s;
    }
  }

  // Every existing page is full (or unavailable): allocate a new data page.
  // Insert into it BEFORE adding it to the directory, so a failed insert never
  // leaves a half-described page behind.
  Page::PageId fresh = Page::kInvalidId;
  Status s = AllocateDataPage(&fresh);
  if (!s.ok()) {
    return s;
  }
  s = TryInsertIntoPage(fresh, data, len, out);
  if (!s.ok()) {
    return s;
  }
  s = AppendDataPageId(fresh);
  if (!s.ok()) {
    return s;
  }
  last_data_page_ = fresh;
  return Status::Ok();
}

Status HeapTable::Get(const RecordId& id, const std::uint8_t** out_data,
                      std::size_t* out_len) const {
  if (out_data != nullptr) {
    *out_data = nullptr;
  }
  if (out_len != nullptr) {
    *out_len = 0;
  }
  if (out_data == nullptr || out_len == nullptr) {
    return Bad("HeapTable::Get: null out-parameter");
  }
  if (!id.valid()) {
    return Bad("HeapTable::Get: invalid record id");
  }
  if (!IsDataPage(id.page)) {
    return Status::Error(DbError::kNotFound,
                         "heap table: record id does not name a table page");
  }

  BufferPool::Handle handle;
  Status s = pool_->Fetch(id.page, &handle);
  if (!s.ok()) {
    return s;
  }
  SlottedPage sp(&handle.page());
  return sp.Read(id.slot, out_data, out_len);
}

Status HeapTable::Update(const RecordId& id, const std::uint8_t* data,
                         std::size_t len) {
  if (data == nullptr || len == 0) {
    return Bad("HeapTable::Update: null or empty record");
  }
  if (len > SlottedPage::kMaxRecordSize) {
    return Status::Error(DbError::kBufferFull,
                         "record is larger than a page can ever hold");
  }
  if (!id.valid()) {
    return Bad("HeapTable::Update: invalid record id");
  }
  if (!IsDataPage(id.page)) {
    return Status::Error(DbError::kNotFound,
                         "heap table: record id does not name a table page");
  }

  BufferPool::Handle handle;
  Status s = pool_->Fetch(id.page, &handle);
  if (!s.ok()) {
    return s;
  }
  SlottedPage sp(&handle.page());
  return sp.Update(id.slot, data, len);
}

Status HeapTable::Delete(const RecordId& id) {
  if (!id.valid()) {
    return Bad("HeapTable::Delete: invalid record id");
  }
  if (!IsDataPage(id.page)) {
    return Status::Error(DbError::kNotFound,
                         "heap table: record id does not name a table page");
  }

  BufferPool::Handle handle;
  Status s = pool_->Fetch(id.page, &handle);
  if (!s.ok()) {
    return s;
  }
  SlottedPage sp(&handle.page());
  return sp.Delete(id.slot);
}

Status HeapTable::Scan(ScanFn fn, void* ctx) const {
  if (fn == nullptr) {
    return Bad("HeapTable::Scan: null callback");
  }

  for (Page::PageId page : data_pages_) {
    BufferPool::Handle handle;
    Status s = pool_->Fetch(page, &handle);
    if (!s.ok()) {
      return s;
    }
    SlottedPage sp(&handle.page());
    ScanBridge bridge;
    bridge.page = page;
    bridge.fn = fn;
    bridge.ctx = ctx;
    bridge.stopped = false;
    s = sp.Scan(&ScanBridgeCb, &bridge);
    if (!s.ok()) {
      return s;
    }
    if (bridge.stopped) {
      break;
    }
  }
  return Status::Ok();
}

}  // namespace nexusdb
