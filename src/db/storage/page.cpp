// NexusDB — Page lifecycle implementation (Phase 1).

#include "db/page.h"

namespace nexusdb {

Page::Page() : id_(kInvalidId), dirty_(false), data_{} {}

Page::Page(PageId id) : id_(id), dirty_(false), data_{} {}

void Page::Reset(PageId id) {
  id_ = id;
  dirty_ = false;
  data_.fill(0);
}

}  // namespace nexusdb
