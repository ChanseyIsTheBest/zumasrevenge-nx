/**
 * culdcept-nx (2026-11-20, credit culdcept-nx): NAME.xmo, an XMA file whose frames are Opus (rex/filesystem/xmo.h has
 * the format). The handle below synthesizes NAME's bytes on each read, a packet at a time: its RIFF header, packet
 * headers and trailer as they were, each frame's 15-bit length field and last bit, and the packet's tag in its tag bits
 * (everything else zero), so the game and the XMA emulation walk the same packets and frames as before. Deterministic
 * (the read caches and parked handles keep what it gives).
 *
 * sasrt-nx (2026-11-21): NAME.xmop, a Wwise package whose XMA2 sounds are Opus: its segments (stored bytes, zeros, a
 * sound's packets) make NAME; a sound's packets are synthesized as NAME.xmo's are, from its own packet headers and frame
 * table, read when a read first reaches the sound and kept for the most recently used sounds.
 */
#include <rex/filesystem/xmo.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <atomic>
#include <mutex>
#include <set>

#include <rex/filesystem.h>
#include <rex/filesystem/lz4_blocks.h>  // NAME.xmop.lz4b (sasrt-nx, 2026-11-21)
#include <rex/logging.h>
#include <rex/string.h>
#include <rex/thread.h>  // the Opus loader thread (zumasrevenge-nx, 2026-11-25)

namespace rex::filesystem {
namespace {

constexpr char kMagic[8] = {'R', 'X', 'X', 'M', 'A', 'O', 'P', '1'};
constexpr char kPackMagic[8] = {'R', 'X', 'X', 'M', 'A', 'P', 'K', '1'};
constexpr size_t kHeaderSize = 64;
constexpr size_t kSegmentSize = 32;
constexpr size_t kSoundEntrySize = 64;
constexpr uint32_t kPacketBits = kXmoPacketBytes * 8;
constexpr uint32_t kPacketHeaderBits = 32;
constexpr uint32_t kFrameHeaderBits = 15;
constexpr uint32_t kMaxFrameLength = 0x7FFF;
// The Opus data of this many bytes stays read at most (sounds not in use are read again when they play): a piece of
// music is 2-5 MB at 128 kbit/s, an effect a few KB.
constexpr size_t kLoadedLimit = 16u << 20;
// sasrt-nx: a 3-minute piece of music at 240 kbit/s XMA has ~17,000 frames, its tables 136 KB (the limit of the kept
// tables is kLayoutStoreLimit since zumasrevenge-nx's store, 2026-11-25).

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint64_t Le64(const uint8_t* p) { return uint64_t(Le32(p)) | (uint64_t(Le32(p + 4)) << 32); }

bool ReadExactly(FileHandle& f, uint64_t offset, void* to, size_t length) {
  size_t got = 0;
  return f.Read(size_t(offset), to, length, &got) && got == length;
}

bool HasSuffix(std::string_view name, std::string_view suffix) {
  if (name.size() <= suffix.size()) return false;
  const std::string_view tail = name.substr(name.size() - suffix.size());
  for (size_t i = 0; i < tail.size(); ++i) {
    char c = tail[i];
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    if (c != suffix[i]) return false;
  }
  return true;
}

// sasrt-nx (2026-11-21, as rr6-nx's sfd_hevc.cpp does for NAME.hevc.lz4b): NAME.xmop.lz4b holds the .xmop package,
// read through OpenLz4Blocks; any other handle as it is.
bool IsPacked(const std::filesystem::path& path) { return HasLz4BlocksSuffix(rex::path_to_utf8(path.filename())); }
std::unique_ptr<FileHandle> Unpacked(std::unique_ptr<FileHandle> f) {
  if (!f || !IsPacked(f->path())) return f;
  return OpenLz4Blocks(std::move(f));  // logs why when it is not a valid .lz4b file
}
bool UnpackedSize(const std::filesystem::path& path, uint64_t* out_size) {
  if (IsPacked(path)) return ReadLz4BlocksSize(path, out_size);
  FileInfo info;
  if (!GetInfo(path, &info)) return false;
  *out_size = info.total_size;
  return true;
}

struct Header {
  uint32_t id = 0;
  uint64_t size = 0;
  uint32_t data_offset = 0, packets = 0, frames = 0, rate = 0;
  uint16_t channels = 0, pre_skip = 0;
  uint32_t opus_count = 0;
  uint64_t opus_table = 0;
  uint32_t trailer = 0;
  uint64_t packets_end() const { return uint64_t(data_offset) + uint64_t(packets) * kXmoPacketBytes; }
  uint64_t frame_table() const { return kHeaderSize + data_offset + uint64_t(packets) * 4; }
};

bool ParseHeader(const uint8_t* h, Header* out) {
  if (std::memcmp(h, kMagic, 8) != 0 || Le32(h + 8) != 1) return false;
  out->id = Le32(h + 12);
  out->size = Le64(h + 16);
  out->data_offset = Le32(h + 24);
  out->packets = Le32(h + 28);
  out->frames = Le32(h + 32);
  out->rate = Le32(h + 36);
  out->channels = Le16(h + 40);
  out->pre_skip = Le16(h + 42);
  out->opus_count = Le32(h + 44);
  out->opus_table = Le64(h + 48);
  out->trailer = Le32(h + 56);
  return out->data_offset <= 4096 && out->channels >= 1 && out->channels <= 2 && out->rate == 48000 &&
         out->trailer <= (1u << 20) && out->packets_end() + out->trailer == out->size &&
         out->packets < (1u << 24);  // the tag's packet index is 24 bits
}

bool ReadHeader(FileHandle& f, Header* out) {
  uint8_t h[kHeaderSize];
  return ReadExactly(f, 0, h, sizeof(h)) && ParseHeader(h, out);
}

// sasrt-nx: NAME.xmop's header and a sound of its table.
struct PackHeader {
  uint32_t segments = 0;
  uint64_t size = 0, segments_offset = 0, stored_offset = 0, stored_length = 0;
  uint32_t sounds = 0;
  uint64_t sounds_offset = 0;
};

bool ParsePackHeader(const uint8_t* h, PackHeader* out) {
  if (std::memcmp(h, kPackMagic, 8) != 0 || Le32(h + 8) != 1) return false;
  out->segments = Le32(h + 12);
  out->size = Le64(h + 16);
  out->segments_offset = Le64(h + 24);
  out->stored_offset = Le64(h + 32);
  out->stored_length = Le64(h + 40);
  out->sounds = Le32(h + 48);
  out->sounds_offset = Le64(h + 56);
  return out->segments >= 1 && out->segments <= (1u << 26) && out->sounds <= (1u << 20);
}

struct PackSound {
  uint32_t id = 0, packets = 0, frames = 0, rate = 0;
  uint16_t channels = 0, pre_skip = 0;
  uint32_t opus_count = 0, opus_rate = 0;
  uint64_t headers = 0, frame_table = 0, opus_table = 0;
};

bool ParsePackSound(const uint8_t* e, PackSound* out) {
  out->id = Le32(e);
  out->packets = Le32(e + 4);
  out->frames = Le32(e + 8);
  out->rate = Le32(e + 12);
  out->channels = Le16(e + 16);
  out->pre_skip = Le16(e + 18);
  out->opus_count = Le32(e + 20);
  out->opus_rate = Le32(e + 24);
  out->headers = Le64(e + 32);
  out->frame_table = Le64(e + 40);
  out->opus_table = Le64(e + 48);
  const bool rates = (out->rate == 48000 && out->opus_rate == 48000) || (out->rate == 24000 && out->opus_rate == 24000) ||
                     (out->rate == 44100 && out->opus_rate == 48000);
  return rates && out->channels >= 1 && out->channels <= 2 && out->packets >= 1 && out->packets < (1u << 24) &&
         out->frames >= 1;
}

struct Frame {
  uint32_t pos;  // bit position in the data
  uint16_t len;  // bits
  uint8_t last;  // the frame's last bit
};

inline uint32_t Bit(const uint8_t* p, uint32_t b) { return (p[b >> 3] >> (7 - (b & 7))) & 1; }
// Bit k of a tag (72 bits, more than a uint64_t: magic, sound id, packet index).
inline uint32_t TagBit(uint32_t k, uint32_t id, uint32_t packet_index) {
  return k < 16 ? (kXmoTagMagic >> (15 - k)) & 1 : k < 48 ? (id >> (47 - k)) & 1 : (packet_index >> (71 - k)) & 1;
}
inline void SetBit(uint8_t* p, uint32_t b) { p[b >> 3] |= uint8_t(0x80 >> (b & 7)); }

// Packet p of a sound's data, synthesized: its header, the length fields and last bits of the frames in it (a frame is
// at most 32767 bits: one that starts two packets before can reach into it), then its tag. NAME.xmo's and NAME.xmop's.
class PacketMaker {
 public:
  PacketMaker(uint32_t id, uint32_t packets, const uint8_t* packet_headers, const Frame* frames, size_t frame_count)
      : id_(id), packets_(packets), headers_(packet_headers), frames_(frames), frame_count_(frame_count) {}

  void Synthesize(uint32_t p, uint8_t* packet) const {
    std::memset(packet, 0, kXmoPacketBytes);
    std::memcpy(packet, headers_ + size_t(p) * 4, 4);
    const uint64_t from = p >= 2 ? uint64_t(p - 2) * kPacketBits : 0, to = uint64_t(p + 1) * kPacketBits;
    const Frame* end = frames_ + frame_count_;
    const Frame* it = std::lower_bound(frames_, end, from, [](const Frame& f, uint64_t v) { return f.pos < v; });
    bool starts = false;  // a frame starts in packet p
    for (; it != end && it->pos < to; ++it) {
      Put(packet, p, *it, 0, kFrameHeaderBits, it->len);
      Put(packet, p, *it, uint32_t(it->len) - 1, 1, it->last);
      starts = starts || it->pos >= uint64_t(p) * kPacketBits;
    }
    // sasrt-nx (2026-11-21, the installer's check): only a packet a frame starts in carries a tag (xmo.h's rule). A
    // sound's last packet can have no frame starting in it and a skip count other than 0xFF: XmoTagBits would then give
    // the end of the frame that runs into it, and the tag would set that frame's last bit ("another frame follows").
    if (!starts) return;
    uint16_t bits[kXmoTagBits];
    if (XmoTagBits(packet, bits, kXmoTagBits) < kXmoTagBits) return;  // no frame starts here (the installer checked)
    for (uint32_t k = 0; k < kXmoTagBits; ++k) {
      if (TagBit(k, id_, p)) SetBit(packet, bits[k]);
    }
  }

 private:
  // Bit `j` of frame `f` as packet and bit: a frame goes on past its packet's end in the packet its header's skip count
  // names (the next one, for a single stream), after that packet's 32-bit header.
  void FrameBit(const Frame& f, uint32_t j, uint64_t* packet, uint32_t* in) const {
    uint64_t pk = f.pos / kPacketBits, b = f.pos % kPacketBits + j;
    while (b >= kPacketBits) {
      b -= kPacketBits;
      const uint8_t skip = pk < packets_ ? headers_[size_t(pk * 4 + 3)] : 0;
      pk += uint64_t(skip == 0xFF ? 0 : skip) + 1;
      b += kPacketHeaderBits;
    }
    *packet = pk;
    *in = uint32_t(b);
  }
  // `count` bits of `value` (MSB first) at frame bits [j, j + count), those that fall in packet p.
  void Put(uint8_t* packet, uint32_t p, const Frame& f, uint32_t j, uint32_t count, uint32_t value) const {
    for (uint32_t k = 0; k < count; ++k) {
      if (!((value >> (count - 1 - k)) & 1)) continue;
      uint64_t pk;
      uint32_t b;
      FrameBit(f, j + k, &pk, &b);
      if (pk == p) SetBit(packet, b);
    }
  }

  uint32_t id_, packets_;
  const uint8_t* headers_;
  const Frame* frames_;
  size_t frame_count_;
};

bool ParseFrames(const uint8_t* table, uint32_t count, uint32_t packets, std::vector<Frame>* out) {
  out->resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = table + size_t(i) * 8;
    (*out)[i] = {Le32(e), Le16(e + 4), e[6]};
    if ((*out)[i].len <= kFrameHeaderBits || (*out)[i].pos / kPacketBits >= packets ||
        (i && (*out)[i].pos <= (*out)[i - 1].pos)) {
      return false;
    }
  }
  return true;
}

// --- the registry: sound id -> its .xmo (or its .xmop and place in it), the Opus data read when the game reads the
// sound's data ---
struct Registered {
  std::filesystem::path path;
  bool pack = false;
  Header header;          // .xmo
  PackSound pack_sound;   // .xmop
  std::shared_ptr<XmoSound> sound;  // null until read (and again after it is let go)
};
std::mutex g_mutex;
std::map<uint32_t, Registered> g_sounds;
std::list<uint32_t> g_loaded;  // read sounds, the most recently used first
size_t g_loaded_bytes = 0;
std::atomic<bool> g_any_sound{false};  // base port 259: AnyXmoSound

void Register(const std::filesystem::path& path, const Header& h) {
  std::lock_guard lock(g_mutex);
  g_any_sound.store(true, std::memory_order_relaxed);
  auto [it, inserted] = g_sounds.try_emplace(h.id);
  if (!inserted && it->second.path != path) {
    REXFS_WARN("[xmo] sound id {:08X} is in both {} and {}: the second is used", h.id,
               rex::path_to_utf8(it->second.path), rex::path_to_utf8(path));
    it->second.sound.reset();
  }
  it->second.path = path;
  it->second.pack = false;
  it->second.header = h;
}

void RegisterPack(const std::filesystem::path& path, const std::vector<PackSound>& sounds) {
  std::lock_guard lock(g_mutex);
  if (!sounds.empty()) g_any_sound.store(true, std::memory_order_relaxed);
  for (const PackSound& s : sounds) {
    auto [it, inserted] = g_sounds.try_emplace(s.id);
    if (!inserted && it->second.path != path) {
      REXFS_WARN("[xmo] sound id {:08X} is in both {} and {}: the second is used", s.id,
                 rex::path_to_utf8(it->second.path), rex::path_to_utf8(path));
      it->second.sound.reset();
    }
    it->second.path = path;
    it->second.pack = true;
    it->second.pack_sound = s;
  }
}

std::shared_ptr<XmoSound> LoadOpus(FileHandle& f, uint32_t id, uint32_t rate, uint32_t opus_rate, uint16_t channels,
                                   uint16_t pre_skip, uint64_t frame_table, uint32_t frames, uint64_t opus_table,
                                   uint32_t opus_count) {
  std::vector<uint8_t> table(size_t(frames) * 8), sizes(size_t(opus_count) * 2);
  if (!ReadExactly(f, frame_table, table.data(), table.size()) ||
      !ReadExactly(f, opus_table, sizes.data(), sizes.size())) {
    return nullptr;
  }
  auto s = std::make_shared<XmoSound>();
  s->id = id;
  s->sample_rate = rate;
  s->opus_rate = opus_rate;
  s->channels = channels;
  s->pre_skip = pre_skip;
  s->frame_pos.resize(frames);
  for (uint32_t i = 0; i < frames; ++i) s->frame_pos[i] = Le32(table.data() + size_t(i) * 8);
  s->opus_offset.resize(size_t(opus_count) + 1);
  uint32_t total = 0;
  for (uint32_t i = 0; i < opus_count; ++i) {
    s->opus_offset[i] = total;
    total += Le16(sizes.data() + size_t(i) * 2);
  }
  s->opus_offset[opus_count] = total;
  s->opus.resize(total);
  if (total && !ReadExactly(f, opus_table + sizes.size(), s->opus.data(), total)) return nullptr;
  return s;
}

std::atomic<XmoSourceOpener> g_opener{nullptr};  // the PopCap ports' asset pack (2026-11-24)

// zumasrevenge-nx (2026-11-25): the files the Opus is read from, each opened once and kept (a load opened its file
// anew and read its block table again: 93 times within one read of Zuma's Revenge's main.rsb package). A kept file's
// reads go under its own mutex. Kept for at most kKeptSources files (a game with a .xmo for every sound opens them as
// before, one at a time).
constexpr size_t kKeptSources = 32;
struct OpenSource {
  std::mutex mutex;
  std::unique_ptr<FileHandle> f;
};
std::mutex g_sources_mutex;
std::map<std::string, std::shared_ptr<OpenSource>> g_sources;

std::unique_ptr<FileHandle> OpenSourceFile(const std::filesystem::path& path) {
  // zumasrevenge-nx (2026-11-25): no read-ahead for these handles (the sounds' Opus and tables are not read in order)
  SetLz4BlocksNoReadAheadOnThisThread(true);
  std::unique_ptr<FileHandle> raw;
  if (const XmoSourceOpener opener = g_opener.load(std::memory_order_acquire)) raw = opener(path);
  if (!raw) raw = FileHandle::OpenExisting(path, FileAccess::kFileReadData);
  auto f = Unpacked(std::move(raw));
  SetLz4BlocksNoReadAheadOnThisThread(false);
  return f;
}

std::shared_ptr<XmoSound> LoadFrom(FileHandle& f, const Registered& r);

std::shared_ptr<XmoSound> Load(const Registered& r) {
  std::shared_ptr<OpenSource> source;
  {
    std::lock_guard lock(g_sources_mutex);
    const std::string key = rex::path_to_utf8(r.path);
    auto it = g_sources.find(key);
    if (it != g_sources.end())
      source = it->second;
    else if (g_sources.size() < kKeptSources)
      source = g_sources[key] = std::make_shared<OpenSource>();
  }
  if (!source) {
    auto f = OpenSourceFile(r.path);
    return f ? LoadFrom(*f, r) : nullptr;
  }
  std::lock_guard lock(source->mutex);
  if (!source->f) source->f = OpenSourceFile(r.path);
  return source->f ? LoadFrom(*source->f, r) : nullptr;
}

std::shared_ptr<XmoSound> LoadFrom(FileHandle& f_ref, const Registered& r) {
  FileHandle* const f = &f_ref;
  if (r.pack) {
    const PackSound& p = r.pack_sound;
    return LoadOpus(*f, p.id, p.rate, p.opus_rate, p.channels, p.pre_skip, p.frame_table, p.frames, p.opus_table,
                    p.opus_count);
  }
  const Header& h = r.header;
  return LoadOpus(*f, h.id, h.rate, h.rate, h.channels, h.pre_skip, h.frame_table(), h.frames, h.opus_table,
                  h.opus_count);
}

// zumasrevenge-nx (2026-11-25): a sound's Opus, read in the background when the game reads the sound's data. The read
// took it on the game's thread before: Zuma's Revenge's main thread read 13.4 MB of its main.rsb package during the
// intro movie, the Opus of 93 sounds came with it (520 ms of 1.4 s; the movie stopped meanwhile). The game reads its
// sounds' data long before it plays them, so the Opus is held by then; a sound played earlier is read by its first
// frame's decode, as a sound pushed out of the 16 MB held is. One thread, never ended (its object is not destroyed at
// exit); without it (its creation failed) the read takes the Opus as before.
// The loader thread runs jobs in order: the sounds' Opus (PrefetchXmoSound) and a package's tables (PrefetchLayouts).
std::mutex g_loader_mutex;
std::condition_variable g_loader_cv;
std::deque<std::function<void()>> g_loader_jobs;
std::set<uint32_t> g_prefetch_queued;  // sound ids whose Opus job is queued
bool g_loader_started = false, g_loader_failed = false;
int g_loader_paused = 0;  // PauseXmoLoader: the movies playing

void LoaderLoop() {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock lock(g_loader_mutex);
      g_loader_cv.wait(lock, [] { return !g_loader_jobs.empty() && g_loader_paused == 0; });
      job = std::move(g_loader_jobs.front());
      g_loader_jobs.pop_front();
    }
    job();
  }
}

}  // namespace

void PauseXmoLoader(bool paused) {
  size_t waiting;
  {
    std::lock_guard lock(g_loader_mutex);
    g_loader_paused = std::max(0, g_loader_paused + (paused ? 1 : -1));
    waiting = g_loader_jobs.size();
  }
  REXFS_INFO("[xmo] the loader thread {} ({} jobs waiting)", paused ? "holds its jobs while a movie plays" : "goes on",
             waiting);
  if (!paused) g_loader_cv.notify_all();
}

namespace {

// False when there is no loader thread (its creation failed): the caller does the job itself, or does without it.
bool QueueLoaderJob(std::function<void()> job) {
  {
    std::lock_guard lock(g_loader_mutex);
    if (!g_loader_started) {
      g_loader_started = true;
      rex::thread::Thread::CreationParameters params;
      params.stack_size = 256 * 1024;
      if (auto thread = rex::thread::Thread::Create(params, LoaderLoop)) {
        thread->set_name("xmo loader");
        thread.release();  // never destroyed: it lives as long as the process
        REXFS_INFO("[xmo] the sounds' Opus and the packages' tables are read by a thread of their own");
      } else {
        g_loader_failed = true;
        REXFS_WARN("[xmo] the loader thread could not be created: the Opus is read with the game's reads");
      }
    }
    if (g_loader_failed) return false;
    g_loader_jobs.push_back(std::move(job));
  }
  g_loader_cv.notify_one();
  return true;
}

void PrefetchXmoSound(uint32_t id) {
  {
    std::lock_guard lock(g_mutex);
    auto it = g_sounds.find(id);
    if (it == g_sounds.end() || it->second.sound) return;  // unknown, or held already
  }
  {
    std::lock_guard lock(g_loader_mutex);
    if (!g_prefetch_queued.insert(id).second) return;  // queued already
  }
  const auto forget = [id] {
    std::lock_guard lock(g_loader_mutex);
    g_prefetch_queued.erase(id);
  };
  if (!QueueLoaderJob([id, forget] {
        forget();
        FindXmoSound(id);
      })) {
    forget();
    FindXmoSound(id);
  }
}

// zumasrevenge-nx (2026-11-25): a package's sounds' packet headers and frame tables (what their packets are made
// from), kept for every handle of the package and read ahead by the loader thread when the package is opened. Each
// handle read them as the game reached the sounds, a few blocks of the package apart from the bytes around them: 94
// sounds took 400 ms of a read of Zuma's Revenge's main.rsb on the console (72 ms with the kept blocks), its intro
// movie stopped meanwhile. Kept up to kLayoutStoreLimit a package; past it a sound's tables are read when needed.
struct Layout {
  std::vector<uint8_t> headers;
  std::vector<Frame> frames;
  size_t bytes() const { return headers.size() + frames.size() * sizeof(Frame); }
};
constexpr size_t kLayoutStoreLimit = 32u << 20;

struct LayoutStore {
  std::mutex mutex;
  std::map<uint32_t, std::shared_ptr<const Layout>> layouts;  // by the sound's index in its package
  size_t bytes = 0;
  bool prefetch_queued = false;

  std::shared_ptr<const Layout> Find(uint32_t s) {
    std::lock_guard lock(mutex);
    auto it = layouts.find(s);
    return it != layouts.end() ? it->second : nullptr;
  }
  // False when the store is full.
  bool Keep(uint32_t s, std::shared_ptr<const Layout> l) {
    std::lock_guard lock(mutex);
    if (layouts.count(s)) return true;
    if (bytes + l->bytes() > kLayoutStoreLimit) return false;
    bytes += l->bytes();
    layouts.emplace(s, std::move(l));
    return true;
  }
};
std::mutex g_layout_stores_mutex;
std::map<std::string, std::shared_ptr<LayoutStore>> g_layout_stores;

std::shared_ptr<LayoutStore> LayoutStoreFor(const std::filesystem::path& path) {
  std::lock_guard lock(g_layout_stores_mutex);
  auto& s = g_layout_stores[rex::path_to_utf8(path)];
  if (!s) s = std::make_shared<LayoutStore>();
  return s;
}

std::shared_ptr<const Layout> ReadLayout(FileHandle& f, const PackSound& ps) {
  auto l = std::make_shared<Layout>();
  l->headers.resize(size_t(ps.packets) * 4);
  std::vector<uint8_t> table(size_t(ps.frames) * 8);
  if (!ReadExactly(f, ps.headers, l->headers.data(), l->headers.size()) ||
      !ReadExactly(f, ps.frame_table, table.data(), table.size()) ||
      !ParseFrames(table.data(), ps.frames, ps.packets, &l->frames)) {
    return nullptr;
  }
  return l;
}

// Every sound's tables, in the package's order, on the loader thread with a handle of its own (the game's reads keep
// theirs). Once a package.
void PrefetchLayouts(const std::filesystem::path& path, std::vector<PackSound> sounds) {
  auto store = LayoutStoreFor(path);
  {
    std::lock_guard lock(store->mutex);
    if (store->prefetch_queued) return;
    store->prefetch_queued = true;
  }
  QueueLoaderJob([path, sounds = std::move(sounds), store] {
    const auto t0 = std::chrono::steady_clock::now();
    auto f = OpenSourceFile(path);
    if (!f) return;
    uint32_t read = 0;
    bool full = false;
    for (uint32_t s = 0; s < sounds.size(); ++s) {
      if (store->Find(s)) continue;
      auto l = ReadLayout(*f, sounds[s]);
      if (!l) break;
      if (!store->Keep(s, l)) {
        full = true;
        break;
      }
      ++read;
    }
    size_t kept;
    {
      std::lock_guard lock(store->mutex);
      kept = store->bytes;
    }
    REXFS_INFO("[xmo] {}: the tables of {} of its {} sounds read ahead in {:.0f} ms ({:.1f} MB kept{})",
               rex::path_to_utf8(path.filename()), read, sounds.size(),
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
               double(kept) / 1048576.0, full ? ", the store full" : "");
  });
}

// --- the synthesized XMA file ---
class XmoHandle : public FileHandle {
 public:
  XmoHandle(std::unique_ptr<FileHandle> packed, Header h, std::vector<uint8_t> riff, std::vector<uint8_t> packet_headers,
            std::vector<Frame> frames, std::vector<uint8_t> trailer)
      : FileHandle(packed->path()),
        packed_(std::move(packed)),
        h_(h),
        riff_(std::move(riff)),
        packet_headers_(std::move(packet_headers)),
        frames_(std::move(frames)),
        trailer_(std::move(trailer)),
        maker_(h_.id, h_.packets, packet_headers_.data(), frames_.data(), frames_.size()) {}

  bool Read(size_t file_offset, void* buffer, size_t buffer_length, size_t* out_bytes_read) override {
    *out_bytes_read = 0;
    if (file_offset >= h_.size) return true;
    const size_t n = size_t(std::min<uint64_t>(buffer_length, h_.size - file_offset));
    uint8_t* out = static_cast<uint8_t*>(buffer);
    const uint64_t lo = file_offset, hi = file_offset + n;
    // NAME's RIFF header
    if (lo < h_.data_offset) std::memcpy(out, riff_.data() + lo, size_t(std::min<uint64_t>(hi, h_.data_offset) - lo));
    // NAME's trailer (its seek chunk)
    const uint64_t t = h_.packets_end();
    if (hi > t) {
      const uint64_t from = std::max(lo, t);
      std::memcpy(out + (from - lo), trailer_.data() + (from - t), size_t(hi - from));
    }
    // the packets, each synthesized whole and the part in the range copied
    if (hi > h_.data_offset && lo < t) {
      // The game reads the data before it plays it: the Opus is read now, not by the first frame's decode on the audio
      // thread (zumasrevenge-nx, 2026-11-25: by the Opus loader thread, not the game's).
      PrefetchXmoSound(h_.id);
      const uint64_t d_lo = lo > h_.data_offset ? lo - h_.data_offset : 0, d_hi = std::min(hi, t) - h_.data_offset;
      uint8_t packet[kXmoPacketBytes];
      for (uint64_t p = d_lo / kXmoPacketBytes; p * kXmoPacketBytes < d_hi; ++p) {
        maker_.Synthesize(uint32_t(p), packet);
        const uint64_t a = std::max(d_lo, p * kXmoPacketBytes), b = std::min(d_hi, (p + 1) * kXmoPacketBytes);
        std::memcpy(out + (h_.data_offset + a - lo), packet + (a - p * kXmoPacketBytes), size_t(b - a));
      }
    }
    *out_bytes_read = n;
    return true;
  }
  bool Write(size_t, const void*, size_t, size_t*) override { return false; }
  bool SetLength(size_t) override { return false; }
  void Flush() override {}

 private:
  std::unique_ptr<FileHandle> packed_;
  Header h_;
  std::vector<uint8_t> riff_, packet_headers_;
  std::vector<Frame> frames_;
  std::vector<uint8_t> trailer_;
  PacketMaker maker_;
};

// --- sasrt-nx (2026-11-21): the synthesized Wwise package ---
struct PackSegment {
  uint64_t offset = 0, length = 0;
  uint32_t kind = 0, sound = 0;
  uint64_t source = 0;
};

class XmopHandle : public FileHandle {
 public:
  XmopHandle(std::unique_ptr<FileHandle> packed, PackHeader h, std::vector<PackSegment> segments,
             std::vector<PackSound> sounds)
      : FileHandle(packed->path()),
        packed_(std::move(packed)),
        h_(h),
        segments_(std::move(segments)),
        sounds_(std::move(sounds)) {}

  bool Read(size_t file_offset, void* buffer, size_t buffer_length, size_t* out_bytes_read) override {
    std::lock_guard<std::mutex> lock(mutex_);
    *out_bytes_read = 0;
    if (file_offset >= h_.size) return true;
    // zumasrevenge-nx (2026-11-25): Zuma's Revenge's intro froze for 1.4 s twice on the console, each time the game's
    // main thread read 6-13 MB of main.rsb (a package of sounds): where such a read's time goes, logged when it is slow.
    const auto t_start = std::chrono::steady_clock::now();
    stats_ = {};
    const bool ok = ReadParts(file_offset, buffer, buffer_length, out_bytes_read);
    const double total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_start).count();
    if (total_ms > 50.0)
      REXFS_WARN("[xmo] {}: a read of {} bytes at {} took {:.1f} ms: {} sounds (their layouts {:.1f} ms, {} read), "
                 "Opus {:.1f} ms ({} loaded), {} packets made in {:.1f} ms, stored bytes {:.1f} ms",
                 rex::path_to_utf8(path_.filename()), buffer_length, file_offset, total_ms, stats_.sounds,
                 stats_.layout_ms, stats_.layouts_read, stats_.opus_ms, stats_.opus_loaded, stats_.packets,
                 stats_.synth_ms, stats_.stored_ms);
    return ok;
  }

  bool ReadParts(size_t file_offset, void* buffer, size_t buffer_length, size_t* out_bytes_read) {
    const size_t want = size_t(std::min<uint64_t>(buffer_length, h_.size - file_offset));
    auto* out = static_cast<uint8_t*>(buffer);
    size_t si = size_t(std::upper_bound(segments_.begin(), segments_.end(), uint64_t(file_offset),
                                        [](uint64_t v, const PackSegment& s) { return v < s.offset; }) -
                       segments_.begin());
    if (si == 0) return false;
    --si;
    size_t done = 0;
    while (done < want && si < segments_.size()) {
      const PackSegment& seg = segments_[si];
      const uint64_t inside = file_offset + done - seg.offset;
      const size_t take = size_t(std::min<uint64_t>(want - done, seg.length - inside));
      if (seg.kind == 0) {
        const auto t0 = std::chrono::steady_clock::now();
        if (!ReadExactly(*packed_, h_.stored_offset + seg.source + inside, out + done, take)) {
          REXFS_ERROR("[xmo] {}: a read of its stored bytes failed", rex::path_to_utf8(path_));
          return false;
        }
        stats_.stored_ms += Ms(t0);
      } else if (seg.kind == 1) {
        std::memset(out + done, 0, take);
      } else if (!SoundBytes(seg.sound, seg.source + inside, out + done, take)) {
        return false;
      }
      done += take;
      ++si;
    }
    *out_bytes_read = done;
    return true;
  }
  bool Write(size_t, const void*, size_t, size_t*) override { return false; }
  bool SetLength(size_t) override { return false; }
  void Flush() override {}

 private:
  // Bytes [from, from + n) of sound s's packets, synthesized.
  bool SoundBytes(uint32_t s, uint64_t from, uint8_t* out, size_t n) {
    const PackSound& ps = sounds_[s];
    ++stats_.sounds;
    auto t0 = std::chrono::steady_clock::now();
    const size_t layouts_before = layouts_loaded_;
    const std::shared_ptr<const Layout> layout = GetLayout(s);
    stats_.layout_ms += Ms(t0);
    stats_.layouts_read += uint32_t(layouts_loaded_ - layouts_before);
    if (!layout) return false;
    t0 = std::chrono::steady_clock::now();
    PrefetchXmoSound(ps.id);  // the Opus read now, by the Opus loader thread (as NAME.xmo's)
    const double opus_ms = Ms(t0);
    stats_.opus_ms += opus_ms;
    if (opus_ms > 1.0) ++stats_.opus_loaded;
    t0 = std::chrono::steady_clock::now();
    const PacketMaker maker(ps.id, ps.packets, layout->headers.data(), layout->frames.data(), layout->frames.size());
    uint8_t packet[kXmoPacketBytes];
    const uint64_t to = from + n;
    for (uint64_t p = from / kXmoPacketBytes; p * kXmoPacketBytes < to; ++p) {
      if (p >= ps.packets) {
        std::memset(out + (std::max(from, p * kXmoPacketBytes) - from), 0, size_t(to - std::max(from, p * kXmoPacketBytes)));
        break;
      }
      maker.Synthesize(uint32_t(p), packet);
      ++stats_.packets;
      const uint64_t a = std::max(from, p * kXmoPacketBytes), b = std::min(to, (p + 1) * kXmoPacketBytes);
      std::memcpy(out + (a - from), packet + (a - p * kXmoPacketBytes), size_t(b - a));
    }
    stats_.synth_ms += Ms(t0);
    return true;
  }

  static double Ms(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }
  // The current read's costs (logged when it is slow).
  struct ReadStats {
    uint32_t sounds = 0, layouts_read = 0, opus_loaded = 0;
    uint64_t packets = 0;
    double layout_ms = 0, opus_ms = 0, synth_ms = 0, stored_ms = 0;
  };
  ReadStats stats_;
  size_t layouts_loaded_ = 0;

  // Sound s's packet headers and frame table: the package's store (the loader thread reads them all ahead when the
  // package is opened), else read now and kept there.
  std::shared_ptr<const Layout> GetLayout(uint32_t s) {
    if (auto l = store_->Find(s)) return l;
    auto l = ReadLayout(*packed_, sounds_[s]);
    if (!l) {
      REXFS_ERROR("[xmo] {}: sound {:08X}'s packet headers or frame table could not be read", rex::path_to_utf8(path_),
                  sounds_[s].id);
      return nullptr;
    }
    ++layouts_loaded_;
    store_->Keep(s, l);
    return l;
  }

  std::unique_ptr<FileHandle> packed_;
  const PackHeader h_;
  const std::vector<PackSegment> segments_;
  const std::vector<PackSound> sounds_;
  std::mutex mutex_;
  const std::shared_ptr<LayoutStore> store_ = LayoutStoreFor(path_);
};

}  // namespace

bool HasXmoSuffix(std::string_view name) { return HasSuffix(name, kXmoSuffix); }
bool HasXmopSuffix(std::string_view name) { return HasSuffix(name, kXmopSuffix); }

bool ReadXmoSize(const std::filesystem::path& path, uint64_t* out_size) {
  auto f = FileHandle::OpenExisting(path, FileAccess::kFileReadData);
  Header h;
  if (!f || !ReadHeader(*f, &h)) return false;
  *out_size = h.size;
  return true;
}

bool ReadXmopSize(const std::filesystem::path& path, uint64_t* out_size) {
  auto f = Unpacked(FileHandle::OpenExisting(path, FileAccess::kFileReadData));
  uint8_t raw[kHeaderSize];
  PackHeader h;
  if (!f || !ReadExactly(*f, 0, raw, sizeof(raw)) || !ParsePackHeader(raw, &h)) return false;
  *out_size = h.size;
  return true;
}

std::unique_ptr<FileHandle> OpenXmo(std::unique_ptr<FileHandle> packed) {
  Header h;
  if (!packed || !ReadHeader(*packed, &h)) {
    REXFS_WARN("[xmo] {}: not a valid .xmo file", packed ? rex::path_to_utf8(packed->path()) : std::string("?"));
    return nullptr;
  }
  std::vector<uint8_t> riff(h.data_offset), packets(size_t(h.packets) * 4), table(size_t(h.frames) * 8),
      trailer(h.trailer);
  uint64_t at = kHeaderSize;
  if (!ReadExactly(*packed, at, riff.data(), riff.size()) ||
      !ReadExactly(*packed, at += riff.size(), packets.data(), packets.size()) ||
      !ReadExactly(*packed, at += packets.size(), table.data(), table.size()) ||
      (h.trailer && !ReadExactly(*packed, at += table.size(), trailer.data(), trailer.size()))) {
    REXFS_WARN("[xmo] {}: truncated", rex::path_to_utf8(packed->path()));
    return nullptr;
  }
  std::vector<Frame> frames;
  if (!ParseFrames(table.data(), h.frames, h.packets, &frames)) {
    REXFS_WARN("[xmo] {}: a frame is not valid", rex::path_to_utf8(packed->path()));
    return nullptr;
  }
  Register(packed->path(), h);
  return std::make_unique<XmoHandle>(std::move(packed), h, std::move(riff), std::move(packets), std::move(frames),
                                     std::move(trailer));
}

std::unique_ptr<FileHandle> OpenXmop(std::unique_ptr<FileHandle> packed) {
  if (!packed) return nullptr;
  const std::filesystem::path path = packed->path();
  packed = Unpacked(std::move(packed));  // NAME.xmop.lz4b
  uint64_t file_size = 0;
  if (!packed || !UnpackedSize(path, &file_size)) {
    REXFS_WARN("[xmo] {}: too short to be a .xmop package", rex::path_to_utf8(path));
    return nullptr;
  }
  return OpenXmop(std::move(packed), file_size);
}

// The PopCap ports (2026-11-24): the package as it is and its size given (an entry of the asset pack).
std::unique_ptr<FileHandle> OpenXmop(std::unique_ptr<FileHandle> packed, uint64_t file_size) {
  if (!packed) return nullptr;
  const std::filesystem::path path = packed->path();
  auto bad = [&](const char* why) {
    REXFS_WARN("[xmo] {}: {}", rex::path_to_utf8(path), why);
    return std::unique_ptr<FileHandle>();
  };
  if (file_size < kHeaderSize) return bad("too short to be a .xmop package");
  uint8_t raw[kHeaderSize];
  PackHeader h;
  if (!ReadExactly(*packed, 0, raw, sizeof(raw)) || !ParsePackHeader(raw, &h)) return bad("not a .xmop package");
  auto within = [&](uint64_t offset, uint64_t length) { return offset <= file_size && length <= file_size - offset; };
  if (!within(h.segments_offset, uint64_t(h.segments) * kSegmentSize) || !within(h.stored_offset, h.stored_length) ||
      !within(h.sounds_offset, uint64_t(h.sounds) * kSoundEntrySize)) {
    return bad("its tables are not inside it");
  }
  std::vector<PackSound> sounds(h.sounds);
  {
    std::vector<uint8_t> t(size_t(h.sounds) * kSoundEntrySize);
    if (!t.empty() && !ReadExactly(*packed, h.sounds_offset, t.data(), t.size())) return bad("its sounds could not be read");
    for (uint32_t i = 0; i < h.sounds; ++i) {
      PackSound& s = sounds[i];
      if (!ParsePackSound(t.data() + size_t(i) * kSoundEntrySize, &s) ||
          !within(s.headers, uint64_t(s.packets) * 4) || !within(s.frame_table, uint64_t(s.frames) * 8) ||
          !within(s.opus_table, uint64_t(s.opus_count) * 2)) {
        return bad("a sound of its table is not valid");
      }
    }
  }
  std::vector<PackSegment> segments(h.segments);
  {
    std::vector<uint8_t> t(size_t(h.segments) * kSegmentSize);
    if (!ReadExactly(*packed, h.segments_offset, t.data(), t.size())) return bad("its segments could not be read");
    uint64_t expect = 0;
    for (uint32_t i = 0; i < h.segments; ++i) {
      const uint8_t* p = t.data() + size_t(i) * kSegmentSize;
      PackSegment& s = segments[i];
      s.offset = Le64(p);
      s.length = Le64(p + 8);
      s.kind = Le32(p + 16);
      s.sound = Le32(p + 20);
      s.source = Le64(p + 24);
      bool ok = s.offset == expect && s.length > 0;
      if (s.kind == 0)
        ok = ok && s.source <= h.stored_length && s.length <= h.stored_length - s.source;
      else if (s.kind == 2)
        ok = ok && s.sound < h.sounds &&
             s.source + s.length <= uint64_t(sounds[s.sound].packets) * kXmoPacketBytes;
      else
        ok = ok && s.kind == 1;
      if (!ok) return bad("its segments do not cover the package in order");
      expect += s.length;
    }
    if (expect != h.size) return bad("its segments do not end at its size");
  }
  RegisterPack(path, sounds);
  REXFS_INFO("[xmo] {}: {} of its sounds in Opus, {:.1f} MB as the package it stands for", rex::path_to_utf8(path.filename()),
             sounds.size(), double(h.size) / 1048576.0);
  PrefetchLayouts(path, sounds);  // zumasrevenge-nx (2026-11-25): the sounds' tables read ahead by the loader thread
  return std::make_unique<XmopHandle>(std::move(packed), h, std::move(segments), std::move(sounds));
}

bool AnyXmoSound() { return g_any_sound.load(std::memory_order_relaxed); }

void SetXmoSourceOpener(XmoSourceOpener opener) { g_opener.store(opener, std::memory_order_release); }

bool XmoSoundKnown(uint32_t id) {
  std::lock_guard lock(g_mutex);
  return g_sounds.count(id) != 0;
}

std::shared_ptr<const XmoSound> FindXmoSound(uint32_t id) {
  Registered r;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_sounds.find(id);
    if (it == g_sounds.end()) return nullptr;
    if (it->second.sound) {
      if (g_loaded.front() != id) {
        g_loaded.remove(id);
        g_loaded.push_front(id);
      }
      return it->second.sound;
    }
    r = it->second;
  }
  auto s = Load(r);  // outside the lock: a read from the SD
  if (!s) {
    REXFS_WARN("[xmo] {}: sound {:08X}'s Opus data could not be read", rex::path_to_utf8(r.path), id);
    return nullptr;
  }
  std::lock_guard lock(g_mutex);
  auto it = g_sounds.find(id);
  if (it == g_sounds.end()) return s;
  if (!it->second.sound) {
    it->second.sound = s;
    g_loaded.push_front(id);
    g_loaded_bytes += s->opus.size();
    // let go of the least recently used (a decoder still holding one keeps it alive)
    while (g_loaded_bytes > kLoadedLimit && g_loaded.size() > 1) {
      const uint32_t old = g_loaded.back();
      g_loaded.pop_back();
      auto o = g_sounds.find(old);
      if (o != g_sounds.end() && o->second.sound) {
        g_loaded_bytes -= o->second.sound->opus.size();
        o->second.sound.reset();
      }
    }
  }
  return it->second.sound;
}

int64_t FindXmoFrame(const XmoSound& sound, uint64_t bit_pos) {
  auto it = std::lower_bound(sound.frame_pos.begin(), sound.frame_pos.end(), bit_pos);
  return it != sound.frame_pos.end() && *it == bit_pos ? int64_t(it - sound.frame_pos.begin()) : -1;
}

uint32_t XmoTagBits(const uint8_t* packet, uint16_t* bits, uint32_t max) {
  if (packet[3] == 0xFF) return 0;  // no frame starts in it
  const uint32_t first =
      ((uint32_t(packet[0] & 3) << 13) | (uint32_t(packet[1]) << 5) | (uint32_t(packet[2]) >> 3)) + kPacketHeaderBits;
  uint32_t n = 0;
  // the frames that start here, as XmaContext::GetPacketInfo walks them
  for (uint32_t off = first; off < kPacketBits && n < max;) {
    if (kPacketBits - off < kFrameHeaderBits) break;  // a header split across packets
    uint32_t len = 0;
    for (uint32_t k = 0; k < kFrameHeaderBits; ++k) len = (len << 1) | Bit(packet, off + k);
    if (len == 0 || len == kMaxFrameLength) break;
    const uint32_t end = std::min(off + len - 1, kPacketBits);  // the last bit is not a tag bit
    for (uint32_t b = off + kFrameHeaderBits; b < end && n < max; ++b) bits[n++] = uint16_t(b);
    if (off + len > kPacketBits) break;  // it goes on in the next packet
    const uint32_t last = Bit(packet, off + len - 1);
    off += len;
    if (!last) break;
  }
  // then the end of a frame that began in an earlier packet
  for (uint32_t b = kPacketHeaderBits + kFrameHeaderBits; b + 1 < std::min(first, kPacketBits) && n < max; ++b)
    bits[n++] = uint16_t(b);
  return n;
}

bool ReadXmoPacketTag(const uint8_t* packet, uint32_t* id, uint32_t* packet_index) {
  uint16_t bits[kXmoTagBits];
  if (XmoTagBits(packet, bits, kXmoTagBits) < kXmoTagBits) return false;
  uint32_t magic = 0, sound = 0, index = 0;
  for (uint32_t k = 0; k < 16; ++k) magic = (magic << 1) | Bit(packet, bits[k]);
  if (magic != kXmoTagMagic) return false;
  for (uint32_t k = 16; k < 48; ++k) sound = (sound << 1) | Bit(packet, bits[k]);
  for (uint32_t k = 48; k < kXmoTagBits; ++k) index = (index << 1) | Bit(packet, bits[k]);
  *id = sound;
  *packet_index = index;
  return true;
}

}  // namespace rex::filesystem
