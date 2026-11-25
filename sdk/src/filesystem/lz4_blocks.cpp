/**
 * rr6-nx (2026-11-10): reading *.lz4b files (rex/filesystem/lz4_blocks.h has the format). 2026-11-11: ZXC blocks
 * (version 2) beside the LZ4 ones, and LZ4 1.10.0's own decoder for those.
 */
#include <rex/filesystem/lz4_blocks.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include <lz4.h>
#include <zxc.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/string.h>
#include <rex/thread.h>  // the read-ahead thread (zumasrevenge-nx, 2026-11-25)

// rr6-nx (2026-11-11): a diagnostic of the codec test (the console runs of 2026-11-10/11 measured loads with it).
REXCVAR_DEFINE_BOOL(lz4b_load_log, false, "Filesystem",
                    "One [lz4b] load line per burst of .lz4b reads (reads less than 1 s apart): its length, the MB read "
                    "from the card and the time in those reads, the MB decoded and the time decoding")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// zumasrevenge-nx (2026-11-25): Zuma's Revenge's game thread loads main.rsb in order while its intro movie plays (one
// 256 KB read a picture, 8-9 ms each from the card, then a 13 MB read 6 MB further on: 200 ms), and the same thread
// presents the movie, which stopped for each. A big .lz4b file read in order is read ahead by a thread of its own, in
// 1 MB host reads, up to this many MB past the game's last read; the game's reads of those blocks come from memory.
REXCVAR_DEFINE_INT32(lz4b_read_ahead_mb, 32, "Filesystem",
                     "A .lz4b file of 64 MB or more that the game reads in order is read ahead by a thread of its own, up "
                     "to this many MB past the game's last read (its reads then come from memory); 0 = off")
    .range(0, 256);

namespace rex::filesystem {

namespace {

constexpr char kMagic[8] = {'R', 'X', 'L', 'Z', '4', 'B', 'L', 'K'};
constexpr char kEndMagic[8] = {'R', 'X', 'L', 'Z', '4', 'E', 'N', 'D'};
constexpr size_t kHeaderSize = 32;
constexpr size_t kFooterSize = 16;
// Whole blocks are read and decoded in runs of up to this much of the file (one host read each).
constexpr uint64_t kRunBytes = 4ull << 20;

uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t Le64(const uint8_t* p) { return uint64_t(Le32(p)) | (uint64_t(Le32(p + 4)) << 32); }

struct Header {
  uint32_t version = 1;
  uint32_t block_size = 0;
  uint64_t size = 0;
  uint32_t block_count = 0;
};

// rr6-nx (2026-11-11): version 1 = LZ4 blocks, version 2 = ZXC blocks (zxc_compress_block), the container the same.
bool ParseHeader(const uint8_t* h, Header* out) {
  if (std::memcmp(h, kMagic, 8) != 0 || (Le32(h + 8) != 1 && Le32(h + 8) != 2)) return false;
  out->version = Le32(h + 8);
  out->block_size = Le32(h + 12);
  out->size = Le64(h + 16);
  out->block_count = Le32(h + 24);
  const uint32_t bs = out->block_size;
  if (bs < 4096 || bs > (16u << 20) || (bs & (bs - 1))) return false;
  if (out->version == 2 && bs > ZXC_BLOCK_SIZE_MAX) return false;
  return out->block_count == (out->size + bs - 1) / bs;
}

bool ReadExactly(FileHandle& f, uint64_t offset, void* to, size_t length) {
  size_t got = 0;
  return f.Read(size_t(offset), to, length, &got) && got == length;
}

using Clock = std::chrono::steady_clock;

int64_t NowNs() {
  static const Clock::time_point start = Clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
}

// zumasrevenge-nx (2026-11-25): the read-ahead's blocks the game's reads took, and the blocks the thread read.
std::atomic<uint64_t> g_ahead_hits{0}, g_ahead_read{0};

// A burst of .lz4b reads less than 1 s apart, logged when the next burst starts (and every 256 MB decoded).
struct LoadLog {
  std::mutex mutex;
  int64_t start_ns = -1, last_ns = 0, read_ns = 0, decode_ns = 0;
  uint64_t reads = 0, packed = 0, raw = 0, blocks = 0, part_blocks = 0, logged_raw = 0;
  uint32_t version = 0;
  // Since the first read, logged at most once a second while reads happen.
  int64_t total_logged_ns = 0, t_read_ns = 0, t_decode_ns = 0;
  uint64_t t_reads = 0, t_packed = 0, t_raw = 0, t_blocks = 0, t_parts = 0;

  void Flush(const char* why) {
    if (start_ns < 0 || !reads) return;
    const double ms = double(last_ns - start_ns) / 1e6;
    REXLOG_INFO("[lz4b] load ({}, v{}): from {:.3f} s, {:.0f} ms long, {} reads; {:.1f} MB from the card in {:.0f} ms "
                "({:.1f} MB/s); {:.1f} MB out ({} blocks, {} part), {:.0f} ms decoding ({:.0f} MB/s)",
                why, version, double(start_ns) / 1e9, ms, reads, double(packed) / 1048576.0, double(read_ns) / 1e6,
                read_ns ? double(packed) / 1048576.0 / (double(read_ns) / 1e9) : 0.0, double(raw) / 1048576.0, blocks,
                part_blocks, double(decode_ns) / 1e6,
                decode_ns ? double(raw) / 1048576.0 / (double(decode_ns) / 1e9) : 0.0);
  }
  void Begin(int64_t now, uint32_t v) {
    std::lock_guard<std::mutex> lock(mutex);
    if (start_ns >= 0 && now - last_ns > 1000000000) {
      Flush("ended");
      start_ns = -1;
    }
    if (start_ns < 0) {
      start_ns = now;
      reads = packed = raw = blocks = part_blocks = logged_raw = 0;
      read_ns = decode_ns = 0;
    }
    version = v;
  }
  void End(int64_t now, uint64_t r_packed, int64_t r_read_ns, uint64_t r_raw, int64_t r_decode_ns, uint32_t r_blocks,
           uint32_t r_parts) {
    std::lock_guard<std::mutex> lock(mutex);
    last_ns = now;
    ++reads;
    packed += r_packed;
    read_ns += r_read_ns;
    raw += r_raw;
    decode_ns += r_decode_ns;
    blocks += r_blocks;
    part_blocks += r_parts;
    if (raw - logged_raw >= (256ull << 20)) {
      Flush("so far");
      logged_raw = raw;
    }
    ++t_reads;
    t_packed += r_packed;
    t_read_ns += r_read_ns;
    t_raw += r_raw;
    t_decode_ns += r_decode_ns;
    t_blocks += r_blocks;
    t_parts += r_parts;
    if (now - total_logged_ns >= 1000000000) {
      total_logged_ns = now;
      REXLOG_INFO("[lz4b] total (v{}) at {:.3f} s: {} reads; {:.1f} MB from the card in {:.0f} ms; {:.1f} MB out ({} blocks, "
                  "{} part) in {:.0f} ms decoding; read-ahead: {} blocks taken by the game's reads, {} read by its thread",
                  version, double(now) / 1e9, t_reads, double(t_packed) / 1048576.0, double(t_read_ns) / 1e6,
                  double(t_raw) / 1048576.0, t_blocks, t_parts, double(t_decode_ns) / 1e6,
                  g_ahead_hits.load(std::memory_order_relaxed), g_ahead_read.load(std::memory_order_relaxed));
    }
  }
};

LoadLog& GetLoadLog() {
  static LoadLog log;
  return log;
}

thread_local bool t_no_read_ahead = false;  // SetLz4BlocksNoReadAheadOnThisThread

void LogBad(const std::filesystem::path& path, const char* why) {
  static std::atomic<uint32_t> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
    REXLOG_ERROR("[lz4b] {}: {}", rex::path_to_utf8(path), why);
}

// One block decoded. `room`: the bytes `to` may be written, at least the block's length (ZXC's decoder writes past a
// block's end on its fast path; with less room it goes through its own buffer and a copy). LZ4's writes nothing past it.
bool DecodeBlockData(uint32_t version, const uint8_t* stored, size_t stored_size, uint32_t length, uint8_t* to,
                     size_t room, zxc_dctx** zxc) {
  if (stored_size == length) {
    std::memcpy(to, stored, length);
    return true;
  }
  if (version == 2) {
    if (!*zxc) *zxc = zxc_create_dctx();
    const size_t cap = std::min(room, zxc_decompress_block_bound(length));
    return *zxc && zxc_decompress_block(*zxc, stored, stored_size, to, cap) == int64_t(length);
  }
  // rr6-nx (2026-11-11): LZ4 1.10.0's own decoder, 2.5x the speed of peggle2-nx's byte loop it replaces on the
  // console (590 against 234 MB/s at the handheld clocks; the same bytes out).
  return LZ4_decompress_safe(reinterpret_cast<const char*>(stored), reinterpret_cast<char*>(to), int(stored_size),
                             int(length)) == int(length);
}

// --- zumasrevenge-nx (2026-11-25): read-ahead (lz4b_read_ahead_mb) ---
constexpr uint64_t kReadAheadMinSize = 64ull << 20;  // only files this big (a game's big archive)
constexpr uint32_t kReadAheadRunBlocks = 4;          // the thread's host reads: 4 blocks (1 MB of 256 KB blocks)
constexpr uint32_t kSequentialBlocks = 16;           // a read starting this few blocks past the last one's end continues it

// A file's read-ahead: the window of blocks the thread fills, [from, to), and the blocks it decoded. The thread holds it
// (and the packed file) as long as it works on it; the handle marks it closed when it goes.
struct ReadAhead {
  std::shared_ptr<FileHandle> packed;
  std::vector<uint64_t> offsets;
  uint32_t version = 1, block_size = 0, block_count = 0;
  uint64_t size = 0;
  std::mutex mutex;
  std::map<uint32_t, std::vector<uint8_t>> blocks;
  uint32_t from = 0, to = 0;
  bool closed = false, failed = false;
  uint64_t hits = 0, read_blocks = 0;
  uint32_t BlockLength(uint32_t b) const {
    return b + 1 < block_count ? block_size : uint32_t(size - uint64_t(b) * block_size);
  }
};

// One thread for every file (the file read most recently in order is the one it works for), never ended.
class ReadAheadWorker {
 public:
  static ReadAheadWorker* Get() {
    static ReadAheadWorker* const worker = Create();
    return worker;
  }
  void Wake(const std::shared_ptr<ReadAhead>& ra) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      active_ = ra;
      dirty_ = true;
    }
    cv_.notify_one();
  }

 private:
  static ReadAheadWorker* Create() {
    auto* worker = new ReadAheadWorker();  // never destroyed: its thread lives as long as the process
    rex::thread::Thread::CreationParameters params;
    params.stack_size = 256 * 1024;
    auto thread = rex::thread::Thread::Create(params, [worker] { worker->Loop(); });
    if (!thread) {
      REXLOG_WARN("[lz4b] the read-ahead thread could not be created: no read-ahead");
      return nullptr;
    }
    thread->set_name("lz4b read-ahead");
    thread.release();
    REXLOG_INFO("[lz4b] big files read in order are read ahead by a thread of their own (lz4b_read_ahead_mb {})",
                REXCVAR_GET(lz4b_read_ahead_mb));
    return worker;
  }

  void Loop() {
    zxc_dctx* zxc = nullptr;
    std::vector<uint8_t> stored;
    for (;;) {
      std::shared_ptr<ReadAhead> ra;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return dirty_; });
        dirty_ = false;
        ra = active_;
      }
      while (ra) {
        uint32_t first = 0, count = 0;
        {
          std::lock_guard<std::mutex> lock(ra->mutex);
          if (ra->closed || ra->failed) break;
          uint32_t b = ra->from;
          while (b < ra->to && ra->blocks.count(b)) ++b;
          if (b >= ra->to) break;  // the window is full
          first = b;
          while (b < ra->to && count < kReadAheadRunBlocks && !ra->blocks.count(b)) {
            ++b;
            ++count;
          }
        }
        // the run's stored bytes in one host read, then each block decoded (outside the lock: the game reads meanwhile)
        const size_t stored_size = size_t(ra->offsets[first + count] - ra->offsets[first]);
        stored.resize(stored_size);
        size_t got = 0;
        bool ok = ra->packed->Read(size_t(ra->offsets[first]), stored.data(), stored_size, &got) && got == stored_size;
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> made;
        const uint8_t* p = stored.data();
        for (uint32_t i = first; ok && i < first + count; ++i) {
          const uint32_t length = ra->BlockLength(i);
          const size_t s = size_t(ra->offsets[i + 1] - ra->offsets[i]);
          std::vector<uint8_t> data(ra->version == 2 ? zxc_decompress_block_bound(length) : size_t(length));
          ok = DecodeBlockData(ra->version, p, s, length, data.data(), data.size(), &zxc);
          data.resize(length);
          made.emplace_back(i, std::move(data));
          p += s;
        }
        {
          std::lock_guard<std::mutex> lock(ra->mutex);
          if (!ok) {
            ra->failed = true;
            REXLOG_WARN("[lz4b] {}: a read-ahead read failed: no more read-ahead for it",
                        rex::path_to_utf8(ra->packed->path()));
            break;
          }
          for (auto& [i, d] : made)
            if (i >= ra->from && i < ra->to) ra->blocks.emplace(i, std::move(d));
          ra->read_blocks += count;
          g_ahead_read.fetch_add(count, std::memory_order_relaxed);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_ != ra || dirty_) break;  // another file, or the window moved: from the top
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::shared_ptr<ReadAhead> active_;
  bool dirty_ = false;
};

class Lz4BlocksHandle final : public FileHandle {
 public:
  Lz4BlocksHandle(std::unique_ptr<FileHandle> packed, const Header& header, std::vector<uint64_t> offsets)
      : FileHandle(packed->path()),
        version_(header.version),
        packed_(std::move(packed)),
        size_(header.size),
        block_size_(header.block_size),
        block_count_(header.block_count),
        offsets_(std::move(offsets)) {
    static const int ahead_mb = std::clamp(int(REXCVAR_GET(lz4b_read_ahead_mb)), 0, 256);
    if (ahead_mb > 0 && size_ >= kReadAheadMinSize && !t_no_read_ahead && ReadAheadWorker::Get()) {
      ra_ = std::make_shared<ReadAhead>();
      ra_->packed = packed_;
      ra_->offsets = offsets_;
      ra_->version = version_;
      ra_->block_size = block_size_;
      ra_->block_count = block_count_;
      ra_->size = size_;
      ahead_blocks_ = uint32_t((uint64_t(ahead_mb) << 20) / block_size_);
    }
  }
  ~Lz4BlocksHandle() override {
    if (ra_) {
      std::lock_guard<std::mutex> lock(ra_->mutex);
      ra_->closed = true;
      ra_->blocks.clear();
    }
    if (zxc_) zxc_free_dctx(zxc_);
  }

  bool Read(size_t file_offset, void* buffer, size_t buffer_length, size_t* out_bytes_read) override {
    std::lock_guard<std::mutex> lock(mutex_);
    *out_bytes_read = 0;
    if (file_offset >= size_) return true;
    static const bool load_log = REXCVAR_GET(lz4b_load_log);
    if (load_log) {
      GetLoadLog().Begin(NowNs(), version_);
      r_packed_ = r_raw_ = 0;
      r_read_ns_ = r_decode_ns_ = 0;
      r_blocks_ = r_parts_ = 0;
    }
    const size_t want = size_t(std::min<uint64_t>(buffer_length, size_ - file_offset));
    auto* out = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < want) {
      const uint64_t at = file_offset + done;
      const uint32_t b = uint32_t(at / block_size_);
      const uint32_t inside = uint32_t(at - uint64_t(b) * block_size_);
      const uint32_t length = BlockLength(b);
      // zumasrevenge-nx (2026-11-25): a block the read-ahead thread decoded already
      if (ra_ && TakeAhead(b, inside, length, out + done, want - done, &done)) continue;
      if (inside == 0 && want - done >= length) {
        // A run of whole blocks: one host read, decoded straight into the caller's buffer (up to a read-ahead block).
        uint32_t end = b;
        uint64_t raw = 0;
        while (end < block_count_ && raw + BlockLength(end) <= want - done && raw < kRunBytes &&
               !(ra_ && end > b && HasAhead(end)))
          raw += BlockLength(end++);
        const size_t packed_length = size_t(offsets_[end] - offsets_[b]);
        packed_buffer_.resize(packed_length);
        if (!TimedRead(offsets_[b], packed_buffer_.data(), packed_length)) {
          LogBad(path_, "a read of its blocks failed");
          return false;
        }
        const uint8_t* p = packed_buffer_.data();
        for (uint32_t i = b; i < end; ++i) {
          // The caller's buffer from here on: ZXC decodes straight into it when it has the decoder's tail room.
          if (!Decode(i, p, out + done, want - done)) return false;
          p += offsets_[i + 1] - offsets_[i];
          done += BlockLength(i);
        }
      } else {
        // Part of a block: the whole block decoded into one of the kept blocks, for the reads that follow.
        // zumasrevenge-nx (2026-11-25): up to kKeptBlocks, the least recently used one replaced. With one kept block,
        // a read of Zuma's Revenge's main.rsb package that alternates between its stored bytes and its sounds' tables
        // decoded the same blocks again and again: 43.9 MB from the card for a read of 13.4 MB (1.4 s on the console,
        // the intro movie stopped meanwhile).
        Kept* kept = nullptr;
        for (Kept& k : kept_) {
          if (k.block == b) {
            kept = &k;
            break;
          }
        }
        if (!kept) {
          if (kept_.size() < kKeptBlocks) {
            kept = &kept_.emplace_back();
          } else {
            kept = &kept_[0];
            for (Kept& k : kept_)
              if (k.used < kept->used) kept = &k;
          }
          const size_t stored = size_t(offsets_[b + 1] - offsets_[b]);
          packed_buffer_.resize(stored);
          kept->data.resize(version_ == 2 ? zxc_decompress_block_bound(block_size_) : size_t(block_size_));
          kept->block = UINT32_MAX;
          if (!TimedRead(offsets_[b], packed_buffer_.data(), stored)) {
            LogBad(path_, "a read of a block failed");
            return false;
          }
          if (!Decode(b, packed_buffer_.data(), kept->data.data(), kept->data.size())) return false;
          ++r_parts_;
          kept->block = b;
        }
        kept->used = ++use_clock_;
        const size_t take = std::min<size_t>(want - done, length - inside);
        std::memcpy(out + done, kept->data.data() + inside, take);
        done += take;
      }
    }
    *out_bytes_read = done;
    if (ra_ && want) MoveWindow(uint32_t(file_offset / block_size_), uint32_t((file_offset + want - 1) / block_size_));
    if (load_log) GetLoadLog().End(NowNs(), r_packed_, r_read_ns_, r_raw_, r_decode_ns_, r_blocks_, r_parts_);
    return true;
  }

  bool Write(size_t, const void*, size_t, size_t* out_bytes_written) override {
    *out_bytes_written = 0;
    return false;
  }
  bool SetLength(size_t) override { return false; }
  void Flush() override {}

 private:
  uint32_t BlockLength(uint32_t b) const {
    return b + 1 < block_count_ ? block_size_ : uint32_t(size_ - uint64_t(b) * block_size_);
  }

  // zumasrevenge-nx (2026-11-25): the read-ahead's block b, its bytes from `inside` copied (as many as the read wants).
  bool TakeAhead(uint32_t b, uint32_t inside, uint32_t length, uint8_t* to, size_t room_left, size_t* done) {
    std::lock_guard<std::mutex> lock(ra_->mutex);
    auto it = ra_->blocks.find(b);
    if (it == ra_->blocks.end()) return false;
    const size_t take = std::min<size_t>(room_left, length - inside);
    std::memcpy(to, it->second.data() + inside, take);
    *done += take;
    ++ra_->hits;
    g_ahead_hits.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  bool HasAhead(uint32_t b) {
    std::lock_guard<std::mutex> lock(ra_->mutex);
    return ra_->blocks.count(b) != 0;
  }
  // After a read of blocks [first, last]: a read that continues the one before (it starts at most kSequentialBlocks
  // past its end) or lands in the window moves the window to the next ahead_blocks_ blocks and wakes the thread; any
  // other (a jump) ends the read-ahead until the reads go in order again.
  void MoveWindow(uint32_t first, uint32_t last) {
    bool wake = false;
    {
      std::lock_guard<std::mutex> lock(ra_->mutex);
      const bool continues = last_end_ != UINT32_MAX && first >= last_end_ && first <= last_end_ + kSequentialBlocks;
      const bool inside = first >= ra_->from && first < ra_->to;
      if (continues || inside) {
        const uint32_t from = last;  // the last block: the next read may start inside it
        const uint32_t to = uint32_t(std::min<uint64_t>(block_count_, uint64_t(last) + 1 + ahead_blocks_));
        if (from != ra_->from || to != ra_->to) {
          ra_->from = from;
          ra_->to = to;
          for (auto it = ra_->blocks.begin(); it != ra_->blocks.end();)
            it = (it->first < from || it->first >= to) ? ra_->blocks.erase(it) : std::next(it);
          wake = true;
        }
      } else if (ra_->from != ra_->to) {
        ra_->from = ra_->to = 0;
        ra_->blocks.clear();
      }
    }
    last_end_ = last;
    if (wake) ReadAheadWorker::Get()->Wake(ra_);
  }

  bool TimedRead(uint64_t offset, void* to, size_t length) {
    const int64_t t0 = NowNs();
    const bool ok = ReadExactly(*packed_, offset, to, length);
    r_read_ns_ += NowNs() - t0;
    r_packed_ += length;
    return ok;
  }

  // `room`: the bytes `to` may be written, at least the block's length. ZXC's decoder writes past a block's end on its
  // fast path (zxc_decompress_block_bound: 2,084 bytes past it); with less room it goes through its own buffer and a
  // copy (the last block of a run decoded into the caller's buffer). LZ4's writes nothing past it.
  bool Decode(uint32_t b, const uint8_t* stored, uint8_t* to, size_t room) {
    const size_t stored_size = size_t(offsets_[b + 1] - offsets_[b]);
    const uint32_t length = BlockLength(b);
    const int64_t t0 = NowNs();
    const bool ok = DecodeBlockData(version_, stored, stored_size, length, to, room, &zxc_);
    r_decode_ns_ += NowNs() - t0;
    r_raw_ += length;
    ++r_blocks_;
    if (ok) return true;
    LogBad(path_, "a block does not decompress");
    return false;
  }

  const uint32_t version_;
  std::shared_ptr<FileHandle> packed_;  // shared with the read-ahead thread (zumasrevenge-nx, 2026-11-25)
  std::shared_ptr<ReadAhead> ra_;       // null: no read-ahead for this file
  uint32_t ahead_blocks_ = 0;
  uint32_t last_end_ = UINT32_MAX;      // the last block of the read before
  const uint64_t size_;
  const uint32_t block_size_;
  const uint32_t block_count_;
  const std::vector<uint64_t> offsets_;  // block i at offsets_[i] .. offsets_[i + 1] of the packed file
  std::mutex mutex_;
  std::vector<uint8_t> packed_buffer_;
  // The decoded blocks kept for partial reads (zumasrevenge-nx, 2026-11-25: was one), made as they are needed.
  static constexpr size_t kKeptBlocks = 8;
  struct Kept {
    uint32_t block = UINT32_MAX;
    uint64_t used = 0;
    std::vector<uint8_t> data;
  };
  std::vector<Kept> kept_;
  uint64_t use_clock_ = 0;
  zxc_dctx* zxc_ = nullptr;
  // This read's timing (lz4b_load_log).
  uint64_t r_packed_ = 0, r_raw_ = 0;
  int64_t r_read_ns_ = 0, r_decode_ns_ = 0;
  uint32_t r_blocks_ = 0, r_parts_ = 0;
};

}  // namespace

bool HasLz4BlocksSuffix(std::string_view name) {
  if (name.size() <= kLz4BlocksSuffix.size()) return false;
  const std::string_view tail = name.substr(name.size() - kLz4BlocksSuffix.size());
  for (size_t i = 0; i < tail.size(); ++i) {
    char c = tail[i];
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    if (c != kLz4BlocksSuffix[i]) return false;
  }
  return true;
}

bool ReadLz4BlocksSize(const std::filesystem::path& path, uint64_t* out_size) {
  auto f = FileHandle::OpenExisting(path, FileAccess::kFileReadData);
  if (!f) return false;
  uint8_t h[kHeaderSize];
  Header header;
  if (!ReadExactly(*f, 0, h, sizeof(h)) || !ParseHeader(h, &header)) return false;
  *out_size = header.size;
  return true;
}

std::unique_ptr<FileHandle> OpenLz4Blocks(std::unique_ptr<FileHandle> packed) {
  if (!packed) return nullptr;
  FileInfo info;
  if (!GetInfo(packed->path(), &info)) {
    LogBad(packed->path(), "too short to be an .lz4b file");
    return nullptr;
  }
  return OpenLz4Blocks(std::move(packed), info.total_size);
}

// sa2-nx-ult (2026-11-24): the size given (an asset pack's entry has no host file of its own).
std::unique_ptr<FileHandle> OpenLz4Blocks(std::unique_ptr<FileHandle> packed, uint64_t packed_size) {
  if (!packed) return nullptr;
  const std::filesystem::path path = packed->path();
  struct {
    uint64_t total_size;
  } info{packed_size};
  if (info.total_size < kHeaderSize + kFooterSize) {
    LogBad(path, "too short to be an .lz4b file");
    return nullptr;
  }
  uint8_t h[kHeaderSize], f[kFooterSize];
  Header header;
  if (!ReadExactly(*packed, 0, h, sizeof(h)) || !ParseHeader(h, &header)) {
    LogBad(path, "not an .lz4b file (header)");
    return nullptr;
  }
  if (!ReadExactly(*packed, info.total_size - kFooterSize, f, sizeof(f)) || std::memcmp(f + 8, kEndMagic, 8) != 0) {
    LogBad(path, "not an .lz4b file (footer): cut short?");
    return nullptr;
  }
  const uint64_t index_offset = Le64(f);
  if (index_offset + uint64_t(header.block_count) * 4 + kFooterSize != info.total_size || index_offset < kHeaderSize) {
    LogBad(path, "its index is not where the footer says");
    return nullptr;
  }
  std::vector<uint8_t> index(size_t(header.block_count) * 4);
  if (!index.empty() && !ReadExactly(*packed, index_offset, index.data(), index.size())) {
    LogBad(path, "its index could not be read");
    return nullptr;
  }
  std::vector<uint64_t> offsets(size_t(header.block_count) + 1);
  offsets[0] = kHeaderSize;
  for (uint32_t i = 0; i < header.block_count; ++i) {
    const uint32_t stored = Le32(index.data() + size_t(i) * 4);
    const uint64_t length =
        i + 1 < header.block_count ? header.block_size : header.size - uint64_t(i) * header.block_size;
    if (stored == 0 || stored > length) {
      LogBad(path, "a block's stored size is not what the format allows");
      return nullptr;
    }
    offsets[i + 1] = offsets[i] + stored;
  }
  if (offsets.back() != index_offset) {
    LogBad(path, "its blocks do not end where the index starts");
    return nullptr;
  }
  static std::atomic<uint32_t> opened{0};
  if (opened.fetch_add(1, std::memory_order_relaxed) < 16)
    REXLOG_INFO("[lz4b] {}: {} bytes in {} blocks of {} KB ({}), {} packed ({:.1f} %)", rex::path_to_utf8(path),
                header.size, header.block_count, header.block_size >> 10,
                header.version == 2 ? "ZXC" : "LZ4",
                info.total_size,
                header.size ? double(info.total_size) * 100.0 / double(header.size) : 0.0);
  return std::make_unique<Lz4BlocksHandle>(std::move(packed), header, std::move(offsets));
}

void SetLz4BlocksNoReadAheadOnThisThread(bool no_read_ahead) { t_no_read_ahead = no_read_ahead; }

}  // namespace rex::filesystem
