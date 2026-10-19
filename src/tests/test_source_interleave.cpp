// source_interleave: with tlp a source puts one TLP from each of its active
// flows on its link in turn, as a GPU's concurrent NCCL channels do; with flow
// it sends one flow at a time in issue order. Observed through each flow's
// "last byte left the source" time. On runfiles/simai_flat.cfg a full TLP
// (256 B + 24 B overhead) is 5 flits and the GPU link moves one flit per
// cycle, so one TLP is 5 cycles on the wire.

#include <cstdlib>
#include <map>
#include <sstream>

#include "test_util.hpp"
#include "booksim_engine.hpp"

namespace {

typedef std::vector<std::pair<std::string, std::string> > Overrides;

const long long kTlp = 5;          // cycles per full TLP on a Gen5 x16 link
const long long kTlpsPer64K = 256; // 64 KiB / 256 B

std::map<booksim::FlowId, uint64_t> g_sent, g_done;
void OnSend(booksim::FlowId id, void *, uint64_t t, void *) { g_sent[id] = t; }
void OnRecv(booksim::FlowId id, void *, uint64_t t, void *) { g_done[id] = t; }

struct Flow {
  int src, dst;
  uint64_t bytes;
  uint64_t at;   // cycle at which it is injected
};

struct Run {
  std::vector<long long> sent, done;
  bool drained;
  Run() : drained(false) {}
};

Run Simulate(const char * cfg, const Overrides & ov, const std::vector<Flow> & flows) {
  Run r;
  g_sent.clear();
  g_done.clear();
  std::vector<booksim::FlowId> ids(flows.size(), booksim::kInvalidFlow);
  {
    QuietCout quiet;
    booksim::Engine * e = booksim::Engine::Create(cfg, ov);
    if(!e) return r;
    e->SetOnSendComplete(&OnSend, NULL);
    e->SetOnRecvComplete(&OnRecv, NULL);
    size_t injected = 0;
    for(uint64_t i = 0; i < 2000000; ++i) {
      for(size_t f = 0; f < flows.size(); ++f) {
        if(ids[f] == booksim::kInvalidFlow && flows[f].at == e->Time()) {
          booksim::FlowDesc d;
          d.src = flows[f].src;
          d.dst = flows[f].dst;
          d.bytes = flows[f].bytes;
          ids[f] = e->InjectFlow(d);
          ++injected;
        }
      }
      if(injected == flows.size() && e->IsIdle()) break;
      e->Step();
    }
    r.drained = e->IsIdle() && e->FlowsOutstanding() == 0 &&
                (!e->PcieCreditsEnabled() || e->PcieCreditsIdle());
    delete e;
  }
  for(size_t f = 0; f < flows.size(); ++f) {
    r.sent.push_back(g_sent.count(ids[f]) ? (long long)g_sent[ids[f]] : -1);
    r.done.push_back(g_done.count(ids[f]) ? (long long)g_done[ids[f]] : -1);
  }
  return r;
}

Overrides Mode(const char * order) {
  Overrides ov;
  ov.push_back(std::make_pair(std::string("source_interleave"), std::string(order)));
  return ov;
}

bool Near(long long a, long long b, long long tol) { return llabs(a - b) <= tol; }

Flow F(int src, int dst, uint64_t bytes, uint64_t at = 0) {
  Flow f = { src, dst, bytes, at };
  return f;
}

} // namespace

int main() {
  const char * flat = "runfiles/simai_flat.cfg";

  // A lone flow is unaffected by the order.
  {
    std::vector<Flow> one(1, F(0, 1, 64 << 10));
    Run a = Simulate(flat, Mode("tlp"), one), b = Simulate(flat, Mode("flow"), one);
    CHECK(a.drained && b.drained);
    CHECK_EQ(a.sent[0], b.sent[0]);
    CHECK_EQ(a.done[0], b.done[0]);
    CHECK(Near(a.sent[0], kTlpsPer64K * kTlp, 2 * kTlp));
  }

  // Two equal flows from one GPU. Interleaved, both finish leaving together
  // after 512 TLPs; one at a time, the first leaves after 256 TLPs and the
  // second 256 TLPs later. The link is equally busy either way.
  {
    std::vector<Flow> two;
    two.push_back(F(0, 1, 64 << 10));
    two.push_back(F(0, 2, 64 << 10));
    Run t = Simulate(flat, Mode("tlp"), two), f = Simulate(flat, Mode("flow"), two);
    CHECK(t.drained && f.drained);
    CHECK(Near(t.sent[0], t.sent[1], kTlp));
    CHECK(Near(t.sent[1], 2 * kTlpsPer64K * kTlp, 2 * kTlp));
    CHECK(Near(f.sent[1] - f.sent[0], kTlpsPer64K * kTlp, kTlp));
    CHECK(Near(f.sent[0], kTlpsPer64K * kTlp, 2 * kTlp));
    CHECK(Near(t.sent[1], f.sent[1], kTlp));
  }

  // Three sizes, 64, 128 and 256 TLPs. Round robin: the smallest is done
  // after 3 x 64 TLPs, the middle after 64 + 2 x 128, the largest last, so the
  // gaps are 128 and 128 TLPs. One at a time: 64, 64 + 128, 64 + 128 + 256,
  // so the gaps are 128 and 256 TLPs.
  {
    std::vector<Flow> three;
    three.push_back(F(0, 1, 16 << 10));
    three.push_back(F(0, 2, 32 << 10));
    three.push_back(F(0, 3, 64 << 10));
    Run t = Simulate(flat, Mode("tlp"), three), f = Simulate(flat, Mode("flow"), three);
    CHECK(t.drained && f.drained);
    CHECK(Near(t.sent[0], 192 * kTlp, 2 * kTlp));
    CHECK(Near(t.sent[1] - t.sent[0], 128 * kTlp, kTlp));
    CHECK(Near(t.sent[2] - t.sent[1], 128 * kTlp, kTlp));
    CHECK(Near(f.sent[1] - f.sent[0], 128 * kTlp, kTlp));
    CHECK(Near(f.sent[2] - f.sent[1], 256 * kTlp, kTlp));
    // Each destination gets its data about when it left.
    for(int i = 0; i < 3; ++i) CHECK(t.done[i] > t.sent[i] && t.done[i] - t.sent[i] < 50);
  }

  // A flow that arrives later joins the rotation at once instead of queueing
  // behind the flow already sending.
  {
    std::vector<Flow> late;
    late.push_back(F(0, 1, 64 << 10, 0));
    late.push_back(F(0, 2, 16 << 10, 400));
    Run t = Simulate(flat, Mode("tlp"), late), f = Simulate(flat, Mode("flow"), late);
    CHECK(t.drained && f.drained);
    CHECK(t.sent[1] < t.sent[0]);
    // From cycle 400 the two alternate: B's 64 TLPs take 128 TLP slots, plus
    // at most the 8 TLPs of A already queued at the source.
    CHECK(t.sent[1] >= 400 + 128 * kTlp && t.sent[1] <= 400 + 136 * kTlp + kTlp);
    CHECK(f.sent[0] < f.sent[1]);
    CHECK(Near(f.sent[1] - f.sent[0], 64 * kTlp, kTlp));
  }

  // Interleaving many flows across a cyclic fabric with loop-free admission,
  // both retry orders, and with the VC0 credit model's whole-TLP links: every
  // flow completes and every credit comes back.
  {
    std::vector<Flow> a2a;
    for(int s = 0; s < 8; ++s) {
      for(int k = 1; k < 8; ++k) a2a.push_back(F(s, (s + k) % 8, 16 << 10));
    }
    const char * orders[] = { "rotate", "oldest_first" };
    for(int o = 0; o < 2; ++o) {
      Overrides ov = Mode("tlp");
      ov.push_back(std::make_pair(std::string("routing_function"), std::string("pinned")));
      ov.push_back(std::make_pair(std::string("pcinet_deadlock_avoidance"), std::string("loopfree")));
      ov.push_back(std::make_pair(std::string("loopfree_retry_order"), std::string(orders[o])));
      ov.push_back(std::make_pair(std::string("multipath_granularity"), std::string("request")));
      Run r = Simulate("runfiles/simai_ring_torus.cfg", ov, a2a);
      CHECK(r.drained);
      for(size_t i = 0; i < a2a.size(); ++i) CHECK(r.done[i] > 0);
    }
  }

  return TestSummary("test_source_interleave");
}
