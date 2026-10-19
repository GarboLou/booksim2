// Engine lifecycle: several complete
// simulations one after another in one process, on different topologies and
// modes, must give identical results; and the per-flow CSV is streamed, so a
// run keeps no history of completed flows.

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "test_util.hpp"
#include "booksim_engine.hpp"

// AddressSanitizer keeps freed memory resident in its quarantine, so resident
// memory grows under it even without a leak.
#if defined(__SANITIZE_ADDRESS__)
#define TEST_UNDER_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TEST_UNDER_ASAN 1
#endif
#endif

namespace {

typedef std::vector<std::pair<std::string, std::string> > Overrides;

struct Result {
  std::string csv, mp, pk;
  uint64_t time;
  bool drained;
  int received;
  Result() : time(0), drained(false), received(0) {}
};

int g_received = 0;
void OnRecv(booksim::FlowId, void *, uint64_t, void *) { ++g_received; }

Result RunOnce(const char * cfg, const Overrides & ov,
               const std::vector<booksim::FlowDesc> & flows, bool with_csv) {
  Result r;
  std::ostringstream csv, mp, pk;
  g_received = 0;
  {
    QuietCout quiet;
    booksim::Engine * e = booksim::Engine::Create(cfg, ov);
    if(!e) return r;
    e->SetOnRecvComplete(&OnRecv, NULL);
    if(with_csv) e->SetFlowCsv(&csv);
    for(size_t i = 0; i < flows.size(); ++i) e->InjectFlow(flows[i]);
    for(uint64_t i = 0; i < 5000000 && !e->IsIdle(); ++i) e->Step();
    r.time = e->Time();
    r.drained = e->IsIdle() && e->FlowsOutstanding() == 0 &&
                (!e->PcieCreditsEnabled() || e->PcieCreditsIdle());
    e->DumpMultipathStats(mp);
    e->DumpPacketStats(pk);
    if(with_csv) e->SetFlowCsv(NULL);
    delete e;
  }
  r.csv = csv.str();
  r.mp = mp.str();
  r.pk = pk.str();
  r.received = g_received;
  return r;
}

booksim::FlowDesc Flow(int src, int dst, uint64_t bytes) {
  booksim::FlowDesc f;
  f.src = src;
  f.dst = dst;
  f.bytes = bytes;
  f.user = reinterpret_cast<void *>((intptr_t)(src * 100 + dst));
  return f;
}

struct Case {
  const char * cfg;
  Overrides ov;
  std::vector<booksim::FlowDesc> flows;
};

std::vector<Case> Cases() {
  std::vector<Case> cases;
  Case ring;                                  // stock 4-VC config, min routing
  ring.cfg = "runfiles/simai_ring_torus.cfg";
  for(int s = 0; s < 8; ++s) ring.flows.push_back(Flow(s, (s + 3) % 8, 32768));
  cases.push_back(ring);

  Case guard;                                 // VC0 credits, per-bridge, loop-free
  guard.cfg = "tests/topo/guard_mesh4.cfg";
  guard.ov.push_back(std::make_pair("routing_function", "pinned"));
  guard.ov.push_back(std::make_pair("pcinet_cross_domain_paths", "per_bridge"));
  guard.ov.push_back(std::make_pair("pcinet_deadlock_avoidance", "loopfree"));
  guard.ov.push_back(std::make_pair("multipath_granularity", "packet"));
  for(int s = 0; s < 4; ++s) guard.flows.push_back(Flow(s, (s + 2) % 4, 65536));
  cases.push_back(guard);

  Case dual;                                  // weighted NTB + UPI split
  dual.cfg = "tests/topo/dual2C_ntb.cfg";
  dual.ov.push_back(std::make_pair("routing_function", "pinned"));
  dual.ov.push_back(std::make_pair("pcinet_cross_domain_paths", "per_bridge"));
  dual.ov.push_back(std::make_pair("multipath_select", "weighted"));
  for(int s = 0; s < 4; ++s) dual.flows.push_back(Flow(s, s + 4, 262144));
  cases.push_back(dual);
  return cases;
}

long RssKib() {
  std::ifstream in("/proc/self/status");
  std::string line;
  while(std::getline(in, line)) {
    if(line.compare(0, 6, "VmRSS:") == 0) return atol(line.c_str() + 6);
  }
  return -1;
}

// Many short waves of typed flows (Posted, Non-Posted, Completion) in one
// engine: once warmed up, resident memory must stay flat. Retained flow
// records (the CSV is streamed, so none may be kept) or the base class's
// never-issued reply queue
// would each add tens to hundreds of bytes per flow or TLP.
void CheckFlatMemory() {
  std::cout << "-- flat memory over repeated waves" << std::endl;
  Overrides ov;
  ov.push_back(std::make_pair("routing_function", "pinned"));
  ov.push_back(std::make_pair("pcinet_cross_domain_paths", "per_bridge"));
  ov.push_back(std::make_pair("pcinet_deadlock_avoidance", "loopfree"));
  long warm = -1, end = -1;
  {
    QuietCout quiet;
    booksim::Engine * e = booksim::Engine::Create("tests/topo/guard_mesh4.cfg", ov);
    if(!e) { CHECK(false); return; }
    const int kWaves = 8000, kWarm = 500;
    for(int w = 0; w < kWaves; ++w) {
      for(int s = 0; s < 4; ++s) {
        booksim::FlowDesc f = Flow(s, (s + 2) % 4, 65);
        f.pcie_class = 1 + (s + w) % 3;
        e->InjectFlow(f);
      }
      for(int i = 0; i < 100000 && !e->IsIdle(); ++i) e->Step();
      if(w == kWarm) warm = RssKib();
    }
    end = RssKib();
    CHECK(e->IsIdle() && e->FlowsOutstanding() == 0 && e->PcieCreditsIdle());
    delete e;
  }
  std::cout << "   RSS after warm-up " << warm << " KiB, after 8000 waves " << end
            << " KiB" << std::endl;
  CHECK(warm > 0 && end > 0);
#ifdef TEST_UNDER_ASAN
  const char * opts = getenv("ASAN_OPTIONS");
  if(!opts || !strstr(opts, "quarantine_size_mb=0")) {
    std::cout << "   AddressSanitizer build: its quarantine keeps freed memory "
              << "resident, so the bound is not checked; set "
              << "ASAN_OPTIONS=quarantine_size_mb=0 to check it" << std::endl;
    return;
  }
#endif
  CHECK(end - warm < 256);                    // 30,000 flows: < 9 B each
}

int Lines(const std::string & s) {
  int n = 0;
  for(size_t i = 0; i < s.size(); ++i) n += (s[i] == '\n');
  return n;
}

} // namespace

int main() {
  std::vector<Case> cases = Cases();
  std::vector<Result> first(cases.size());
  // Three rounds over all cases: 9 engines in one process, alternating
  // topologies, with and without a CSV stream.
  for(int round = 0; round < 3; ++round) {
    for(size_t c = 0; c < cases.size(); ++c) {
      const bool with_csv = (round != 2);
      std::cout << "-- round " << round << ": " << cases[c].cfg
                << (with_csv ? "" : " (no CSV)") << std::endl;
      Result r = RunOnce(cases[c].cfg, cases[c].ov, cases[c].flows, with_csv);
      CHECK(r.drained);
      CHECK_EQ(r.received, (int)cases[c].flows.size());
      if(round == 0) {
        first[c] = r;
        CHECK_EQ(Lines(r.csv), (int)cases[c].flows.size() + 1);   // header + rows
        CHECK(r.csv.compare(0, 5, "flow,") == 0);
        continue;
      }
      CHECK_EQ(r.time, first[c].time);
      CHECK(r.mp == first[c].mp);
      CHECK(r.pk == first[c].pk);
      if(with_csv) CHECK(r.csv == first[c].csv);
      else CHECK(r.csv.empty());
    }
  }
  CheckFlatMemory();
  return TestSummary("test_engine_lifecycle");
}
