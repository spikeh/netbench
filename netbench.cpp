#include <boost/algorithm/string/join.hpp>
#include <boost/align/aligned_allocator.hpp>
#include <boost/core/noncopyable.hpp>
#include <numeric>
#include <string_view>
#include <thread>
#include <unordered_set>

#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <unistd.h>

#include <netinet/in.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/times.h>

#include "control.h"
#include "sender.h"
#include "socket.h"
#include "util.h"

namespace po = boost::program_options;

namespace {
#ifndef __NR_io_uring_enter
static constexpr int __NR_io_uring_enter = 426;
#endif
#ifndef __NR_io_uring_register
static constexpr int __NR_io_uring_register = 427;
#endif

static inline int ____sys_io_uring_enter2(
    int fd,
    unsigned to_submit,
    unsigned min_complete,
    unsigned flags,
    sigset_t* sig,
    int sz) {
  int ret;

  ret =
      syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, sig, sz);
  return (ret < 0) ? -errno : ret;
}

} // namespace

/*
 * Network benchmark tool.
 *
 * This tool will benchmark network coordinator stacks. specifically looking at
 * io_uring vs epoll.
 * The approach is to setup a single threaded receiver, and then spawn up N
 * threads with M connections. They wijll then send some requests, where a
 * request is a single (host endian) 32 bit unsigned int indicating length, and
 * then that number of bytes. The receiver when it collects a single "request"
 * will respond with a single byte (contents unimportant). The sender can then
 * treat this as a completed transaction and add it to it's stats.
 *
 */

std::atomic<bool> globalShouldShutdown{false};
void intHandler(int dummy) {
  if (globalShouldShutdown.load()) {
    die("already should have shutdown at signal");
  }
  globalShouldShutdown = true;
}

enum class RxEngine { IoUring, Epoll };
struct RxConfig {
  int backlog = 100000;
  int max_events = 32;
  int recv_size = 4096;
  bool recvmsg = false;
  size_t workload = 0;
  std::string description;

  std::string describe() const {
    if (description.empty()) {
      return toString();
    }
    return description;
  }

  virtual std::string const toString() const {
    // only give the important options:
    auto is_default = [this](auto RxConfig::*x) {
      RxConfig base;
      return this->*x == base.*x;
    };
    return strcat(
        is_default(&RxConfig::recvmsg) ? "" : strcat(" recvmsg=", recvmsg),
        is_default(&RxConfig::workload) ? "" : strcat(" workload=", workload));
  }
};

struct IoUringRxConfig : RxConfig {
  bool supports_nonblock_accept = false;
  bool register_ring = true;
  int provide_buffers = 2;
  bool fixed_files = true;
  int sqe_count = 64;
  int cqe_count = 0;
  int max_cqe_loop = 256 * 32;
  int provided_buffer_count = 8000;
  int fixed_file_count = 16000;
  int provided_buffer_low_watermark = -1;
  int provided_buffer_compact = 1;
  bool huge_pages = false;
  int multishot_recv = 1;
  bool defer_taskrun = false;

  // not for actual user updating, but dependent on the kernel:
  unsigned int cqe_skip_success_flag = 0;

  std::string const toString() const override {
    // only give the important options:
    auto is_default = [this](auto IoUringRxConfig::*x) {
      IoUringRxConfig base;
      return this->*x == base.*x;
    };
    return strcat(
        RxConfig::toString(),
        (!is_default(&IoUringRxConfig::fixed_files) ||
         !is_default(&IoUringRxConfig::fixed_file_count))
            ? strcat(
                  " fixed_files=",
                  fixed_files ? strcat("1 (count=", fixed_file_count, ")")
                              : strcat("0"))
            : "",
        is_default(&IoUringRxConfig::provide_buffers)
            ? ""
            : strcat(" provide_buffers=", provide_buffers),
        is_default(&IoUringRxConfig::provided_buffer_count)
            ? ""
            : strcat(" provided_buffer_count=", provided_buffer_count),
        is_default(&IoUringRxConfig::sqe_count)
            ? ""
            : strcat(" sqe_count=", sqe_count),
        is_default(&IoUringRxConfig::cqe_count)
            ? ""
            : strcat(" cqe_count=", cqe_count),
        is_default(&IoUringRxConfig::max_cqe_loop)
            ? ""
            : strcat(" max_cqe_loop=", max_cqe_loop),
        is_default(&IoUringRxConfig::huge_pages)
            ? ""
            : strcat(" huge_pages=", huge_pages),
        is_default(&IoUringRxConfig::defer_taskrun)
            ? ""
            : strcat(" defer_taskrun=", defer_taskrun),
        is_default(&IoUringRxConfig::multishot_recv)
            ? ""
            : strcat(" multishot_recv=", multishot_recv));
  }
};

struct EpollRxConfig : RxConfig {
  bool batch_send = false;

  std::string const toString() const override {
    // only give the important options:
    auto is_default = [this](auto EpollRxConfig::*x) {
      EpollRxConfig base;
      return this->*x == base.*x;
    };
    return strcat(
        RxConfig::toString(),
        is_default(&EpollRxConfig::batch_send)
            ? ""
            : strcat(" batch_send=", batch_send));
  }
};

struct Config {
  std::vector<uint16_t> use_port;
  uint16_t control_port = 0;
  bool client_only = false;
  bool server_only = false;
  GlobalSendOptions send_options;

  bool print_rx_stats = true;
  bool print_read_stats = true;
  std::vector<std::string> tx;
  std::vector<std::string> rx;
};

int mkServerSock(
    RxConfig const& rx_cfg,
    uint16_t port,
    bool const isv6,
    int extra_flags) {
  int fd = checkedErrno(mkBoundSock(port, isv6, extra_flags));
  checkedErrno(listen(fd, rx_cfg.backlog), "listen");
  vlog("made sock ", fd, " v6=", isv6, " port=", port);
  return fd;
}

std::pair<struct io_uring, IoUringRxConfig> mkIoUring(
    IoUringRxConfig const& rx_cfg) {
  struct io_uring_params params;
  struct io_uring ring;
  memset(&params, 0, sizeof(params));

  // default to Nx sqe_count as we are very happy to submit multiple sqe off one
  // cqe (eg send,read) and this can build up quickly
  int cqe_count =
      rx_cfg.cqe_count <= 0 ? 128 * rx_cfg.sqe_count : rx_cfg.cqe_count;

  unsigned int newer_flags =
      IORING_SETUP_SUBMIT_ALL | IORING_SETUP_COOP_TASKRUN;

  params.flags = newer_flags;
  params.flags |= IORING_SETUP_CQSIZE;

  if (rx_cfg.defer_taskrun) {
    params.flags |= IORING_SETUP_DEFER_TASKRUN;
    params.flags |= IORING_SETUP_SINGLE_ISSUER;
    params.flags |= IORING_SETUP_R_DISABLED;
  }

  params.cq_entries = cqe_count;
  int ret = io_uring_queue_init_params(rx_cfg.sqe_count, &ring, &params);
  if (ret < 0) {
    log("trying init again without COOP_TASKRUN or SUBMIT_ALL");
    params.flags = params.flags & (~newer_flags);
    checkedErrno(
        io_uring_queue_init_params(rx_cfg.sqe_count, &ring, &params),
        "io_uring_queue_init_params");
  }

  auto ret_cfg = rx_cfg;
  if (params.features & IORING_FEAT_CQE_SKIP) {
    ret_cfg.cqe_skip_success_flag = IOSQE_CQE_SKIP_SUCCESS;
  }
  return std::make_pair(ring, std::move(ret_cfg));
}

void runWorkload(RxConfig const& cfg, uint32_t consumed) {
  if (!cfg.workload)
    return;
  runWorkload(consumed, cfg.workload);
}

struct ConsumeResults {
  size_t to_write = 0;
  uint32_t count = 0;

  ConsumeResults& operator+=(ConsumeResults const& rhs) {
    to_write += rhs.to_write;
    count += rhs.count;
    return *this;
  }
};

// benchmark protocol is <uint32_t length>:<payload of size length>
// response is a single byte when it is received
struct ProtocolParser {
  // consume data and return number of new sends

  ConsumeResults consume(char const* data, size_t n) {
    ConsumeResults ret;
    while (n > 0) {
      so_far += n;
      if (!is_reading[0]) {
        if (likely(n >= sizeof(is_reading) && size_buff_have == 0)) {
          size_buff_have = sizeof(is_reading);
          memcpy(&is_reading, data, sizeof(is_reading));
        } else {
          uint32_t size_buff_add =
              std::min<uint32_t>(n, sizeof(is_reading) - size_buff_have);
          memcpy(size_buff + size_buff_have, data, size_buff_add);
          size_buff_have += size_buff_add;
          if (size_buff_have >= sizeof(is_reading)) {
            memcpy(&is_reading, size_buff, sizeof(is_reading));
          }
        }
      }
      if (is_reading[0] && so_far >= is_reading[0] + sizeof(is_reading)) {
        data += n;
        n = so_far - (is_reading[0] + sizeof(is_reading));
        ret.to_write += is_reading[1];
        ret.count++;
        so_far = size_buff_have = 0;
        memset(&is_reading, 0, sizeof(is_reading));
      } else {
        break;
      }
    }
    return ret;
  }

  uint32_t size_buff_have = 0;
  std::array<uint32_t, 2> is_reading = {{0}};
  char size_buff[sizeof(is_reading)];
  uint32_t so_far = 0;
};

class RxStats {
 public:
  RxStats(std::string const& name, bool countReads)
      : name_(name), countReads_(countReads) {
    auto const now = std::chrono::steady_clock::now();
    started_ = lastStats_ = now;
    lastClock_ = checkedErrno(times(&lastTimes_), "initial times");
    if (countReads_) {
      reads_.reserve(32000);
    }
  }

  void startWait() {
    waitStarted_ = std::chrono::steady_clock::now();
  }

  void doneWait() {
    auto now = std::chrono::steady_clock::now();
    // anything under 100us seems to be very noisy
    static constexpr std::chrono::microseconds kEpsilon{100};
    if (now > waitStarted_ + kEpsilon) {
      idle_ += (now - waitStarted_);
    }
  }

  void doneLoop(
      size_t bytes,
      size_t requests,
      unsigned int reads,
      bool is_overflow = false) {
    auto const now = std::chrono::steady_clock::now();
    auto const duration = now - lastStats_;
    ++loops_;

    if (is_overflow) {
      ++overflows_;
    }

    if (countReads_) {
      reads_.push_back(reads);
    }

    if (duration >= std::chrono::seconds(1)) {
      doLog(bytes, requests, now, duration);
    }
  }

 private:
  std::chrono::milliseconds getMs(clock_t from, clock_t to) {
    return std::chrono::milliseconds(
        to <= from ? 0llu : (((to - from) * 1000llu) / ticksPerSecond_));
  }

  template <size_t N>
  int getReadStats(std::array<char, N>& arr) {
    if (!reads_.size()) {
      return 0;
    }

    std::sort(reads_.begin(), reads_.end());
    size_t tot = std::accumulate(reads_.begin(), reads_.end(), size_t(0));
    double avg = tot / (double)reads_.size();
    unsigned int p10 = reads_[reads_.size() / 10];
    unsigned int p50 = reads_[reads_.size() / 2];
    unsigned int p90 = reads_[(int)(reads_.size() * 0.9)];
    return snprintf(
        arr.data(),
        arr.size(),
        " read_per_loop: p10=%u p50=%u p90=%u avg=%.2f",
        p10,
        p50,
        p90,
        avg);
  }

  void doLog(
      size_t bytes,
      size_t requests,
      std::chrono::steady_clock::time_point now,
      std::chrono::steady_clock::duration duration) {
    using namespace std::chrono;
    uint64_t const millis = duration_cast<milliseconds>(duration).count();
    double bps = ((bytes - lastBytes_) * 1000.0) / millis;
    double rps = ((requests - lastRequests_) * 1000.0) / millis;
    struct tms times_now {};
    clock_t clock_now = checkedErrno(::times(&times_now), "loop times");

    if (requests > lastRequests_ && lastRps_) {
      char buff[2048];
      // use snprintf as I like the floating point formatting
      int written = snprintf(
          buff,
          sizeof(buff),
          "%s: rps:%6.2fk Bps:%6.2fM idle=%lums "
          "user=%lums system=%lums wall=%lums loops=%lu overflows=%lu",
          name_.c_str(),
          rps / 1000.0,
          bps / 1000000.0,
          duration_cast<milliseconds>(idle_).count(),
          getMs(lastTimes_.tms_utime, times_now.tms_utime).count(),
          getMs(lastTimes_.tms_stime, times_now.tms_stime).count(),
          getMs(lastClock_, clock_now).count(),
          loops_,
          overflows_);
      if (written >= 0) {
        std::array<char, 2048> read_stats_buf;
        std::string_view read_stats;
        if (countReads_) {
          read_stats = std::string_view(
              read_stats_buf.data(), getReadStats(read_stats_buf));
          reads_.clear();
        }

        log(std::string_view(buff, written), read_stats);
      }
    }
    loops_ = overflows_ = 0;
    idle_ = steady_clock::duration{0};
    lastClock_ = clock_now;
    lastTimes_ = times_now;
    lastBytes_ = bytes;
    lastRequests_ = requests;
    lastStats_ = now;
    lastRps_ = rps;
  }

 private:
  std::string const& name_;
  bool const countReads_;
  std::vector<unsigned int> reads_;
  std::chrono::steady_clock::time_point started_ =
      std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point lastStats_ =
      std::chrono::steady_clock::now();

  std::chrono::steady_clock::time_point waitStarted;
  std::chrono::steady_clock::duration totalWaited{0};
  uint64_t ticksPerSecond_ = sysconf(_SC_CLK_TCK);
  struct tms lastTimes_;
  clock_t lastClock_;
  uint64_t loops_ = 0;
  uint64_t overflows_ = 0;

  std::chrono::steady_clock::time_point waitStarted_;
  std::chrono::steady_clock::duration idle_{0};
  size_t lastBytes_ = 0;
  size_t lastRequests_ = 0;
  size_t lastRps_ = 0;
};

class RunnerBase {
 public:
  explicit RunnerBase(std::string const& name) : name_(name) {}
  std::string const& name() const {
    return name_;
  }
  virtual void start() {}
  virtual void loop(std::atomic<bool>* should_shutdown) = 0;
  virtual void stop() = 0;
  virtual void addListenSock(int fd, bool v6) = 0;
  virtual ~RunnerBase() = default;

 protected:
  void didRead(int x) {
    bytesRx_ += x;
  }

  void finishedRequests(int n) {
    requestsRx_ += n;
  }

  void newSock() {
    socks_++;
    if (socks_ % 100 == 0) {
      vlog("add sock: now ", socks_);
    }
  }

  void delSock() {
    socks_--;
    if (socks_ % 100 == 0) {
      vlog("del sock: now ", socks_);
    }
  }

  int socks() const {
    return socks_;
  }

  size_t requestsRx_ = 0;
  size_t bytesRx_ = 0;

 private:
  std::string const name_;
  int socks_ = 0;
};

class NullRunner : public RunnerBase {
 public:
  explicit NullRunner(std::string const& name) : RunnerBase(name) {}
  void loop(std::atomic<bool>*) override {}
  void stop() override {}
  void addListenSock(int fd, bool) override {
    close(fd);
  }
};

class BufferProviderV1 : private boost::noncopyable {
 public:
  static constexpr int kBgid = 1;

  explicit BufferProviderV1(IoUringRxConfig const& rx_cfg)
      : sizePerBuffer_(addAlignment(rx_cfg.recv_size)),
        lowWatermark_(rx_cfg.provided_buffer_low_watermark) {
    auto count = rx_cfg.provided_buffer_count;
    buffer_.resize(count * sizePerBuffer_);
    for (ssize_t i = 0; i < count; i++) {
      buffers_.push_back(buffer_.data() + i * sizePerBuffer_);
    }
    toProvide_.reserve(128);
    toProvide2_.reserve(128);
    toProvide_.emplace_back(0, count);
    toProvideCount_ = count;
  }

  size_t count() const {
    return buffers_.size();
  }

  size_t sizePerBuffer() const {
    return sizePerBuffer_;
  }

  size_t toProvideCount() const {
    return toProvideCount_;
  }

  bool canProvide() const {
    return toProvide_.size();
  }

  bool needsToProvide() const {
    return toProvideCount_ > lowWatermark_;
  }

  void initialRegister(struct io_uring*) {}

  void compact() {
    if (toProvide_.size() <= 1) {
      return;
    } else if (toProvide_.size() == 2) {
      // actually a common case due to the way the kernel internals work
      if (toProvide_[0].merge(toProvide_[1])) {
        toProvide_.pop_back();
      }
      return;
    }
    auto was = toProvide_.size();
    std::sort(
        toProvide_.begin(), toProvide_.end(), [](auto const& a, auto const& b) {
          return a.sortable < b.sortable;
        });
    toProvide2_.clear();
    toProvide2_.push_back(toProvide_[0]);
    for (size_t i = 1; i < toProvide_.size(); i++) {
      auto const& p = toProvide_[i];
      if (!toProvide2_.back().merge(p)) {
        toProvide2_.push_back(p);
      }
    }
    toProvide_.swap(toProvide2_);
    if (unlikely(isVerbose())) {
      vlog("compact() was ", was, " now ", toProvide_.size());
      for (auto const& t : toProvide_) {
        vlog("...", t.start, " count=", t.count);
      }
    }
  }

  void returnIndex(uint16_t i) {
    if (toProvide_.empty()) {
      toProvide_.emplace_back(i);
    } else if (toProvide_.back().merge(i)) {
      // yay, nothing to do
    } else if (
        toProvide_.size() >= 2 && toProvide_[toProvide_.size() - 2].merge(i)) {
      // yay too, try merge these two. this accounts for out of order by 1 index
      // where we receive 1,3,2. so we merge 2 into 3, and then (2,3) into 1
      if (toProvide_[toProvide_.size() - 2].merge(toProvide_.back())) {
        toProvide_.pop_back();
      }
    } else {
      toProvide_.emplace_back(i);
    }
    ++toProvideCount_;
  }

  void provide(struct io_uring_sqe* sqe) {
    Range const& r = toProvide_.back();
    io_uring_prep_provide_buffers(
        sqe, buffers_[r.start], sizePerBuffer_, r.count, kBgid, r.start);
    sqe->flags |= IOSQE_CQE_SKIP_SUCCESS;
    toProvideCount_ -= r.count;
    toProvide_.pop_back();
    assert(toProvide_.size() != 0 || toProvideCount_ == 0);
  }

  char const* getData(uint16_t i) const {
    return buffers_[i];
  }

 private:
  static constexpr int kAlignment = 32;

  size_t addAlignment(size_t n) {
    return kAlignment * ((n + kAlignment - 1) / kAlignment);
  }

  struct Range {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    explicit Range(uint16_t idx, uint16_t count = 1)
        : count(count), start(idx) {}
#else
    explicit Range(uint16_t idx, uint16_t count = 1)
        : start(idx), count(count) {}
#endif
    union {
      struct {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        uint16_t count;
        uint16_t start;
#else
        uint16_t start;
        uint16_t count;
#endif
      };
      uint32_t sortable; // big endian might need to swap these around
    };

    bool merge(uint16_t idx) {
      if (idx == start - 1) {
        start = idx;
        count++;
        return true;
      } else if (idx == start + count) {
        count++;
        return true;
      } else {
        return false;
      }
    }

    bool merge(Range const& r) {
      if (start + count == r.start) {
        count += r.count;
        return true;
      } else if (r.start + r.count == start) {
        count += r.count;
        start = r.start;
        return true;
      } else {
        return false;
      }
    }
  };

  size_t sizePerBuffer_;
  std::vector<char, boost::alignment::aligned_allocator<char, kAlignment>>
      buffer_;
  std::vector<char*> buffers_;
  ssize_t toProvideCount_ = 0;
  int lowWatermark_;
  std::vector<Range> toProvide_;
  std::vector<Range> toProvide2_;
};

class BufferProviderV2 : private boost::noncopyable {
 private:
 public:
  static constexpr int kBgid = 1;
  static constexpr size_t kHugePageMask = (1LLU << 21) - 1; // 2MB
  static constexpr size_t kBufferAlignMask = 31LLU;

  explicit BufferProviderV2(IoUringRxConfig const& rx_cfg)
      : count_(rx_cfg.provided_buffer_count),
        sizePerBuffer_(addAlignment(rx_cfg.recv_size)) {
    ringSize_ = 1;
    ringMask_ = 0;
    while (ringSize_ < count_) {
      ringSize_ *= 2;
    }
    ringMask_ = io_uring_buf_ring_mask(ringSize_);

    int extra_mmap_flags = 0;
    char* buffer_base;

    ringMemSize_ = ringSize_ * sizeof(struct io_uring_buf);
    ringMemSize_ = (ringMemSize_ + kBufferAlignMask) & (~kBufferAlignMask);
    bufferMmapSize_ = count_ * sizePerBuffer_ + ringMemSize_;
    size_t page_mask = 4095;

    if (rx_cfg.huge_pages) {
      bufferMmapSize_ = (bufferMmapSize_ + kHugePageMask) & (~kHugePageMask);
      extra_mmap_flags |= MAP_HUGETLB;
      page_mask = kHugePageMask;
      checkHugePages(bufferMmapSize_ / (1 + kHugePageMask));
    }

    bufferMmap_ = mmap(
        NULL,
        bufferMmapSize_,
        PROT_READ | PROT_WRITE,
        MAP_ANONYMOUS | MAP_PRIVATE | extra_mmap_flags,
        -1,
        0);
    vlog(
        "mmap buffer size=",
        bufferMmapSize_,
        " ring size=",
        ringMemSize_,
        " pages=",
        bufferMmapSize_ / (1 + page_mask));
    if (bufferMmap_ == MAP_FAILED) {
      auto errnoCopy = errno;
      die("unable to allocate pages of size ",
          bufferMmapSize_,
          ": ",
          strerror(errnoCopy));
    }
    buffer_base = (char*)bufferMmap_ + ringMemSize_;
    ring_ = (struct io_uring_buf_ring*)bufferMmap_;
    io_uring_buf_ring_init(ring_);
    for (size_t i = 0; i < count_; i++) {
      buffers_.push_back(buffer_base + i * sizePerBuffer_);
    }

    if (count_ >= std::numeric_limits<uint16_t>::max()) {
      die("buffer count too large: ", count_);
    }
    for (uint16_t i = 0; i < count_; i++) {
      ring_->bufs[i] = {};
      populate(ring_->bufs[i], i);
    }
    tailCached_ = count_;
    io_uring_smp_store_release(&ring_->tail, tailCached_);

    vlog(
        "ring address=",
        ring_,
        " ring size=",
        ringSize_,
        " buffer count=",
        count_,
        " ring_mask=",
        ringMask_,
        " tail now ",
        tailCached_);
  }

  ~BufferProviderV2() {
    munmap(bufferMmap_, bufferMmapSize_);
  }

  size_t count() const {
    return count_;
  }

  size_t sizePerBuffer() const {
    return sizePerBuffer_;
  }

  size_t toProvideCount() const {
    return cachedIndices;
  }

  bool canProvide() const {
    return false;
  }

  bool needsToProvide() const {
    return false;
  }

  void compact() {}

  inline void populate(struct io_uring_buf& b, uint16_t i) {
    b.bid = i;
    b.addr = (__u64)getData(i);

    // can we assume kernel doesnt touch len or resv?
    b.len = sizePerBuffer_;
    // b.resv = 0;
  }

  void returnIndex(uint16_t i) {
    indices[cachedIndices++] = i;
    if (likely(cachedIndices < indices.size())) {
      return;
    }
    cachedIndices = 0;
    for (uint16_t idx : indices) {
      populate(ring_->bufs[(tailCached_ & ringMask_)], idx);
      ++tailCached_;
    }

    io_uring_smp_store_release(&ring_->tail, tailCached_);
  }

  void provide(struct io_uring_sqe*) {}

  char const* getData(uint16_t i) const {
    return buffers_[i];
  }

  void initialRegister(struct io_uring* ring) {
    struct io_uring_buf_reg reg;
    memset(&reg, 0, sizeof(reg));
    reg.ring_addr = (__u64)ring_;
    reg.ring_entries = ringSize_;
    reg.bgid = kBgid;
    checkedErrno(io_uring_register_buf_ring(ring, &reg, 0), "register pbuf");
  }

 private:
  static constexpr int kAlignment = 32;

  size_t addAlignment(size_t n) {
    return kAlignment * ((n + kAlignment - 1) / kAlignment);
  }

  size_t count_;
  size_t sizePerBuffer_;
  size_t bufferMmapSize_ = 0;
  void* bufferMmap_ = nullptr;
  std::vector<char*> buffers_;
  uint16_t tailCached_ = 0;
  size_t ringMemSize_;
  uint32_t ringSize_;
  uint32_t ringMask_;
  uint32_t cachedIndices = 0;
  struct io_uring_buf_ring* ring_;
  std::array<uint16_t, 32> indices;
};

static constexpr int kUseBufferProviderFlag = 1;
static constexpr int kUseBufferProviderV2Flag = 2;

int providedBufferIdx(struct io_uring_cqe* cqe) {
  if (!(cqe->flags & IORING_CQE_F_BUFFER)) {
    return -1;
  }
  return cqe->flags >> 16;
}

template <size_t ReadSize = 4096, size_t Flags = 0>
struct BasicSock {
  static constexpr int kUseBufferProviderVersion =
      (Flags & kUseBufferProviderV2Flag) ? 2
      : (Flags & kUseBufferProviderFlag) ? 1
                                         : 0;

  using TBufferProvider = std::conditional_t<
      kUseBufferProviderVersion == 2,
      BufferProviderV2,
      BufferProviderV1>;

  bool isFixedFiles() const {
    return cfg_.fixed_files;
  }

  bool isMultiShotRecv() const {
    return cfg_.multishot_recv;
  }

  explicit BasicSock(IoUringRxConfig const& cfg, int fd) : cfg_(cfg), fd_(fd) {
    if (cfg_.recvmsg) {
      memset(&recvmsgHdr_, 0, sizeof(recvmsgHdr_));
      memset(&recvmsgHdrIoVec_, 0, sizeof(recvmsgHdrIoVec_));
      recvmsgHdr_.msg_iov = &recvmsgHdrIoVec_;
      recvmsgHdrIoVec_.iov_base = &buff[0];
      recvmsgHdrIoVec_.iov_len = ReadSize;
      if (isMultiShotRecv() || kUseBufferProviderVersion > 0) {
        recvmsgHdr_.msg_iovlen = 0;
      } else {
        recvmsgHdr_.msg_iovlen = 1;
      }
    }
  }

  ~BasicSock() {
    if (!closed_) {
      log("socket not closed at destruct");
    }
  }

  int fd() const {
    return fd_;
  }

  ConsumeResults const& peekSend() {
    return do_send;
  }

  void didSend() {
    do_send = {};
  }

  void addSend(struct io_uring_sqe* sqe, unsigned char* b, uint32_t len) {
    io_uring_prep_send(sqe, fd_, b, len, MSG_WAITALL);
    if (isFixedFiles()) {
      sqe->flags |= IOSQE_FIXED_FILE;
    }
    sqe->flags |= cfg_.cqe_skip_success_flag;
  }

  void addRead(struct io_uring_sqe* sqe, TBufferProvider& provider) {
    if (kUseBufferProviderVersion) {
      size_t const size = isMultiShotRecv() ? 0LLU : provider.sizePerBuffer();

      if (cfg_.recvmsg) {
        io_uring_prep_recvmsg(sqe, fd_, &recvmsgHdr_, 0);
        if (isMultiShotRecv()) {
          sqe->ioprio |= IORING_RECV_MULTISHOT;
        }
      } else {
        if (isMultiShotRecv()) {
          io_uring_prep_recv_multishot(sqe, fd_, NULL, size, 0);
        } else {
          io_uring_prep_recv(sqe, fd_, NULL, size, 0);
        }
      }
      sqe->flags |= IOSQE_BUFFER_SELECT;
      sqe->buf_group = TBufferProvider::kBgid;
    } else if (cfg_.recvmsg) {
      io_uring_prep_recvmsg(sqe, fd_, &recvmsgHdr_, 0);
    } else {
      io_uring_prep_recv(sqe, fd_, &buff[0], sizeof(buff), 0);
    }

    if (isFixedFiles()) {
      sqe->flags |= IOSQE_FIXED_FILE;
    }
  }

  bool closing() const {
    return closed_;
  }

  void doClose() {
    closed_ = true;
    ::close(fd_);
  }

  void addClose(struct io_uring_sqe* sqe) {
    closed_ = true;
    if (isFixedFiles())
      io_uring_prep_close_direct(sqe, fd_);
    else
      io_uring_prep_close(sqe, fd_);
  }

  struct DidReadResult {
    explicit DidReadResult(int amount, int idx)
        : amount(amount), recycleBufferIdx(idx) {}

    int amount;
    int recycleBufferIdx;
  };

  DidReadResult didRead(TBufferProvider& provider, struct io_uring_cqe* cqe) {
    // pull remaining data
    int res = cqe->res;
    if (res <= 0) {
      return DidReadResult(res, -1);
    }

    if (kUseBufferProviderVersion) {
      int recycleBufferIdx = providedBufferIdx(cqe);
      auto* data = provider.getData(recycleBufferIdx);

      if (isMultiShotRecv() && cfg_.recvmsg) {
        if (recycleBufferIdx < 0) {
          die("bad recycleBufferIdx");
        }
        if (!data) {
          die("bad data");
        }
        auto* m = io_uring_recvmsg_validate((void*)data, res, &recvmsgHdr_);
        if (!m) {
          return DidReadResult(0, recycleBufferIdx);
        }
        res = io_uring_recvmsg_payload_length(m, cqe->res, &recvmsgHdr_);
        data = (const char*)io_uring_recvmsg_payload(m, &recvmsgHdr_);
      }

      didRead(data, res);
      return DidReadResult(res, recycleBufferIdx);
    } else {
      didRead(buff, res);
      return DidReadResult(res, -1);
    }
  }

 private:
  void didRead(char const* b, size_t n) {
    auto consumed = parser.consume(b, n);
    runWorkload(cfg_, consumed.count);
    do_send += consumed;
  }

  IoUringRxConfig const cfg_;
  int fd_;
  ProtocolParser parser;
  ConsumeResults do_send;
  bool closed_ = false;
  struct msghdr recvmsgHdr_;
  struct iovec recvmsgHdrIoVec_;

  char buff[ReadSize];
};

struct ListenSock : private boost::noncopyable {
  ListenSock(int fd, bool v6) : fd(fd), isv6(v6) {}
  virtual ~ListenSock() {
    if (!closed) {
      ::close(fd);
    }
    vlog("close ListenSock");
  }

  void close() {
    ::close(fd);
    closed = true;
  }

  int fd;
  bool isv6;
  struct sockaddr_in addr;
  struct sockaddr_in6 addr6;
  socklen_t client_len;
  bool closed = false;
  int nextAcceptIdx = -1;
};

template <class TSock>
struct IOUringRunner : public RunnerBase {
  explicit IOUringRunner(
      Config const& cfg,
      IoUringRxConfig const& rx_cfg,
      struct io_uring r,
      std::string const& name)
      : RunnerBase(name), cfg_(cfg), rxCfg_(rx_cfg), ring(r), buffers_(rx_cfg) {
    sendBuff_.resize(2048);

    if (TSock::kUseBufferProviderVersion) {
      buffers_.initialRegister(&ring);
      provideBuffers(true);
      submit();
    }

    if (isFixedFiles()) {
      std::vector<int> files(rx_cfg.fixed_file_count, -1);
      checkedErrno(
          io_uring_register_files(&ring, files.data(), files.size()),
          "io_uring_register_files");
      for (int i = rx_cfg.fixed_file_count - 1; i >= 0; i--)
        acceptFdPool_.push_back(i);
    }
  }

  ~IOUringRunner() {
    if (socks()) {
      vlog(
          "IOUringRunner shutting down with ",
          socks(),
          " sockets still: stopping=",
          stopping);
    }

    io_uring_queue_exit(&ring);
  }

  void provideBuffers(bool force) {
    if (TSock::kUseBufferProviderVersion != 1) {
      return;
    }
    if (!(force || buffers_.needsToProvide())) {
      return;
    }

    if (rxCfg_.provided_buffer_compact) {
      buffers_.compact();
    }
    while (buffers_.canProvide()) {
      auto* sqe = get_sqe();
      buffers_.provide(sqe);
      io_uring_sqe_set_data(sqe, NULL);
    }
  }

  static constexpr int kAccept = 1;
  static constexpr int kRead = 2;
  static constexpr int kWrite = 3;
  static constexpr int kOther = 0;

  void addListenSock(int fd, bool v6) override {
    listeners_++;
    listenSocks_.push_back(std::make_unique<ListenSock>(fd, v6));
    addAccept(listenSocks_.back().get());
  }

  void addAccept(ListenSock* ls) {
    struct io_uring_sqe* sqe = get_sqe();
    struct sockaddr* addr;
    if (ls->isv6) {
      ls->client_len = sizeof(ls->addr6);
      addr = (struct sockaddr*)&ls->addr6;
    } else {
      ls->client_len = sizeof(ls->addr);
      addr = (struct sockaddr*)&ls->addr;
    }
    if (isFixedFiles()) {
      if (ls->nextAcceptIdx >= 0) {
        die("only allowed one accept at a time");
      }
      ls->nextAcceptIdx = nextFdIdx();
      io_uring_prep_accept_direct(
          sqe, ls->fd, addr, &ls->client_len, SOCK_NONBLOCK, ls->nextAcceptIdx);
    } else {
      io_uring_prep_accept(sqe, ls->fd, addr, &ls->client_len, SOCK_NONBLOCK);
    }
    io_uring_sqe_set_data(sqe, tag(ls, kAccept));
  }

  struct io_uring_sqe* get_sqe() {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      submit();
      sqe = io_uring_get_sqe(&ring);
      if (!sqe) {
        throw std::runtime_error("no sqe available");
      }
    }
    ++expected;
    return sqe;
  }

  void addRead(TSock* sock) {
    struct io_uring_sqe* sqe = get_sqe();
    sock->addRead(sqe, buffers_);
    io_uring_sqe_set_data(sqe, tag(sock, kRead));
  }

  void addSend(TSock* sock, uint32_t len) {
    if (unlikely(sendBuff_.size() < len)) {
      sendBuff_.resize(len);
    }
    struct io_uring_sqe* sqe = get_sqe();
    sock->addSend(sqe, sendBuff_.data(), len);
    io_uring_sqe_set_data(sqe, tag(sock, kWrite));
  }

  void processAccept(struct io_uring_cqe* cqe) {
    int fd = cqe->res;
    ListenSock* ls = untag<ListenSock>(cqe->user_data);
    if (fd >= 0) {
      int used_fd = fd;
      if (isFixedFiles()) {
        if (fd > 0) {
          die("trying to use fixed files, but got given an actual fd. "
              "implies that this kernel does not support this feature");
        }
        if (ls->nextAcceptIdx < 0) {
          die("no nextAcceptIdx");
        }
        used_fd = ls->nextAcceptIdx;
        ls->nextAcceptIdx = -1;
      }
      TSock* sock = new TSock(rxCfg_, used_fd);
      addRead(sock);
      newSock();
    } else if (!stopping) {
      die("unexpected accept result ",
          strerror(-fd),
          "(",
          fd,
          ") ud=",
          cqe->user_data);
    }

    if (stopping) {
      return;
    } else {
      if (rxCfg_.supports_nonblock_accept && !isFixedFiles()) {
        // get any outstanding sockets
        struct sockaddr_in addr;
        struct sockaddr_in6 addr6;
        socklen_t addrlen = ls->isv6 ? sizeof(addr6) : sizeof(addr);
        struct sockaddr* paddr =
            ls->isv6 ? (struct sockaddr*)&addr6 : (struct sockaddr*)&addr;
        while (1) {
          int sock_fd = accept4(ls->fd, paddr, &addrlen, SOCK_NONBLOCK);
          if (sock_fd == -1 && errno == EAGAIN) {
            break;
          } else if (sock_fd == -1) {
            checkedErrno(sock_fd, "accept4");
          }
          TSock* sock = new TSock(rxCfg_, sock_fd);
          addRead(sock);
          newSock();
        }
      }
      addAccept(untag<ListenSock>(cqe->user_data));
    }
  }

  void processClose(struct io_uring_cqe* cqe, TSock* sock) {
    int res = cqe->res;
    if (!res || res == -EBADF) {
      if (isFixedFiles()) {
        // recycle index
        acceptFdPool_.push_back(sock->fd());
      }
    } else {
      // cannot recycle
      log("unable to close fd, ret=", res);
    }
    delete sock;
  }

  void processRead(struct io_uring_cqe* cqe) {
    TSock* sock = untag<TSock>(cqe->user_data);
    auto res = sock->didRead(buffers_, cqe);

    if (res.recycleBufferIdx > 0) {
      buffers_.returnIndex(res.recycleBufferIdx);
      provideBuffers(false);
    }

    if (res.amount > 0) {
      if (auto const& sends = sock->peekSend(); sends.to_write > 0) {
        finishedRequests(sends.count);
        addSend(sock, sends.to_write);
        sock->didSend();
      }
      didRead(res.amount);
      if (!sock->isMultiShotRecv() || !(cqe->flags & IORING_CQE_F_MORE)) {
        addRead(sock);
      }
    } else if (res.amount <= 0) {
      if (unlikely(cqe->res == -ENOBUFS)) {
        die("not enough buffers, but will just requeue. so far have ",
            ++enobuffCount_,
            "state: can provide=",
            buffers_.toProvideCount(),
            " need=",
            buffers_.needsToProvide());
        addRead(sock);
        return;
      }
      if (cqe->res < 0 && !stopping) {
        if (unlikely(cqe->res != -ECONNRESET)) {
          log("unexpected read: ",
              cqe->res,
              "(",
              strerror(-cqe->res),
              ") deleting ",
              sock);
        }
      }

      if (isFixedFiles()) {
        auto* sqe = get_sqe();
        sock->addClose(sqe);
        io_uring_sqe_set_data(sqe, tag(sock, kOther));
      } else {
        sock->doClose();
        delete sock;
        delSock();
      }
    }
  }

  void processCqe(struct io_uring_cqe* cqe, unsigned int& reads) {
    switch (get_tag(cqe->user_data)) {
      case kAccept:
        processAccept(cqe);
        break;
      case kRead:
        ++reads;
        processRead(cqe);
        break;
      case kWrite:
        // be careful if you do something here as kRead might delete sockets.
        // this is ok as we only ever have one read outstanding
        // at once
        if (cqe->res < 0) {
          // we should track these down and make sure they only happen when the
          // sender socket is closed
          TSock* sock = untag<TSock>(cqe->user_data);
          if (!sock->closing()) {
            log("bad socket write ",
                cqe->res,
                " closing=",
                sock->closing(),
                " fd=",
                sock->fd());
          }
        }
        break;
      case kOther:
        if (cqe->user_data) {
          TSock* sock = untag<TSock>(cqe->user_data);
          if (sock->closing()) {
            // assume this was a close
            processClose(cqe, sock);
          }
        }
        break;
      default:
        if (cqe->user_data == LIBURING_UDATA_TIMEOUT) {
          break;
        }
        die("unexpected completion:", cqe->user_data);
        break;
    }
  }

  void submit() {
    while (expected) {
      int got = io_uring_submit(&ring);
      if (got != expected) {
        if (got == 0) {
          if (stopping) {
            // assume some kind of cancel issue?
            expected--;
          } else {
            die("literally submitted nothing, wanted ", expected);
          }
        }
      }
      expected -= got;
    }
  }

  int submitAndWait1(struct io_uring_cqe** cqe, struct __kernel_timespec* ts) {
    int got = checkedErrno(
        io_uring_submit_and_wait_timeout(&ring, cqe, 1, ts, NULL),
        "submit_and_wait_timeout");
    if (got >= 0) {
      // io_uring_submit_and_wait_timeout actually returns 0
      expected = 0;
      return 0;
    } else if (got == -ETIME || got == -EINTR) {
      return 0;
    } else {
      die("submit_and_wait_timeout failed with ", got);
      return got;
    }
  }

  bool isOverflow() const {
    return IO_URING_READ_ONCE(*ring.sq.kflags) & IORING_SQ_CQ_OVERFLOW;
  }

  // todo: replace with io_uring_flush_overflow when it lands
  int flushOverflow() const {
    int flags = IORING_ENTER_GETEVENTS;
    if (rxCfg_.register_ring) {
      flags |= IORING_ENTER_REGISTERED_RING;
    }
    return ____sys_io_uring_enter2(
        ring.enter_ring_fd, 0, 0, flags, NULL, _NSIG / 8);
  }

  void start() override {
  }

  void loop(std::atomic<bool>* should_shutdown) override {
    RxStats rx_stats{name(), cfg_.print_read_stats};
    struct __kernel_timespec timeout;
    timeout.tv_sec = 1;
    timeout.tv_nsec = 0;

    if (rxCfg_.register_ring) {
      io_uring_enable_rings(&ring);
      io_uring_register_ring_fd(&ring);
    }

    while (socks() || !stopping) {
      bool const was_overflow = isOverflow();
      unsigned int reads = 0;
      struct io_uring_cqe* cqe = nullptr;
      provideBuffers(false);

      rx_stats.startWait();

      if (was_overflow) {
        flushOverflow();
        rx_stats.doneWait();
      } else if (expected) {
        submitAndWait1(&cqe, &timeout);
        rx_stats.doneWait();
        // cqe might not be set here if we submitted
      } else {
        int wait_res = checkedErrno(
            io_uring_wait_cqe_timeout(&ring, &cqe, &timeout),
            "wait_cqe_timeout");

        rx_stats.doneWait();

        // can trust here that cqe will be set
        if (!wait_res && cqe) {
          processCqe(cqe, reads);
          io_uring_cqe_seen(&ring, cqe);
        }
      }

      if (should_shutdown->load() || globalShouldShutdown.load()) {
        if (stopping) {
          // eh we gave it a good try
          break;
        }
        vlog("stopping");
        stop();
        vlog("stopped");
        timeout.tv_sec = 0;
        timeout.tv_nsec = 100000000;
      }

      int cqe_count = 0;
      unsigned int head;
      io_uring_for_each_cqe(&ring, head, cqe) {
        processCqe(cqe, reads);
        cqe_count++;
      }
      io_uring_cq_advance(&ring, cqe_count);

      if (!cqe_count && stopping) {
        vlog("processed ", cqe_count, " socks()=", socks());
      }

      if (cfg_.print_rx_stats) {
        rx_stats.doneLoop(bytesRx_, requestsRx_, reads, was_overflow);
      }
    }
  }

  void stop() override {
    stopping = true;
    for (auto& l : listenSocks_) {
      l->close();
    }
  }

  int nextFdIdx() {
    if (acceptFdPool_.empty()) {
      die("no fd for accept");
    }
    int ret = acceptFdPool_.back();
    acceptFdPool_.pop_back();
    return ret;
  }

  static inline void* tag(void* ptr, int x) {
    size_t uptr;
    memcpy(&uptr, &ptr, sizeof(size_t));
#ifndef NDEBUG
    if (uptr & (size_t)0x0f) {
      die("bad ptr");
    }
    if (x > 4) {
      die("bad tag");
    }
#endif
    return (void*)(uptr | x);
  }

  template <class T>
  static T* untag(size_t ptr) {
    return (T*)(ptr & ~((size_t)0x0f));
  }

  static int get_tag(uint64_t ptr) {
    return (int)(ptr & 0x0f);
  }

  bool isFixedFiles() const {
    return rxCfg_.fixed_files;
  }

  Config cfg_;
  IoUringRxConfig rxCfg_;
  int expected = 0;
  bool stopping = false;
  struct io_uring ring;

  typename TSock::TBufferProvider buffers_;
  std::vector<std::unique_ptr<ListenSock>> listenSocks_;
  std::vector<unsigned char> sendBuff_;
  int listeners_ = 0;
  uint32_t enobuffCount_ = 0;
  std::vector<int> acceptFdPool_;
};

static constexpr uint32_t kSocket = 0;
static constexpr uint32_t kAccept4 = 1;
static constexpr uint32_t kAccept6 = 2;

struct EPollData {
  uint32_t type;
  int fd;
  size_t to_write = 0;
  bool write_in_epoll = false;
  ProtocolParser parser;
};

struct EPollRunner : public RunnerBase {
  explicit EPollRunner(
      Config const& cfg,
      EpollRxConfig const& rx_cfg,
      std::string const& name)
      : RunnerBase(name), cfg_(cfg), rxCfg_(rx_cfg) {
    epoll_fd = checkedErrno(epoll_create(rx_cfg.max_events), "epoll_create");
    rcvbuff.resize(rx_cfg.recv_size);
    events.resize(rx_cfg.max_events);

    memset(&recvmsgHdr_, 0, sizeof(recvmsgHdr_));
    memset(&recvmsgHdrIoVec_, 0, sizeof(recvmsgHdrIoVec_));
    recvmsgHdr_.msg_iov = &recvmsgHdrIoVec_;
    recvmsgHdr_.msg_iovlen = 1;
    recvmsgHdrIoVec_.iov_base = rcvbuff.data();
    recvmsgHdrIoVec_.iov_len = rcvbuff.size();
  }

  ~EPollRunner() {
    for (auto& l : listeners_) {
      close(l->fd);
    }
    for (auto* ed : sockets_) {
      close(ed->fd);
      delete ed;
    }
    close(epoll_fd);
    vlog("EPollRunner cleaned up");
  }

  void addListenSock(int fd, bool v6) override {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));

    auto ed = std::make_unique<EPollData>();
    ed->type = v6 ? kAccept6 : kAccept4;
    ed->fd = fd;
    ev.events = EPOLLIN;
    ev.data.ptr = ed.get();
    checkedErrno(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev), "epoll_add");
    listeners_.push_back(std::move(ed));
    vlog("listening on ", fd, " v=", v6);
  }

  void doSocket(
      EPollData* ed,
      uint32_t events,
      std::vector<EPollData*>& write_queue,
      unsigned int& reads) {
    if (events & EPOLLIN) {
      reads++;
      if (doRead(ed)) {
        return;
      }
    }
    if ((events & EPOLLOUT) || (ed->to_write && !rxCfg_.batch_send)) {
      doWrite(ed);
    } else if (ed->to_write) {
      write_queue.push_back(ed);
    }
  }

  void doWrite(EPollData* ed) {
    int res;

    while (ed->to_write) {
      res = send(
          ed->fd,
          rcvbuff.data(),
          std::min<size_t>(ed->to_write, rcvbuff.size()),
          MSG_NOSIGNAL);
      if (res < 0 && errno == EAGAIN) {
        break;
      }
      if (res < 0) {
        // something went wrong - probably socket is dead
        ed->to_write = 0;
      } else {
        ed->to_write -= std::min<uint32_t>(ed->to_write, res);
      }
    }

    if (ed->write_in_epoll && !ed->to_write) {
      struct epoll_event ev;
      memset(&ev, 0, sizeof(ev));
      ev.events = EPOLLIN;
      ev.data.ptr = ed;
      checkedErrno(
          epoll_ctl(epoll_fd, EPOLL_CTL_MOD, ed->fd, &ev),
          "epoll_remove_write");
      ed->write_in_epoll = false;
    } else if (!ed->write_in_epoll && ed->to_write) {
      struct epoll_event ev;
      memset(&ev, 0, sizeof(ev));
      ev.events = EPOLLIN | EPOLLOUT;
      ev.data.ptr = ed;
      checkedErrno(
          epoll_ctl(epoll_fd, EPOLL_CTL_MOD, ed->fd, &ev), "epoll_add_write");
      ed->write_in_epoll = true;
    }
  }

  int doRead(EPollData* ed) {
    int res;
    int fd = ed->fd;
    do {
      if (rxCfg_.recvmsg) {
        res = recvmsg(fd, &recvmsgHdr_, MSG_NOSIGNAL);
      } else {
        res = recv(fd, rcvbuff.data(), rcvbuff.size(), MSG_NOSIGNAL);
      }
      if (res <= 0) {
        int errnum = errno;
        if (res < 0 && errnum == EAGAIN) {
          return 0;
        }

        checkedErrno(
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL),
            "epoll_del fd=",
            fd,
            " res=",
            res,
            " errno=",
            errnum);
        delSock();
        close(fd);
        sockets_.erase(ed);
        delete ed;

        return -1;
      } else {
        didRead(res);
        auto consumed = ed->parser.consume(rcvbuff.data(), res);
        runWorkload(rxCfg_, consumed.count);
        finishedRequests(consumed.count);
        ed->to_write += consumed.to_write;
      }
    } while (res == (int)rcvbuff.size());
    return 0;
  }

  void doAccept(int fd, bool isv6) {
    struct sockaddr_in addr;
    struct sockaddr_in6 addr6;
    socklen_t addrlen = isv6 ? sizeof(addr6) : sizeof(addr);
    struct sockaddr* paddr =
        isv6 ? (struct sockaddr*)&addr6 : (struct sockaddr*)&addr;
    while (true) {
      int sock_fd = accept4(fd, paddr, &addrlen, SOCK_NONBLOCK);
      if (sock_fd == -1 && errno == EAGAIN) {
        break;
      } else if (sock_fd == -1) {
        checkedErrno(sock_fd, "accept4");
      }
      struct epoll_event ev;
      memset(&ev, 0, sizeof(ev));
      ev.events = EPOLLIN | EPOLLET;
      EPollData* ed = new EPollData();
      ed->type = kSocket;
      ed->fd = sock_fd;
      ev.data.ptr = ed;
      checkedErrno(
          epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_fd, &ev), "epoll add sock");
      sockets_.insert(ed);
      newSock();
    }
  }

  void stop() override {}

  void loop(std::atomic<bool>* should_shutdown) override {
    RxStats rx_stats{name(), cfg_.print_read_stats};
    std::vector<EPollData*> write_queue;
    write_queue.reserve(1024);
    while (!should_shutdown->load() && !globalShouldShutdown.load()) {
      rx_stats.startWait();
      int nevents = checkedErrno(
          epoll_wait(epoll_fd, events.data(), events.size(), 1000),
          "epoll_wait");
      rx_stats.doneWait();
      if (!nevents) {
        vlog("epoll: no events socks()=", socks());
      }
      unsigned int reads = 0;
      for (int i = 0; i < nevents; ++i) {
        EPollData* ed = (EPollData*)events[i].data.ptr;
        switch (ed->type) {
          case kAccept4:
            doAccept(ed->fd, false);
            break;
          case kAccept6:
            doAccept(ed->fd, true);
            break;
          default:
            doSocket(ed, events[i].events, write_queue, reads);
            break;
        }
      }

      for (auto* ed : write_queue) {
        if (!sockets_.count(ed) || !ed->to_write) {
          continue;
        }
        doWrite(ed);
      }
      write_queue.clear();
      if (cfg_.print_rx_stats) {
        rx_stats.doneLoop(bytesRx_, requestsRx_, reads);
      }
    }

    vlog("epollrunner: done socks=", socks());
  }

  Config const cfg_;
  EpollRxConfig const rxCfg_;
  int epoll_fd;
  std::vector<struct epoll_event> events;
  std::vector<char> rcvbuff;
  std::vector<std::unique_ptr<EPollData>> listeners_;
  std::unordered_set<EPollData*> sockets_;
  struct msghdr recvmsgHdr_;
  struct iovec recvmsgHdrIoVec_;
};

uint16_t pickPort(Config const& config) {
  static uint16_t startPort =
      config.use_port.size() ? config.use_port[0] : 10000 + rand() % 2000;
  bool v6 = config.send_options.ipv6;
  if (config.use_port.size()) {
    return startPort++;
  }
  for (int i = 0; i < 1000; i++) {
    auto port = startPort++;
    if (v6) {
      int v6 = mkBoundSock(port, true);
      if (v6 < 0) {
        continue;
      }
      close(v6);
    } else {
      int v4 = mkBoundSock(port, false);
      if (v4 < 0) {
        continue;
      }
      close(v4);
    }
    return port;
  }
  die("no port found");
  return 0;
}

void run(std::unique_ptr<RunnerBase> runner, std::atomic<bool>* shutdown) {
  try {
    runner->start();
    runner->loop(shutdown);
  } catch (InterruptedException const&) {
    vlog("interrupted, cleaning up nicely");
    runner->stop();
    vlog("waiting until done:");
    runner->loop(shutdown);
    vlog("done");
  } catch (std::exception const& ex) {
    log("caught exception, terminating: ", ex.what());
    throw;
  }
}

struct Receiver {
  std::unique_ptr<RunnerBase> r;
  uint16_t port;
  std::string name;
  std::string rxCfg;
};

Receiver makeEpollRx(Config const& cfg, EpollRxConfig const& rx_cfg) {
  uint16_t port = pickPort(cfg);
  auto runner =
      std::make_unique<EPollRunner>(cfg, rx_cfg, strcat("epoll port=", port));
  runner->addListenSock(
      mkServerSock(rx_cfg, port, cfg.send_options.ipv6, SOCK_NONBLOCK),
      cfg.send_options.ipv6);
  return Receiver{std::move(runner), port, "epoll", rx_cfg.describe()};
}

template <size_t flags>
struct BasicSockPicker {
  // if using buffer provider, don't need any buffer
  // except for to do sending
  using Sock = std::conditional_t<
      flags & kUseBufferProviderFlag,
      BasicSock<64, flags>,
      BasicSock<4096, flags>>;
};

template <size_t MbFlag>
void mbIoUringRxFactory(
    Config const& cfg,
    IoUringRxConfig const& rx_cfg,
    std::string const& name,
    size_t flags,
    std::unique_ptr<RunnerBase>& runner) {
  if (flags == MbFlag) {
    if (runner) {
      die("already had a runner? flags=", flags, " this=", MbFlag);
    }
    auto [ring, new_cfg] = mkIoUring(rx_cfg);
    runner =
        std::make_unique<IOUringRunner<typename BasicSockPicker<MbFlag>::Sock>>(
            cfg, new_cfg, ring, name);
  }
}

template <size_t... PossibleFlag>
Receiver makeIoUringRx(
    Config const& cfg,
    IoUringRxConfig const& rx_cfg,
    std::index_sequence<PossibleFlag...>) {
  uint16_t port = pickPort(cfg);

  std::unique_ptr<RunnerBase> runner;
  size_t flags = (rx_cfg.provide_buffers == 1 ? kUseBufferProviderFlag : 0) |
      (rx_cfg.provide_buffers == 2 ? kUseBufferProviderV2Flag : 0);

  ((mbIoUringRxFactory<PossibleFlag>(
       cfg, rx_cfg, strcat("io_uring port=", port), flags, runner)),
   ...);

  if (!runner) {
    die("no factory for runner flags=",
        flags,
        " maybe you need to increase the index sequence "
        "size in the caller of this");
  }

  // io_uring doesnt seem to like accepting on a nonblocking socket
  int sock_flags = rx_cfg.supports_nonblock_accept ? SOCK_NONBLOCK : 0;

  runner->addListenSock(
      mkServerSock(rx_cfg, port, cfg.send_options.ipv6, sock_flags),
      cfg.send_options.ipv6);

  return Receiver{std::move(runner), port, "io_uring", rx_cfg.describe()};
}

Config parse(int argc, char** argv) {
  Config config;
  po::options_description desc;
  int runs = 1;
  // clang-format off
desc.add_options()
("help", "produce help message")
("verbose", "verbose logging")
("print_rx_stats", po::value(&config.print_rx_stats)
         ->default_value(config.print_rx_stats))
("print_read_stats", po::value(&config.print_read_stats)
        ->default_value(config.print_read_stats))
("use_port", po::value<std::vector<uint16_t>>(&config.use_port)->multitoken(),
 "what target port")
("control_port", po::value(&config.control_port))
("server_only", po::value(&config.server_only),
 "do not tx locally, wait for it")
("client_only", po::value(&config.client_only),
 "do not rx locally, only send requests")
("runs", po::value(&runs)->default_value(runs),
  "how many times to run the test")
("host", po::value(&config.send_options.host))
("v6", po::value(&config.send_options.ipv6))
("time", po::value(&config.send_options.run_seconds))
("tx", po::value<std::vector<std::string> >()->multitoken(),
 "tx scenarios to run (can be multiple)")
("rx", po::value<std::vector<std::string> >()->multitoken(),
 "rx engines to run (can be multiple)")
;
  // clang-format on

  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);
  if (vm.count("help")) {
    std::cerr << desc << "\n";
    std::cerr << "tx options are:\n";
    for (auto tx : allScenarios()) {
      std::cerr << "    " << tx << "\n";
    }
    std::cerr << "rx engines are: epoll, io_uring\n";
    exit(1);
  }
  if (vm.count("verbose")) {
    setVerbose();
  }
  if (vm.count("tx")) {
    for (auto const& tx : vm["tx"].as<std::vector<std::string>>()) {
      if (tx == "all") {
        auto all = allScenarios();
        config.tx.insert(config.tx.end(), all.begin(), all.end());
      } else {
        // quick exit for --help
        PerSendOptions::parseOptions(tx);

        config.tx.push_back(tx);
      }
    }
  } else {
    config.tx.push_back("epoll");
  }

  if (vm.count("rx")) {
    for (auto const& rx : vm["rx"].as<std::vector<std::string>>()) {
      if (rx.empty()) {
        continue;
      }
      config.rx.push_back(rx);
    }
  } else {
    config.rx.push_back("io_uring");
    config.rx.push_back("epoll");
  }

  if (config.server_only) {
    config.tx.clear();
  }

  if (config.client_only) {
    config.rx.clear();
  }

  if (runs <= 0) {
    die("bad runs");
  } else if (runs > 1) {
    auto const rx = config.rx;
    auto const tx = config.tx;
    for (int i = 1; i < runs; i++) {
      config.rx.insert(config.rx.end(), rx.begin(), rx.end());
      config.tx.insert(config.tx.end(), tx.begin(), tx.end());
    }
  }

  // validations:

  if (config.server_only && config.client_only) {
    die("only one of server/client only please");
  }

  return config;
}

std::pair<RxEngine, std::vector<std::string>> getRxEngine(
    std::string const& parse) {
  auto split = po::split_unix(parse);
  if (split.size() < 1) {
    die("no engine in ", parse);
  }
  auto e = split[0];
  // don't erase the first one, boost skips it expecting an executable
  if (e == "epoll") {
    return std::make_pair(RxEngine::Epoll, split);
  } else if (e == "io_uring") {
    return std::make_pair(RxEngine::IoUring, split);
  } else {
    die("bad rx engine ", e);
  }
  throw std::logic_error("should not get here");
}

std::function<Receiver(Config const&)> parseRx(std::string const& parse) {
  IoUringRxConfig io_uring_cfg;
  EpollRxConfig epoll_cfg;
  po::options_description epoll_desc;
  po::options_description io_uring_desc;

  // clang-format off
auto add_base = [&](po::options_description& d, RxConfig& cfg) {
  d.add_options()
("backlog", po::value(&cfg.backlog)->default_value(cfg.backlog))
("max_events", po::value(&cfg.max_events)->default_value(cfg.max_events))
("recv_size", po::value(&cfg.recv_size)->default_value(cfg.recv_size))
("recvmsg",  po::value(&cfg.recvmsg)->default_value(cfg.recvmsg))
("workload",  po::value(&cfg.workload)->default_value(cfg.workload))
("description",  po::value(&cfg.description))
  ;
};

add_base(epoll_desc, epoll_cfg);
add_base(io_uring_desc, io_uring_cfg);

io_uring_desc.add_options()
  ("provide_buffers",  po::value(&io_uring_cfg.provide_buffers)
     ->default_value(io_uring_cfg.provide_buffers))
  ("fixed_files",  po::value(&io_uring_cfg.fixed_files)
     ->default_value(io_uring_cfg.fixed_files))
  ("max_cqe_loop",  po::value(&io_uring_cfg.max_cqe_loop)
     ->default_value(io_uring_cfg.max_cqe_loop))
  ("huge_pages",  po::value(&io_uring_cfg.huge_pages)
     ->default_value(io_uring_cfg.huge_pages))
  ("multishot_recv",  po::value(&io_uring_cfg.multishot_recv)
     ->default_value(io_uring_cfg.multishot_recv))
  ("supports_nonblock_accept",  po::value(&io_uring_cfg.supports_nonblock_accept)
     ->default_value(io_uring_cfg.supports_nonblock_accept))
  ("register_ring",  po::value(&io_uring_cfg.register_ring)
     ->default_value(io_uring_cfg.register_ring))
  ("sqe_count", po::value(&io_uring_cfg.sqe_count)
     ->default_value(io_uring_cfg.sqe_count))
  ("cqe_count", po::value(&io_uring_cfg.cqe_count)
     ->default_value(io_uring_cfg.cqe_count))
  ("provided_buffer_count", po::value(&io_uring_cfg.provided_buffer_count)
     ->default_value(io_uring_cfg.provided_buffer_count))
  ("fixed_file_count", po::value(&io_uring_cfg.fixed_file_count)
     ->default_value(io_uring_cfg.fixed_file_count))
  ("provided_buffer_low_watermark", po::value(&io_uring_cfg.provided_buffer_low_watermark)
     ->default_value(io_uring_cfg.provided_buffer_low_watermark))
  ("provided_buffer_compact", po::value(&io_uring_cfg.provided_buffer_compact)
     ->default_value(io_uring_cfg.provided_buffer_compact))
  ("defer_taskrun", po::value(&io_uring_cfg.defer_taskrun)
     ->default_value(io_uring_cfg.defer_taskrun))
  ;

epoll_desc.add_options()
  ("batch_send",  po::value(&epoll_cfg.batch_send)
     ->default_value(epoll_cfg.batch_send))
  ;

  // clang-format on

  po::options_description* used_desc = NULL;
  auto const [engine, splits] = getRxEngine(parse);
  switch (engine) {
    case RxEngine::IoUring:
      used_desc = &io_uring_desc;
      break;
    case RxEngine::Epoll:
      used_desc = &epoll_desc;
      break;
  };

  simpleParse(*used_desc, splits);

  if (io_uring_cfg.provided_buffer_low_watermark < 0) {
    // default to quarter unless explicitly told
    io_uring_cfg.provided_buffer_low_watermark =
        io_uring_cfg.provided_buffer_count / 4;
  }

  switch (engine) {
    case RxEngine::IoUring:
      return [io_uring_cfg](Config const& cfg) -> Receiver {
        return makeIoUringRx(cfg, io_uring_cfg, std::make_index_sequence<4>{});
      };
    case RxEngine::Epoll:
      return [epoll_cfg](Config const& cfg) -> Receiver {
        return makeEpollRx(cfg, epoll_cfg);
      };
      break;
  };
  die("bad engine ", (int)engine);
  return {};
}

template <class T>
struct SimpleAggregate {
  explicit SimpleAggregate(std::vector<T>&& vals) {
    std::sort(vals.begin(), vals.end());
    avg = std::accumulate(vals.begin(), vals.end(), T(0)) / vals.size();
    p50 = vals[vals.size() / 2];
    p100 = vals.back();
  }

  template <class Formatter>
  std::string toString(Formatter f) const {
    return strcat("p50=", f(p50), " avg=", f(avg), " p100=", f(p100));
  }
  T avg;
  T p50;
  T p100;
};

struct AggregateResults {
  AggregateResults(SimpleAggregate<double> pps, SimpleAggregate<double> bps)
      : packetsPerSecond(std::move(pps)), bytesPerSecond(std::move(bps)) {}

  std::string toString() const {
    return strcat(
        "packetsPerSecond={",
        packetsPerSecond.toString(
            [](double x) { return strcat(x / 1000, "k"); }),
        "} bytesPerSecond={",
        bytesPerSecond.toString(
            [](double x) { return strcat(x / 1000000, "M"); }),
        "}");
  }

  SimpleAggregate<double> packetsPerSecond;
  SimpleAggregate<double> bytesPerSecond;
};

AggregateResults aggregateResults(std::vector<SendResults> const& results) {
  std::vector<double> pps;
  std::vector<double> bps;
  for (auto const& r : results) {
    pps.push_back(r.packetsPerSecond);
    bps.push_back(r.bytesPerSecond);
  }
  return AggregateResults{
      SimpleAggregate<double>{std::move(pps)},
      SimpleAggregate<double>{std::move(bps)}};
}

int main(int argc, char** argv) {
  Config const cfg = parse(argc, argv);
  signal(SIGINT, intHandler);
  std::vector<std::function<Receiver()>> receiver_factories;
  std::unique_ptr<IControlServer> control_server;
  for (auto const& rx : cfg.rx) {
    receiver_factories.push_back(
        [&cfg, parsed = parseRx(rx)]() -> Receiver { return parsed(cfg); });
  }

  if (cfg.client_only) {
    std::unordered_map<uint16_t, std::string> port_name_map;
    std::vector<uint16_t> used_ports = cfg.use_port;
    if (cfg.control_port) {
      port_name_map = getPortNameMap(
          cfg.send_options.host, cfg.control_port, cfg.send_options.ipv6);
      if (used_ports.empty()) {
        log("taking all ports from server");
        for (auto const& kv : port_name_map) {
          used_ports.push_back(kv.first);
        }
        std::sort(used_ports.begin(), used_ports.end());
      }
    }
    if (used_ports.empty()) {
      die("please specify port for client_only");
    }
    receiver_factories.clear();
    log("using given ports not setting up local receivers");
    for (auto port : used_ports) {
      receiver_factories.push_back([port, port_name_map]() -> Receiver {
        auto it = port_name_map.find(port);
        std::string name;
        if (it == port_name_map.end()) {
          name = strcat("given_port port=", port);
        } else {
          name = it->second;
        }
        return Receiver{
            std::make_unique<NullRunner>(strcat("null port=", port)),
            port,
            std::move(name)};
      });
    }
  }

  std::vector<std::pair<std::string, SendResults>> results;
  if (cfg.tx.size()) {
    for (auto const& tx : cfg.tx) {
      for (auto const& r : receiver_factories) {
        Receiver rcv = r();
        std::atomic<bool> should_shutdown{false};
        log("running ", tx, " for ", rcv.name, " cfg=", rcv.rxCfg);

        std::thread rcv_thread(wrapThread(
            strcat("rcv", rcv.name),
            [r = std::move(rcv.r), shutdown = &should_shutdown]() mutable {
              run(std::move(r), shutdown);
            }));

        auto res = runSender(tx, cfg.send_options, rcv.port);
        should_shutdown = true;
        log("...done sender");
        rcv_thread.join();
        log("...done receiver");
        results.emplace_back(
            strcat("tx:", tx, " rx:", rcv.name, " ", rcv.rxCfg),
            std::move(res));
      }
    }

    for (auto& r : results) {
      log(r.first);
      log(std::string(30, ' '), r.second.toString());
    }

    // build up to_agg but do it in insertion order of results
    // hence the nasty but probably not a big deal std::find_if
    std::vector<std::pair<std::string, std::vector<SendResults>>> to_agg;
    for (auto& r : results) {
      auto it = std::find_if(to_agg.begin(), to_agg.end(), [&](auto const& x) {
        return x.first == r.first;
      });
      if (it == to_agg.end()) {
        to_agg
            .emplace_back(
                std::piecewise_construct,
                std::make_tuple(r.first),
                std::make_tuple())
            .second.push_back(std::move(r.second));
      } else {
        it->second.push_back(std::move(r.second));
      }
    }

    for (auto& kv : to_agg) {
      if (kv.second.size() <= 1) {
        continue;
      }
      log("aggregated:  ", kv.first);
      log(std::string(30, ' '),
          aggregateResults(std::move(kv.second)).toString());
    }

  } else {
    // no built in sender mode
    std::atomic<bool> should_shutdown{false};
    std::vector<Receiver> receivers;
    std::vector<std::thread> receiver_threads;
    std::unordered_map<uint16_t, std::string> server_port_name_map;
    for (auto& r : receiver_factories) {
      receivers.push_back(r());
    }
    log("using receivers: ");
    for (auto const& r : receivers) {
      log(r.name, " port=", r.port, " rx_cfg=", r.rxCfg);
      server_port_name_map[r.port] = strcat(r.name, " ", r.rxCfg);
    }

    if (cfg.control_port) {
      control_server = makeControlServer(
          server_port_name_map, cfg.control_port, cfg.send_options.ipv6);
    }

    for (auto& r : receivers) {
      receiver_threads.emplace_back(wrapThread(
          strcat("rcv", r.name),
          [r = std::move(r.r), shutdown = &should_shutdown]() mutable {
            run(std::move(r), shutdown);
          }));
    }

    for (size_t i = 0; i < receivers.size(); ++i) {
      vlog("waiting for ", receivers[i].name);
      receiver_threads[i].join();
    }
  }

  vlog("all done");
  return 0;
}
