// NexusDB — Phase 1 storage test: Page + PageStorage on real disk.
//
// Exercises: empty-db allocation (first id == 0), multi-page allocation,
// write -> close -> reopen -> read persistence with exact byte equality,
// plus invalid-id / beyond-EOF / bad-path / use-after-close errors.
//
// No in-memory fake: every check goes through a real database file in the
// OS temp directory, removed before and after the run.
//
// Same plain style as tests/container/container_test.c: PASS/FAIL per
// check, exit 0 on success, 1 on any failure.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#include "common/config.h"
#include "db/page.h"
#include "db/page_storage.h"

namespace {

using nexusdb::Page;
using nexusdb::PageStorage;
using nexusdb::Status;

int g_failures = 0;

#define CHECK(desc, expr)                                     \
  do {                                                        \
    if (expr) {                                               \
      printf("PASS: %s\n", desc);                             \
    } else {                                                  \
      printf("FAIL: %s\n", desc);                             \
      g_failures++;                                           \
    }                                                         \
  } while (0)

std::string TestDbPath() {
  return (std::filesystem::temp_directory_path() / "nexusdb_storage_test.db")
      .string();
}

// Removes the test database file if present (fresh start / cleanup).
void RemoveDb(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

// Fills a page with a repeating byte pattern for exact-match verification.
void FillPattern(Page* page, unsigned char byte) {
  std::memset(page->data(), byte, Page::size());
  page->MarkDirty();
}

bool MatchesPattern(const Page& page, unsigned char byte) {
  for (std::size_t i = 0; i < Page::size(); ++i) {
    if (page.data()[i] != byte) {
      return false;
    }
  }
  return true;
}

}  // namespace

int main() {
  printf("=== NexusDB Phase 1: Page + PageStorage test ===\n\n");
  printf("page size: %u bytes\n\n", nexusdb::kPageSize);

  const std::string path = TestDbPath();
  RemoveDb(path);

  // ---- 0. Page basics (no storage involved) ----
  {
    Page fresh;
    CHECK("default page is invalid", !fresh.IsValid());
    CHECK("default page is clean", !fresh.IsDirty());
    CHECK("default page data zeroed",
          std::memcmp(fresh.data(), std::string(Page::size(), '\0').data(),
                      Page::size()) == 0);

    Page p(7);
    CHECK("explicit page holds id", p.IsValid() && p.id() == 7);
    p.MarkDirty();
    CHECK("mark dirty sticks", p.IsDirty());
    p.ClearDirty();
    CHECK("clear dirty sticks", !p.IsDirty());
    p.Reset(3);
    CHECK("reset reinitializes id and zeroes",
          p.IsValid() && p.id() == 3 && !p.IsDirty() &&
              p.data()[0] == 0 && p.data()[Page::size() - 1] == 0);
  }

  // ---- 1. Open errors ----
  {
    std::unique_ptr<PageStorage> db;
    Status s = PageStorage::Open("", &db);
    CHECK("empty path rejected", !s.ok());
    s = PageStorage::Open(path, nullptr);
    CHECK("null out rejected", !s.ok());
  }

  // ---- 2. Empty database: first allocation is id 0 ----
  {
    std::unique_ptr<PageStorage> db;
    CHECK("open fresh db", PageStorage::Open(path, &db).ok());
    std::uint32_t n = 99;
    CHECK("fresh db has 0 pages",
          db->NumPages(&n).ok() && n == 0);
    CHECK("page 0 does not exist yet", !db->PageExists(0));

    Page p0;
    CHECK("allocate on empty db", db->AllocatePage(&p0).ok());
    CHECK("first allocated page id == 0", p0.IsValid() && p0.id() == 0);
    CHECK("allocated page is zeroed and clean",
          !p0.IsDirty() && p0.data()[0] == 0 &&
              p0.data()[Page::size() - 1] == 0);
    CHECK("page 0 exists after allocate", db->PageExists(0));
    CHECK("read beyond EOF fails",
          !db->ReadPage(1, &p0).ok());
    CHECK("close works", db->Close().ok());
    CHECK("double close is ok", db->Close().ok());
    CHECK("use after close fails",
          !db->NumPages(&n).ok() && !db->PageExists(0));
  }
  RemoveDb(path);

  // ---- 3. Persistence: write "Hello NexusDB", close, reopen, verify ----
  {
    const char* kMessage = "Hello NexusDB";
    {
      std::unique_ptr<PageStorage> db;
      CHECK("open for persistence write",
            PageStorage::Open(path, &db).ok());
      Page page;
      CHECK("allocate persistence page", db->AllocatePage(&page).ok());
      std::memcpy(page.data(), kMessage, std::strlen(kMessage) + 1);
      page.MarkDirty();
      CHECK("write persistence page", db->WritePage(page).ok());
      CHECK("close after write", db->Close().ok());
    }
    {
      std::unique_ptr<PageStorage> db;
      CHECK("reopen persistence db", PageStorage::Open(path, &db).ok());
      std::uint32_t n = 0;
      CHECK("reopened db still has 1 page",
            db->NumPages(&n).ok() && n == 1);
      Page back;
      CHECK("read persisted page", db->ReadPage(0, &back).ok());
      CHECK("persisted page id correct", back.id() == 0);
      CHECK("persisted page reads clean", !back.IsDirty());
      CHECK("exact data preserved",
            std::memcmp(back.data(), kMessage, std::strlen(kMessage) + 1) ==
                0);
      // Bytes past the message must be the original zeros.
      bool rest_zero = true;
      for (std::size_t i = std::strlen(kMessage) + 1; i < Page::size(); ++i) {
        if (back.data()[i] != 0) {
          rest_zero = false;
          break;
        }
      }
      CHECK("untouched bytes still zero", rest_zero);
      CHECK("close persistence db", db->Close().ok());
    }
  }
  RemoveDb(path);

  // ---- 4. Multiple pages: allocate 0,1,2, write distinct data, verify ----
  {
    {
      std::unique_ptr<PageStorage> db;
      CHECK("open multi-page db", PageStorage::Open(path, &db).ok());
      for (std::uint32_t want = 0; want < 3; ++want) {
        Page p;
        char desc[64];
        std::snprintf(desc, sizeof(desc), "allocate page %u", want);
        bool ok_alloc = db->AllocatePage(&p).ok();
        CHECK(desc, ok_alloc && p.id() == want);
        FillPattern(&p, static_cast<unsigned char>(want + 1));
        CHECK("write multi page", db->WritePage(p).ok());
      }
      std::uint32_t n = 0;
      CHECK("3 pages allocated", db->NumPages(&n).ok() && n == 3);
      CHECK("close multi-page db", db->Close().ok());
    }
    {
      std::unique_ptr<PageStorage> db;
      CHECK("reopen multi-page db", PageStorage::Open(path, &db).ok());
      for (std::uint32_t want = 0; want < 3; ++want) {
        Page p;
        char desc[64];
        std::snprintf(desc, sizeof(desc), "page %u survives reopen", want);
        CHECK(desc,
              db->ReadPage(want, &p).ok() && p.id() == want &&
                  MatchesPattern(p, static_cast<unsigned char>(want + 1)));
      }
      CHECK("close reopened multi-page db", db->Close().ok());
    }
  }
  RemoveDb(path);

  // ---- 5. Invalid ids and unallocated writes ----
  {
    std::unique_ptr<PageStorage> db;
    CHECK("open error-path db", PageStorage::Open(path, &db).ok());
    Page p;
    CHECK("read from empty db fails", !db->ReadPage(0, &p).ok());
    CHECK("read null page fails", !db->ReadPage(0, nullptr).ok());
    CHECK("huge id read fails",
          !db->ReadPage(Page::kInvalidId, &p).ok());

    Page stray(5);  // never allocated
    FillPattern(&stray, 0xAB);
    CHECK("write to unallocated id fails", !db->WritePage(stray).ok());

    Page invalid;  // default: invalid id
    CHECK("write of invalid page fails", !db->WritePage(invalid).ok());
    CHECK("allocate null fails", !db->AllocatePage(nullptr).ok());
    CHECK("numpages null fails", !db->NumPages(nullptr).ok());

    CHECK("allocate after errors still id 0", db->AllocatePage(&p).ok() &&
                                                  p.id() == 0);
    CHECK("write to allocated page works now",
          db->WritePage(p).ok() && db->ReadPage(0, &p).ok());
    CHECK("close error-path db", db->Close().ok());
  }
  RemoveDb(path);

  printf("\n=== %s (%d failure(s)) ===\n",
         g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
