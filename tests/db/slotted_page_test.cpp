// NexusDB — R1.1 slotted-page test.
//
// Deterministic, in-memory tests over a bare Page (no storage/disk involved).
// Covers: empty-page init+validate, insert/read round trips, multiple
// variable-length records, page-full behaviour with no partial corruption,
// delete/tombstone semantics, compaction with stable slot ids, update with
// smaller/equal/larger records, invalid slots and malformed headers/directories,
// boundary-length records with arithmetic-overflow protection, and scanning.
//
// Same plain PASS/FAIL style as the other DB tests: one check per line, exit 0
// when every check passes, 1 otherwise. No timing assumptions anywhere.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "common/config.h"
#include "common/error.h"
#include "db/page.h"
#include "db/slotted_page.h"

namespace {

using nexusdb::DbError;
using nexusdb::Page;
using nexusdb::SlottedPage;
using nexusdb::Status;

int g_failures = 0;

#define CHECK(desc, expr)                     \
  do {                                        \
    if (expr) {                               \
      printf("PASS: %s\n", desc);             \
    } else {                                  \
      printf("FAIL: %s\n", desc);             \
      ++g_failures;                           \
    }                                         \
  } while (0)

// Documented on-page header offsets (see include/db/slotted_page.h).
constexpr std::size_t kOffMagic = 0;
constexpr std::size_t kOffVersion = 4;
constexpr std::size_t kOffSlotCount = 6;
constexpr std::size_t kOffFreeStart = 8;
constexpr std::size_t kOffFreeEnd = 10;
constexpr std::size_t kSlot0 = SlottedPage::kHeaderSize;

void RawWriteU16(std::uint8_t* p, std::uint16_t v) {
  p[0] = static_cast<std::uint8_t>(v & 0xFFu);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
}

// Fills `n` bytes at `p` with a repeatable pattern seeded by `seed`.
void FillPattern(std::uint8_t* p, std::size_t n, std::uint8_t seed) {
  for (std::size_t i = 0; i < n; ++i) {
    p[i] = static_cast<std::uint8_t>(seed + static_cast<std::uint8_t>(i));
  }
}

bool MatchesPattern(const std::uint8_t* p, std::size_t n, std::uint8_t seed) {
  for (std::size_t i = 0; i < n; ++i) {
    if (p[i] != static_cast<std::uint8_t>(seed + static_cast<std::uint8_t>(i))) {
      return false;
    }
  }
  return true;
}

// Reads a record and checks its length and byte pattern in one step.
bool RecordMatches(const SlottedPage& sp, SlottedPage::SlotId slot,
                   std::size_t expect_len, std::uint8_t seed) {
  const std::uint8_t* data = nullptr;
  std::size_t len = 0;
  if (!sp.Read(slot, &data, &len).ok()) {
    return false;
  }
  return len == expect_len && MatchesPattern(data, len, seed);
}

// Inserts a patterned record of `len` bytes.
Status InsertPattern(SlottedPage& sp, std::size_t len, std::uint8_t seed,
                     SlottedPage::SlotId* out_slot) {
  std::uint8_t buf[nexusdb::kPageSize];
  FillPattern(buf, len, seed);
  return sp.Insert(buf, len, out_slot);
}

// Scan collector used by the scan tests.
struct ScanCollector {
  SlottedPage::SlotId slots[64];
  std::size_t count;
  std::size_t stop_after;  // 0 = never stop early
};

bool CollectScan(SlottedPage::SlotId slot, const std::uint8_t* data,
                 std::size_t len, void* ctx) {
  ScanCollector* c = static_cast<ScanCollector*>(ctx);
  if (c->count < 64) {
    c->slots[c->count] = slot;
  }
  ++c->count;
  if (c->stop_after != 0 && c->count >= c->stop_after) {
    return false;
  }
  (void)data;
  (void)len;
  return true;
}

}  // namespace

int main() {
  printf("=== NexusDB R1.1: slotted-page test ===\n");
  printf("page size: %u, header: %zu, slot entry: %zu, max record: %zu\n\n",
         nexusdb::kPageSize, SlottedPage::kHeaderSize,
         SlottedPage::kSlotEntrySize, SlottedPage::kMaxRecordSize);

  // ---- 1. Empty-page initialization and validation ----
  {
    Page p;
    CHECK("Init succeeds", SlottedPage::Init(&p).ok());
    CHECK("Init marks page dirty", p.IsDirty());

    SlottedPage sp(&p);
    CHECK("empty page validates", sp.Validate().ok());
    CHECK("static Validate agrees", SlottedPage::Validate(p).ok());
    CHECK("empty page has 0 slots", sp.slot_count() == 0);
    CHECK("empty page has 0 live records", sp.live_count() == 0);
    CHECK("empty page free bytes == page - header",
          sp.free_bytes() == nexusdb::kPageSize - SlottedPage::kHeaderSize);

    // Re-initializing a dirty, garbage-filled page must fully reset it.
    std::memset(p.data(), 0xAB, Page::size());
    CHECK("re-Init on garbage succeeds", SlottedPage::Init(&p).ok());
    CHECK("re-Init yields a valid empty page",
          sp.Validate().ok() && sp.slot_count() == 0 && sp.live_count() == 0);
  }

  // ---- 2. Insert / read round trip ----
  {
    Page p;
    SlottedPage::Init(&p);
    SlottedPage sp(&p);
    const std::size_t before = sp.free_bytes();

    SlottedPage::SlotId slot = SlottedPage::kInvalidSlot;
    CHECK("insert returns ok", InsertPattern(sp, 5, 1, &slot).ok());
    CHECK("first slot id is 0", slot == 0);
    CHECK("read back inserted record matches", RecordMatches(sp, 0, 5, 1));
    CHECK("insert consumes slot entry + record",
          sp.free_bytes() == before - (SlottedPage::kSlotEntrySize + 5));
    CHECK("page still validates after insert", sp.Validate().ok());
  }

  // ---- 3. Multiple variable-length records ----
  {
    Page p;
    SlottedPage::Init(&p);
    SlottedPage sp(&p);
    const std::size_t lens[5] = {1, 7, 40, 300, 4096 / 4};
    for (std::size_t i = 0; i < 5; ++i) {
      SlottedPage::SlotId slot = SlottedPage::kInvalidSlot;
      CHECK("insert variable record ok",
            InsertPattern(sp, lens[i], static_cast<std::uint8_t>(i * 10 + 1),
                          &slot)
                .ok());
      CHECK("variable record slot id is sequential",
            slot == static_cast<SlottedPage::SlotId>(i));
    }
    bool all_match = true;
    for (std::size_t i = 0; i < 5; ++i) {
      if (!RecordMatches(sp, static_cast<SlottedPage::SlotId>(i), lens[i],
                         static_cast<std::uint8_t>(i * 10 + 1))) {
        all_match = false;
      }
    }
    CHECK("all variable records read back intact", all_match);
    CHECK("live count matches inserts", sp.live_count() == 5);
  }

  // ---- 4. Page-full behaviour without partial corruption ----
  {
    Page p;
    SlottedPage::Init(&p);
    SlottedPage sp(&p);

    // 64-byte records: 60 fit exactly (60 * 68 == 4080 free bytes).
    const std::size_t rec = 64;
    std::size_t inserted = 0;
    Status last = Status::Ok();
    for (std::size_t i = 0; i < 200; ++i) {
      SlottedPage::SlotId slot = SlottedPage::kInvalidSlot;
      last = InsertPattern(sp, rec, static_cast<std::uint8_t>(i + 1), &slot);
      if (!last.ok()) {
        break;
      }
      ++inserted;
    }
    CHECK("inserts fill the page (60 records)", inserted == 60);
    CHECK("overflow insert fails with kBufferFull",
          !last.ok() && last.code == DbError::kBufferFull);
    CHECK("page is exactly full", sp.free_bytes() == 0);
    CHECK("overflow did not add a slot", sp.slot_count() == 60);
    CHECK("page still validates after overflow", sp.Validate().ok());

    bool intact = true;
    for (std::size_t i = 0; i < inserted; ++i) {
      if (!RecordMatches(sp, static_cast<SlottedPage::SlotId>(i), rec,
                         static_cast<std::uint8_t>(i + 1))) {
        intact = false;
      }
    }
    CHECK("all records survive the failed insert", intact);
  }

  // ---- 5. Delete and tombstone behaviour ----
  {
    Page p;
    SlottedPage::Init(&p);
    SlottedPage sp(&p);
    SlottedPage::SlotId s0 = 0, s1 = 0, s2 = 0;
    InsertPattern(sp, 10, 11, &s0);
    InsertPattern(sp, 20, 21, &s1);
    InsertPattern(sp, 30, 31, &s2);

    CHECK("delete live record ok", sp.Delete(s1).ok());
    const std::uint8_t* data = nullptr;
    std::size_t len = 0;
    Status r = sp.Read(s1, &data, &len);
    CHECK("reading a tombstone fails with kNotFound",
          !r.ok() && r.code == DbError::kNotFound);
    CHECK("tombstone zeroes out-params", data == nullptr && len == 0);
    CHECK("live count drops after delete", sp.live_count() == 2);
    CHECK("slot count keeps the tombstone", sp.slot_count() == 3);
    CHECK("survivors still readable",
          RecordMatches(sp, s0, 10, 11) && RecordMatches(sp, s2, 30, 31));

    Status again = sp.Delete(s1);
    CHECK("deleting a tombstone fails with kNotFound",
          !again.ok() && again.code == DbError::kNotFound);

    Status oob = sp.Delete(99);
    CHECK("deleting an out-of-range slot fails with kInvalidArgument",
          !oob.ok() && oob.code == DbError::kInvalidArgument);
    CHECK("page validates with a tombstone", sp.Validate().ok());
  }

  // ---- 6. Compaction and slot-id stability ----
  {
    Page p;
    SlottedPage::Init(&p);
    SlottedPage sp(&p);
    SlottedPage::SlotId a = 0, b = 0, c = 0;
    InsertPattern(sp, 100, 1, &a);
    InsertPattern(sp, 200, 2, &b);
    InsertPattern(sp, 300, 3, &c);
    const std::size_t free_before_delete = sp.free_bytes();
    sp.Delete(b);
    CHECK("delete does not change contiguous free bytes",
          sp.free_bytes() == free_before_delete);

    CHECK("compact succeeds", sp.Compact().ok());
    CHECK("compaction reclaims the tombstoned bytes",
          sp.free_bytes() == free_before_delete + 200);
    CHECK("compaction keeps slot count unchanged", sp.slot_count() == 3);
    CHECK("slot ids stable after compaction",
          RecordMatches(sp, a, 100, 1) && RecordMatches(sp, c, 300, 3));
    const std::uint8_t* data = nullptr;
    std::size_t len = 0;
    Status r = sp.Read(b, &data, &len);
    CHECK("tombstone stays deleted after compaction",
          !r.ok() && r.code == DbError::kNotFound);
    CHECK("page validates after compaction", sp.Validate().ok());

    // Compacting an already-compact page is a no-op that still succeeds.
    const std::size_t free_after = sp.free_bytes();
    CHECK("double compaction is a clean no-op",
          sp.Compact().ok() && sp.free_bytes() == free_after);
  }

  // ---- 7. Update with smaller, equal, and larger records ----
  {
    Page p;
    SlottedPage::Init(&p);
    SlottedPage sp(&p);
    SlottedPage::SlotId s = SlottedPage::kInvalidSlot;
    InsertPattern(sp, 6, 100, &s);  // "length 6"

    SlottedPage::SlotId s2 = SlottedPage::kInvalidSlot;
    InsertPattern(sp, 3, 200, &s2);
    Status upd = Status::Ok();

    std::uint8_t small[3];
    FillPattern(small, 3, 40);
    upd = sp.Update(s, small, 3);
    CHECK("update to smaller record succeeds", upd.ok());
    CHECK("smaller update reads back", RecordMatches(sp, s, 3, 40));

    std::uint8_t equal[3];
    FillPattern(equal, 3, 50);
    CHECK("update to equal size succeeds", sp.Update(s, equal, 3).ok());
    CHECK("equal update reads back", RecordMatches(sp, s, 3, 50));

    std::uint8_t bigger[500];
    FillPattern(bigger, 500, 60);
    CHECK("update to larger record relocates and succeeds",
          sp.Update(s, bigger, 500).ok());
    CHECK("larger update reads back", RecordMatches(sp, s, 500, 60));
    CHECK("neighbour record untouched by relocation",
          RecordMatches(sp, s2, 3, 200));
    CHECK("page validates after updates", sp.Validate().ok());

    // Out-of-range and tombstoned updates must fail cleanly.
    Status oob = sp.Update(12345, small, 3);
    CHECK("update out-of-range fails with kInvalidArgument",
          !oob.ok() && oob.code == DbError::kInvalidArgument);
    CHECK("delete then update fails with kNotFound",
          sp.Delete(s).ok() && sp.Update(s, small, 3).code ==
                                   DbError::kNotFound);
  }

  // ---- 8. Invalid slots and malformed headers / directories ----
  {
    Page p;
    SlottedPage::Init(&p);
    SlottedPage sp(&p);
    SlottedPage::SlotId s = 0;
    InsertPattern(sp, 8, 77, &s);

    const std::uint8_t* data = nullptr;
    std::size_t len = 0;
    Status r = sp.Read(3, &data, &len);
    CHECK("read of out-of-range slot fails with kInvalidArgument",
          !r.ok() && r.code == DbError::kInvalidArgument);

    // Bad magic.
    Page bad = p;
    bad.data()[kOffMagic] = static_cast<std::uint8_t>(bad.data()[kOffMagic] ^ 0xFF);
    CHECK("bad magic fails validation",
          SlottedPage::Validate(bad).code == DbError::kInvalidArgument);
    CHECK("ops on bad magic fail", !SlottedPage(&bad).Read(0, &data, &len).ok());

    // Bad version.
    Page badver = p;
    RawWriteU16(badver.data() + kOffVersion, 2);
    CHECK("unsupported version fails validation",
          !SlottedPage::Validate(badver).ok());

    // Slot directory overflows the page.
    Page bigdir = p;
    RawWriteU16(bigdir.data() + kOffSlotCount, 60000);
    CHECK("oversized slot directory fails validation",
          !SlottedPage::Validate(bigdir).ok());

    // free_start overlapping the directory.
    Page clash = p;
    RawWriteU16(clash.data() + kOffFreeStart, 16);  // dir_end is 20 (1 slot)
    CHECK("free_start overlapping directory fails validation",
          !SlottedPage::Validate(clash).ok());

    // free_end precedes free_start.
    Page inverted = p;
    RawWriteU16(inverted.data() + kOffFreeEnd, 4);
    CHECK("free_end before free_start fails validation",
          !SlottedPage::Validate(inverted).ok());

    // Live slot pointing into the free region.
    Page intofree = p;
    RawWriteU16(intofree.data() + kSlot0, 0);
    CHECK("live slot into the free region fails validation",
          !SlottedPage::Validate(intofree).ok());

    // Live slot length extending beyond the page (also an overflow probe).
    Page overrun = p;
    RawWriteU16(overrun.data() + kOffFreeStart, 20);
    RawWriteU16(overrun.data() + kOffFreeEnd, 20);
    RawWriteU16(overrun.data() + kSlot0, 0xFFF0);
    RawWriteU16(overrun.data() + kSlot0 + 2, 0x00FF);
    Status ov = SlottedPage::Validate(overrun);
    CHECK("slot off+len beyond the page fails validation",
          !ov.ok() && ov.code == DbError::kInvalidArgument);

    // All-garbage page.
    Page junk;
    std::memset(junk.data(), 0xCD, Page::size());
    CHECK("garbage page fails validation", !SlottedPage::Validate(junk).ok());

    // Null page: every operation must fail, never crash.
    SlottedPage nil(nullptr);
    CHECK("null page Validate fails", !nil.Validate().ok());
    CHECK("null page Insert fails",
          !nil.Insert(reinterpret_cast<const std::uint8_t*>("x"), 1, nullptr)
               .ok());
    CHECK("null page Read fails", !nil.Read(0, &data, &len).ok());
    CHECK("null page Delete fails", !nil.Delete(0).ok());
    CHECK("null page Compact fails", !nil.Compact().ok());
    CHECK("null page Scan fails", !nil.Scan(CollectScan, nullptr).ok());
    CHECK("null page accessors return 0",
          nil.slot_count() == 0 && nil.live_count() == 0 &&
              nil.free_bytes() == 0);

    // Null page pointer passed to the static API.
    CHECK("static Init(null) fails", !SlottedPage::Init(nullptr).ok());
  }

  // ---- 9. Boundary-length records and arithmetic-overflow protection ----
  {
    Page p;
    SlottedPage::Init(&p);
    SlottedPage sp(&p);

    SlottedPage::SlotId s = SlottedPage::kInvalidSlot;
    std::uint8_t big[SlottedPage::kMaxRecordSize];
    FillPattern(big, sizeof(big), 1);
    CHECK("insert of the maximum record succeeds", sp.Insert(big, sizeof(big), &s).ok());
    CHECK("maximum record reads back intact",
          RecordMatches(sp, s, sizeof(big), 1));
    CHECK("page is exactly full with the maximum record", sp.free_bytes() == 0);

    SlottedPage::SlotId s2 = SlottedPage::kInvalidSlot;
    std::uint8_t one[1] = {0x5A};
    Status full = sp.Insert(one, 1, &s2);
    CHECK("any further insert fails with kBufferFull",
          !full.ok() && full.code == DbError::kBufferFull);

    // Over-maximum length is rejected up front.
    Page p2;
    SlottedPage::Init(&p2);
    SlottedPage sp2(&p2);
    std::uint8_t too[SlottedPage::kMaxRecordSize + 1];
    FillPattern(too, sizeof(too), 1);
    Status toolong = sp2.Insert(too, sizeof(too), &s2);
    CHECK("over-maximum record rejected with kBufferFull",
          !toolong.ok() && toolong.code == DbError::kBufferFull);
    CHECK("rejected over-max insert added no slot", sp2.slot_count() == 0);

    // Null / empty record arguments.
    Status nullrec = sp2.Insert(nullptr, 5, &s2);
    Status emptyrec = sp2.Insert(one, 0, &s2);
    CHECK("null record rejected", !nullrec.ok() &&
                                      nullrec.code == DbError::kInvalidArgument);
    CHECK("empty record rejected", !emptyrec.ok() &&
                                       emptyrec.code == DbError::kInvalidArgument);

    // Corner offsets: a crafted slot just fitting the page validates; one byte
    // too long never does, and never overflows the 16-bit arithmetic.
    Page edge;
    SlottedPage::Init(&edge);
    RawWriteU16(edge.data() + kOffSlotCount, 1);
    RawWriteU16(edge.data() + kOffFreeStart, 20);
    RawWriteU16(edge.data() + kOffFreeEnd, 20);
    RawWriteU16(edge.data() + kSlot0, static_cast<std::uint16_t>(Page::size() - 8));
    RawWriteU16(edge.data() + kSlot0 + 2, 8);
    CHECK("a slot exactly reaching the page end validates",
          SlottedPage::Validate(edge).ok());
    RawWriteU16(edge.data() + kSlot0 + 2, 9);
    CHECK("a slot one byte past the page end fails validation",
          !SlottedPage::Validate(edge).ok());
  }

  // ---- 10. Scanning live records ----
  {
    Page p;
    SlottedPage::Init(&p);
    SlottedPage sp(&p);
    SlottedPage::SlotId a = 0, b = 0, c = 0;
    InsertPattern(sp, 4, 1, &a);
    InsertPattern(sp, 8, 2, &b);
    InsertPattern(sp, 16, 3, &c);

    ScanCollector all{};
    const Status s = sp.Scan(CollectScan, &all);
    CHECK("scan succeeds", s.ok());
    CHECK("scan yields every live slot in order",
          all.count == 3 && all.slots[0] == a && all.slots[1] == b &&
              all.slots[2] == c);

    sp.Delete(b);
    ScanCollector after{};
    sp.Scan(CollectScan, &after);
    CHECK("scan skips tombstones",
          after.count == 2 && after.slots[0] == a && after.slots[1] == c);

    ScanCollector early{};
    early.stop_after = 1;
    CHECK("scan can stop early and still succeed",
          sp.Scan(CollectScan, &early).ok() && early.count == 1);

    CHECK("scan rejects a null callback",
          sp.Scan(nullptr, &all).code == DbError::kInvalidArgument);
  }

  printf("\n=== %s (%d failure(s)) ===\n",
         g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
