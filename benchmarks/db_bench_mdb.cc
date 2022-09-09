#include <lmdb.h>
#include <stdio.h>
#include <stdlib.h>

#include "port/port.h"
#include "util/histogram.h"
#include "util/mutexlock.h"
#include "util/random.h"
#include "util/testutil.h"

#define CHECK_MDB(stmt)                                     \
  do {                                                      \
    int ec = (stmt);                                        \
    if (ec != MDB_SUCCESS)                                  \
      fprintf(stderr, "mdb error: %s\n", mdb_strerror(ec)); \
  } while (0)

// Comma-separated list of operations to run in the specified order
//   Actual benchmarks:
//
//   fillseq       -- write N values in sequential key order in async mode
//   fillrandom    -- write N values in random key order in async mode
//   fillrandint   -- write N values in random binary key order in async mode
//   overwrite     -- overwrite N values in random key order in async mode
//   fillseqsync   -- write N/100 values in sequential key order in sync mode
//   fillseqbatch  -- batch write N values in sequential key order in async mode
//   fillrandsync  -- write N/100 values in random key order in sync mode
//   fillrandbatch  -- batch write N values in random key order in async mode
//   fillrandibatch  -- batch write N values in random binary key order in async
//   mode fillrand100K  -- write N/1000 100K values in random order in async
//   mode fillseq100K   -- write N/1000 100K values in seq order in async mode
//   readseq       -- read N times sequentially
//   readreverse   -- read N times in reverse order
//   readseq100K   -- read N/1000 100K values in sequential order in async mode
//   readrand100K  -- read N/1000 100K values in sequential order in async mode
//   readrandom    -- read N times in random order
static const char* FLAGS_benchmarks =
    "fillseqsync,"
    "fillrandsync,"
    "fillseq,"
    "fillseqbatch,"
    "fillrandom,"
    "fillrandbatch,"
    "overwrite,"
#if 0
    "overwritebatch,"
#endif
    "readrandom,"
    "readseq,"
    "readreverse,"
#if 0
    "fillrand100K,"
    "fillseq100K,"
    "readseq100K,"
    "readrand100K,"
#endif
    ;

// Number of key/values to place in database
static int FLAGS_num = 1000000;

// Number of read operations to do.  If negative, do FLAGS_num reads.
static int FLAGS_reads = -1;

// Number of concurrent threads to run.
static int FLAGS_threads = 1;

// Size of each value
static int FLAGS_value_size = 100;

// Number of key/values to place in database
static int FLAGS_batch = 1000;

// Arrange to generate values that shrink to this fraction of
// their original size after compression
static double FLAGS_compression_ratio = 0.5;

// Print histogram of operation timings
static bool FLAGS_histogram = false;

// Page size. Default 1 KB
static int FLAGS_page_size = 1024;

// If true, do not destroy the existing database.  If you set this
// flag and also specify a benchmark that wants a fresh database, that
// benchmark will fail.
static bool FLAGS_use_existing_db = false;

// If true, we allow batch writes to occur
static bool FLAGS_transaction = true;

// If false, skip sync of meta pages on synchronous writes
static bool FLAGS_metasync = true;

// If true, use writable mmap
static bool FLAGS_writemap = true;

// Use the db with the following name.
static const char* FLAGS_db = NULL;

static int* shuff = NULL;

// Common key prefix length.
static int FLAGS_key_prefix = 0;

namespace leveldb {
namespace {

leveldb::Env* g_env = nullptr;

class RandomGenerator {
 private:
  std::string data_;
  int pos_;

 public:
  RandomGenerator() {
    Random rnd(301);
    std::string piece;
    while (data_.size() < 1048576) {
      test::CompressibleString(&rnd, FLAGS_compression_ratio, 100, &piece);
      data_.append(piece);
    }
    pos_ = 0;
  }

  Slice Generate(size_t len) {
    if (pos_ + len > data_.size()) {
      pos_ = 0;
      assert(len < data_.size());
    }
    pos_ += len;
    return Slice(data_.data() + pos_ - len, len);
  }
};

class KeyBuffer {
 public:
  KeyBuffer() {
    assert(FLAGS_key_prefix < sizeof(buffer_));
    memset(buffer_, 'a', FLAGS_key_prefix);
  }

  KeyBuffer& operator=(KeyBuffer& other) = delete;
  KeyBuffer(KeyBuffer& other) = delete;

  void Set(int k) {
    std::snprintf(buffer_ + FLAGS_key_prefix,
                  sizeof(buffer_) - FLAGS_key_prefix, "%016d", k);
  }

  Slice slice() const { return Slice(buffer_, FLAGS_key_prefix + 16); }

 private:
  char buffer_[1024];
};

#if defined(__linux)
static Slice TrimSpace(Slice s) {
  size_t start = 0;
  while (start < s.size() && isspace(s[start])) {
    start++;
  }
  size_t limit = s.size();
  while (limit > start && isspace(s[limit - 1])) {
    limit--;
  }
  return Slice(s.data() + start, limit - start);
}
#endif

static void AppendWithSpace(std::string* str, Slice msg) {
  if (msg.empty()) return;
  if (!str->empty()) {
    str->push_back(' ');
  }
  str->append(msg.data(), msg.size());
}

class Stats {
 private:
  double start_;
  double finish_;
  double seconds_;
  int done_;
  int next_report_;
  int64_t bytes_;
  double last_op_finish_;
  Histogram hist_;
  std::string message_;

 public:
  Stats() { Start(); }

  void Start() {
    next_report_ = 100;
    hist_.Clear();
    done_ = 0;
    bytes_ = 0;
    seconds_ = 0;
    message_.clear();
    start_ = finish_ = last_op_finish_ = g_env->NowMicros();
  }

  void Merge(const Stats& other) {
    hist_.Merge(other.hist_);
    done_ += other.done_;
    bytes_ += other.bytes_;
    seconds_ += other.seconds_;
    if (other.start_ < start_) start_ = other.start_;
    if (other.finish_ > finish_) finish_ = other.finish_;

    // Just keep the messsages from one thread
    if (message_.empty()) message_ = other.message_;
  }

  void Stop() {
    finish_ = g_env->NowMicros();
    seconds_ = (finish_ - start_) * 1e-6;
  }

  void AddMessage(Slice msg) { AppendWithSpace(&message_, msg); }

  void FinishedSingleOp() {
    if (FLAGS_histogram) {
      double now = g_env->NowMicros();
      double micros = now - last_op_finish_;
      hist_.Add(micros);
      if (micros > 20000) {
        std::fprintf(stderr, "long op %.1f micros %30s\r", micros, "");
        std::fflush(stderr);
      }
      last_op_finish_ = now;
    }

    done_++;
    if (done_ >= next_report_) {
      if (next_report_ < 1000)
        next_report_ += 100;
      else if (next_report_ < 5000)
        next_report_ += 500;
      else if (next_report_ < 10000)
        next_report_ += 1000;
      else if (next_report_ < 50000)
        next_report_ += 5000;
      else if (next_report_ < 100000)
        next_report_ += 10000;
      else if (next_report_ < 500000)
        next_report_ += 50000;
      else
        next_report_ += 100000;
      std::fprintf(stderr, "... finished %d ops%30s\r", done_, "");
      std::fflush(stderr);
    }
  }

  void AddBytes(int64_t n) { bytes_ += n; }

  void Report(const Slice& name) {
    // Pretend at lease one op was done in case we are running a benchmark
    // that does not call FinishSingleOp().
    if (done_ < 1) done_ = 1;

    std::string extra;
    if (bytes_ > 0) {
      // Rate is computed on actual elapsed time, not the sum of per-thread
      // elapsed times.
      double elapsed = (finish_ - start_) * 1e-6;
      char rate[100];
      std::snprintf(rate, sizeof(rate), "%6.1f MB/s",
                    (bytes_ / 1048576.0) / elapsed);
      extra = rate;
    }
    AppendWithSpace(&extra, message_);

    std::fprintf(stdout, "%-12s : %11.3f micros/op;%s%s\n",
                 name.ToString().c_str(), seconds_ * 1e6 / done_,
                 (extra.empty() ? "" : " "), extra.c_str());
    if (FLAGS_histogram) {
      std::fprintf(stdout, "Microseconds per op:\n%s\n",
                   hist_.ToString().c_str());
    }
    std::fflush(stdout);
  }
};

// State shared by all concurrent executions of the same enchmark
struct SharedState {
  port::Mutex mu;
  port::CondVar cv GUARDED_BY(mu);
  int total GUARDED_BY(mu);

  // Each thread goes through the following states:
  //   (1) initializing
  //   (2) waiting for others to be initialized
  //   (3) running
  //   (4) done

  int num_initialized GUARDED_BY(mu);
  int num_done GUARDED_BY(mu);
  bool start GUARDED_BY(mu);

  SharedState(int total)
      : cv(&mu), total(total), num_initialized(0), num_done(0), start(false) {}
};

struct ThreadState {
  int tid;      // 0..n-1 when running n thread
  Random rand;  // Has different seeds for different threads
  Stats stats;
  SharedState* shared;

  ThreadState(int index, int seed) : tid(index), rand(seed), shared(nullptr) {}
};

}  // namespace

class Benchmark {
 private:
  MDB_env* db_;
  MDB_dbi dbi_;
  int db_num_;
  int num_;
  int value_size_;
  int entries_per_batch_;
  int reads_;
  int heap_counter_;
  int total_thread_count_;

  void PrintHeader() {
    const int kKeySize = 16 + FLAGS_key_prefix;
    PrintEnvironment();
    std::fprintf(stdout, "Keys:       %d bytes each\n", kKeySize);
    std::fprintf(
        stdout, "Values:     %d bytes each (%d bytes after compression)\n",
        FLAGS_value_size,
        static_cast<int>(FLAGS_value_size * FLAGS_compression_ratio + 0.5));
    std::fprintf(stdout, "Entries:    %d\n", num_);
    std::fprintf(stdout, "RawSize:    %.1f MB (estimated)\n",
                 ((static_cast<int64_t>(kKeySize + FLAGS_value_size) * num_) /
                  1048576.0));
    std::fprintf(
        stdout, "FileSize:   %.1f MB (estimated)\n",
        (((kKeySize + FLAGS_value_size * FLAGS_compression_ratio) * num_) /
         1048576.0));
    PrintWarnings();
    std::fprintf(stdout, "------------------------------------------------\n");
  }

  void PrintWarnings() {
#if defined(__GNUC__) && !defined(__OPTIMIZE__)
    std::fprintf(
        stdout,
        "WARNING: Optimization is disabled: benchmarks unnecessarily slow\n");
#endif
#ifndef NDEBUG
    std::fprintf(
        stdout,
        "WARNING: Assertions are enabled; benchmarks unnecessarily slow\n");
#endif

    // See if snappy is working by attempting to compress a compressible string
    const char text[] = "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy";
    std::string compressed;
    if (!port::Snappy_Compress(text, sizeof(text), &compressed)) {
      std::fprintf(stdout, "WARNING: Snappy compression is not enabled\n");
    } else if (compressed.size() >= sizeof(text)) {
      std::fprintf(stdout, "WARNING: Snappy compression is not effective\n");
    }
  }

  void PrintEnvironment() {
    fprintf(stderr, "MDB:    version %s\n", mdb_version(NULL, NULL, NULL));

#if defined(__linux)
    time_t now = time(NULL);
    fprintf(stderr, "Date:           %s", ctime(&now));  // ctime() adds newline

    FILE* cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo != NULL) {
      char line[1000];
      int num_cpus = 0;
      std::string cpu_type;
      std::string cache_size;
      while (fgets(line, sizeof(line), cpuinfo) != NULL) {
        const char* sep = strchr(line, ':');
        if (sep == NULL) {
          continue;
        }
        Slice key = TrimSpace(Slice(line, sep - 1 - line));
        Slice val = TrimSpace(Slice(sep + 1));
        if (key == "model name") {
          ++num_cpus;
          cpu_type = val.ToString();
        } else if (key == "cache size") {
          cache_size = val.ToString();
        }
      }
      fclose(cpuinfo);
      fprintf(stderr, "CPU:            %d * %s\n", num_cpus, cpu_type.c_str());
      fprintf(stderr, "CPUCache:       %s\n", cache_size.c_str());
    }
#endif
  }

 public:
  Benchmark()
      : db_(NULL),
        db_num_(0),
        num_(FLAGS_num),
        value_size_(FLAGS_value_size),
        entries_per_batch_(1),
        reads_(FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads),
        heap_counter_(0),
        total_thread_count_(0) {
    std::vector<std::string> files;
    std::string test_dir;
    Env::Default()->GetTestDirectory(&test_dir);
    Env::Default()->GetChildren(test_dir.c_str(), &files);
    if (!FLAGS_use_existing_db) {
      for (int i = 0; i < files.size(); ++i) {
        if (Slice(files[i]).starts_with("dbbench_mdb")) {
          std::string file_name(test_dir);
          file_name += "/";
          file_name += files[i];
          Env::Default()->DeleteFile(file_name.c_str());
        }
      }
    }
  }

  ~Benchmark() { mdb_env_close(db_); }

  void Run() {
    PrintHeader();
    Open(DBFlags::NONE);

    const char* benchmarks = FLAGS_benchmarks;
    while (benchmarks != nullptr) {
      const char* sep = strchr(benchmarks, ',');
      Slice name;
      if (sep == nullptr) {
        name = benchmarks;
        benchmarks = nullptr;
      } else {
        name = Slice(benchmarks, sep - benchmarks);
        benchmarks = sep + 1;
      }

      // Reset parameter that may be overriden below
      num_ = FLAGS_num;
      reads_ = (FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads);
      value_size_ = FLAGS_value_size;
      entries_per_batch_ = 1;

      void (Benchmark::*method)(ThreadState*) = nullptr;
      bool fresh_db = false;
      int num_threads = FLAGS_threads;

      if (name == Slice("fillseq")) {
        fresh_db = true;
        method = &Benchmark::WriteSeq;
      } else if (name == Slice("fillrandom")) {
        fresh_db = true;
        method = &Benchmark::WriteRandom;
      } else if (name == Slice("readseq")) {
        method = &Benchmark::ReadSequential;
      } else if (name == Slice("readrandom")) {
        method = &Benchmark::ReadRandom;
      } else {
        if (!name.empty()) {
          // no error message for empty name
          std::fprintf(stderr, "unknown benchmark '%s'\n",
                       name.ToString().c_str());
        }
      }

      if (fresh_db) {
        if (FLAGS_use_existing_db) {
          std::fprintf(stdout, "%-12s : skipped(--use_existing_db is true)\n",
                       name.ToString().c_str());
          method = nullptr;
        } else {
          // TODO(gaopin): desotry db and reopen.
          if (db_) {
            char cmd[200];
            sprintf(cmd, "rm -rf %s*", FLAGS_db);
            mdb_env_close(db_);
            system(cmd);
            db_ = NULL;
          }
          Open(DBFlags::NONE);
        }
      }

      if (method != nullptr) {
        RunBenchmark(num_threads, name, method);
      }
    }
  }

 private:
  enum DBFlags { NONE = 0, SYNC, INT };

  struct ThreadArg {
    Benchmark* bm;
    SharedState* shared;
    ThreadState* thread;
    void (Benchmark::*method)(ThreadState*);
  };

  static void ThreadBody(void* v) {
    ThreadArg* arg = reinterpret_cast<ThreadArg*>(v);
    SharedState* shared = arg->shared;
    ThreadState* thread = arg->thread;
    {
      MutexLock l(&shared->mu);
      shared->num_initialized++;
      if (shared->num_initialized >= shared->total) {
        shared->cv.SignalAll();
      }
      while (!shared->start) {
        shared->cv.Wait();
      }
    }

    thread->stats.Start();
    (arg->bm->*(arg->method))(thread);
    thread->stats.Stop();

    {
      MutexLock l(&shared->mu);
      shared->num_done++;
      if (shared->num_done >= shared->total) {
        shared->cv.SignalAll();
      }
    }
  }

  void RunBenchmark(int n, Slice name,
                    void (Benchmark::*method)(ThreadState*)) {
    SharedState shared(n);

    ThreadArg* arg = new ThreadArg[n];
    for (int i = 0; i < n; ++i) {
      arg[i].bm = this;
      arg[i].method = method;
      arg[i].shared = &shared;
      ++total_thread_count_;

      arg[i].thread = new ThreadState(i, 1000 + total_thread_count_);
      arg[i].thread->shared = &shared;
      g_env->StartThread(ThreadBody, &arg[i]);
    }

    shared.mu.Lock();
    while (shared.num_initialized < n) {
      shared.cv.Wait();
    }

    shared.start = true;
    shared.cv.SignalAll();
    while (shared.num_done < n) {
      shared.cv.Wait();
    }
    shared.mu.Unlock();

    for (int i = 1; i < n; ++i) {
      arg[0].thread->stats.Merge(arg[i].thread->stats);
    }
    arg[0].thread->stats.Report(name);

    for (int i = 0; i < n; ++i) {
      delete arg[i].thread;
    }
    delete[] arg;
  }

  void Open(DBFlags flags) {
    assert(db_ == NULL);
    int rc;
    MDB_txn* txn;

    char file_name[100], cmd[200];
    db_num_++;
    std::string test_dir;
    Env::Default()->GetTestDirectory(&test_dir);
    snprintf(file_name, sizeof(file_name), "%s/dbbench_mdb-%d",
             test_dir.c_str(), db_num_);
    sprintf(cmd, "mkdir -p %s", file_name);
    system(cmd);

    int env_opt = 0;
    if (flags != SYNC)
      env_opt = MDB_NOSYNC;
    else if (!FLAGS_metasync)
      env_opt = MDB_NOMETASYNC;

    if (FLAGS_writemap) env_opt |= MDB_WRITEMAP;

    // Create tuning options and open the database
    CHECK_MDB(mdb_env_create(&db_));
    CHECK_MDB(
        mdb_env_set_mapsize(db_, FLAGS_num * FLAGS_value_size * 32L / 10));
    CHECK_MDB(mdb_env_set_maxreaders(db_, 1024));
    CHECK_MDB(mdb_env_open(db_, file_name, env_opt, 0664));
    CHECK_MDB(mdb_txn_begin(db_, NULL, 0, &txn));
    CHECK_MDB(mdb_open(txn, NULL, flags == INT ? MDB_INTEGERKEY : 0, &dbi_));
    CHECK_MDB(mdb_txn_commit(txn));
  }

  void WriteSeq(ThreadState* thread) { DoWrite(thread, true); }

  void WriteRandom(ThreadState* thread) { DoWrite(thread, false); }

  void DoWrite(ThreadState* thread, bool seq) {
    if (num_ != FLAGS_num) {
      char msg[100];
      std::snprintf(msg, sizeof(msg), "(%d ops)", num_);
      thread->stats.AddMessage(msg);
    }

    MDB_txn* txn;
    RandomGenerator gen;
    int64_t bytes = 0;
    KeyBuffer key;
    int flag = 0;
    if (seq) flag = MDB_APPEND;

    for (int i = 0; i < num_; i += entries_per_batch_) {
      MDB_cursor* mc;
      CHECK_MDB(mdb_txn_begin(db_, NULL, 0, &txn));
      CHECK_MDB(mdb_cursor_open(txn, dbi_, &mc));

      for (int j = 0; j < entries_per_batch_; j++) {
        int rc;
        const int k = seq ? i + j : thread->rand.Uniform(FLAGS_num);
        key.Set(k);
        MDB_val mkey, mval;
        mkey.mv_data =
            const_cast<void*>(static_cast<const void*>(key.slice().data()));
        mkey.mv_size = key.slice().size();
        mval.mv_data = const_cast<void*>(
            static_cast<const void*>(gen.Generate(value_size_).data()));
        mval.mv_size = value_size_;

        rc = mdb_cursor_put(mc, &mkey, &mval, flag);
        if (rc != 0 && rc != MDB_KEYEXIST) {
          fprintf(stderr, "set error: %s\n", mdb_strerror(rc));
        }
        bytes += value_size_ + key.slice().size();
        thread->stats.FinishedSingleOp();
      }

      mdb_cursor_close(mc);
      CHECK_MDB(mdb_txn_commit(txn));
    }
    thread->stats.AddBytes(bytes);
  }

  void ReadSequential(ThreadState* thread) {
    MDB_txn* txn;
    MDB_cursor* cursor;
    MDB_val key, data;

    int64_t bytes = 0;

    CHECK_MDB(mdb_txn_begin(db_, NULL, MDB_RDONLY, &txn));
    CHECK_MDB(mdb_cursor_open(txn, dbi_, &cursor));
    while (mdb_cursor_get(cursor, &key, &data, MDB_NEXT) == 0) {
      bytes += key.mv_size + data.mv_size;
      thread->stats.FinishedSingleOp();
    }

    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);

    thread->stats.AddBytes(bytes);
  }

  void ReadRandom(ThreadState* thread) {
    MDB_txn* txn;
    MDB_cursor* cursor;
    MDB_val mkey, mval;
    KeyBuffer key;
    int found = 0;

    CHECK_MDB(mdb_txn_begin(db_, NULL, MDB_RDONLY, &txn));
    CHECK_MDB(mdb_cursor_open(txn, dbi_, &cursor));
    for (int i = 0; i < reads_; i++) {
      const int k = thread->rand.Uniform(FLAGS_num);
      key.Set(k);
      auto skey = key.slice();
      mkey.mv_data = const_cast<void*>(static_cast<const void*>(skey.data()));
      mkey.mv_size = skey.size();

      int rc = mdb_cursor_get(cursor, &mkey, &mval, MDB_SET);
      if (rc == 0) {
        found++;
      }
      thread->stats.FinishedSingleOp();
    }
    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);

    char msg[100];
    std::snprintf(msg, sizeof(msg), "(%d of %d found)", found, num_);
    thread->stats.AddMessage(msg);
  }

};  // class benchmark

}  // namespace leveldb

int main(int argc, char** argv) {
  std::string default_db_path;
  for (int i = 1; i < argc; ++i) {
    double d;
    int n;
    char junk;
    if (leveldb::Slice(argv[i]).starts_with("--benchmarks=")) {
      FLAGS_benchmarks = argv[i] + strlen("--benchmarks=");
    } else if (sscanf(argv[i], "--compression_ratio=%lf%c", &d, &junk) == 1) {
      FLAGS_compression_ratio = d;
    } else if (sscanf(argv[i], "--histogram=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_histogram = n;

    } else if (sscanf(argv[i], "--use_existing_db=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_use_existing_db = n;

    } else if (sscanf(argv[i], "--num=%d%c", &n, &junk) == 1) {
      FLAGS_num = n;
    } else if (sscanf(argv[i], "--reads=%d%c", &n, &junk) == 1) {
      FLAGS_reads = n;
    } else if (sscanf(argv[i], "--threads=%d%c", &n, &junk) == 1) {
      FLAGS_threads = n;
    } else if (sscanf(argv[i], "--value_size=%d%c", &n, &junk) == 1) {
      FLAGS_value_size = n;
    } else if (sscanf(argv[i], "--key_prefix=%d%c", &n, &junk) == 1) {
      FLAGS_key_prefix = n;
    } else if (strncmp(argv[i], "--db=", 5) == 0) {
      FLAGS_db = argv[i] + 5;
    } else {
      std::fprintf(stderr, "Invalid flag '%s'\n", argv[i]);
      std::exit(1);
    }
  }

  leveldb::g_env = leveldb::Env::Default();

  // Choose a location for the test database if none given with --db=<path>
  if (FLAGS_db == nullptr) {
    leveldb::g_env->GetTestDirectory(&default_db_path);
    default_db_path += "/dbbench";
    FLAGS_db = default_db_path.c_str();
  }

  leveldb::Benchmark benchmark;
  benchmark.Run();
  return 0;
}