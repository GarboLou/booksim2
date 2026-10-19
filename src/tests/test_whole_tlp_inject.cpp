// whole_tlp at the source: once a GPU starts a TLP on its link, the link
// carries only that TLP until its tail, even when another BookSim traffic
// class has a packet waiting, of equal or higher priority. A traffic class is a separate source queue (classes = 2), not a
// PCIe type. Each case runs in a child with check_whole_tlp = 1, which ends
// the process on any split TLP. On runfiles/simai_flat.cfg a full TLP
// (256 B + 24 B overhead) is 5 flits, 5 cycles on a GPU link.

#include <sys/wait.h>
#include <unistd.h>
#include <cstdlib>
#include <map>
#include <sstream>

#include "test_util.hpp"
#include "booksim_engine.hpp"

namespace {

typedef std::vector<std::pair<std::string, std::string> > Overrides;

const long long kTlp = 5;    // cycles per full TLP on a Gen5 x16 link
const long long kTlps = 16;  // TLPs in a 4 KiB flow

struct Flow {
  int src, dst, cls;
  uint64_t at;   // cycle at which it is injected
};

struct Outcome {
  bool clean;    // the child finished: no split TLP, no other error
  bool drained;
  std::vector<long long> sent;   // per flow: cycle its last byte left the source
  std::string out;
  Outcome() : clean(false), drained(false) {}
};

std::map<booksim::FlowId, uint64_t> g_sent;
void OnSend(booksim::FlowId id, void *, uint64_t t, void *) { g_sent[id] = t; }

void Set(Overrides & ov, const char * k, const char * v) {
  ov.push_back(std::make_pair(std::string(k), std::string(v)));
}

// The VC0 switch model as utils/vc0_config.py writes it for the flat
// topology (switch radix 9), with two traffic classes and the check on.
Overrides Vc0(bool whole_tlp) {
  Overrides ov;
  Set(ov, "speculative", "1");
  Set(ov, "noq", "1");
  Set(ov, "noq_vcs_per_output", "1");
  Set(ov, "num_vcs", "27");
  Set(ov, "write_request_begin_vc", "0");  Set(ov, "write_request_end_vc", "8");
  Set(ov, "read_request_begin_vc", "9");   Set(ov, "read_request_end_vc", "17");
  Set(ov, "read_reply_begin_vc", "18");    Set(ov, "read_reply_end_vc", "26");
  Set(ov, "pcie_default_class", "P");
  Set(ov, "buffer_policy", "shared");
  Set(ov, "pcie_credits", "1");
  Set(ov, "pcie_ph", "32");   Set(ov, "pcie_pd_bytes", "8192");
  Set(ov, "pcie_nph", "32");  Set(ov, "pcie_npd_bytes", "1024");
  Set(ov, "pcie_cplh", "0");  Set(ov, "pcie_cpld_bytes", "0");
  Set(ov, "whole_tlp", whole_tlp ? "1" : "0");
  Set(ov, "check_whole_tlp", "1");
  Set(ov, "classes", "2");
  return ov;
}

// Class 1 outranks class 0.
Overrides HighClass1(Overrides ov) {
  Set(ov, "priority", "class");
  Set(ov, "class_priority", "{0,1}");
  return ov;
}

void Child(const Overrides & ov, const std::vector<Flow> & flows) {
  booksim::Engine * e = booksim::Engine::Create("runfiles/simai_flat.cfg", ov);
  if(!e) _exit(3);
  e->SetOnSendComplete(&OnSend, NULL);
  std::vector<booksim::FlowId> ids(flows.size(), booksim::kInvalidFlow);
  size_t injected = 0;
  for(uint64_t i = 0; i < 200000; ++i) {
    for(size_t f = 0; f < flows.size(); ++f) {
      if(ids[f] == booksim::kInvalidFlow && flows[f].at == e->Time()) {
        booksim::FlowDesc d;
        d.src = flows[f].src;
        d.dst = flows[f].dst;
        d.bytes = 4096;
        d.cls = flows[f].cls;
        ids[f] = e->InjectFlow(d);
        ++injected;
      }
    }
    if(injected == flows.size() && e->IsIdle()) break;
    e->Step();
  }
  const bool drained = e->IsIdle() && e->FlowsOutstanding() == 0 && e->PcieCreditsIdle();
  std::cout << "RESULT " << (drained ? 1 : 0);
  for(size_t f = 0; f < flows.size(); ++f) {
    std::cout << " " << (g_sent.count(ids[f]) ? (long long)g_sent[ids[f]] : -1LL);
  }
  std::cout << std::endl;
  delete e;
  _exit(0);
}

Outcome Run(const Overrides & ov, const std::vector<Flow> & flows) {
  Outcome o;
  int fds[2];
  if(pipe(fds) != 0) return o;
  pid_t pid = fork();
  if(pid == 0) {
    dup2(fds[1], 1);
    close(fds[0]);
    Child(ov, flows);
  }
  close(fds[1]);
  char buf[512];
  ssize_t n;
  while((n = read(fds[0], buf, sizeof(buf))) > 0) o.out.append(buf, n);
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  size_t at = o.out.rfind("RESULT ");
  o.clean = WIFEXITED(status) && WEXITSTATUS(status) == 0 && at != std::string::npos;
  if(o.clean) {
    std::istringstream is(o.out.substr(at + 7));
    int d = 0;
    is >> d;
    o.drained = (d == 1);
    long long t;
    while(is >> t) o.sent.push_back(t);
  } else {
    size_t err = o.out.find("Error");
    std::cout << "  child failed: "
              << (err == std::string::npos ? std::string("(no message)")
                                           : o.out.substr(err, o.out.find('\n', err) - err))
              << std::endl;
  }
  return o;
}

Flow F(int src, int dst, int cls, uint64_t at = 0) {
  Flow f = { src, dst, cls, at };
  return f;
}

bool CleanRun(const Outcome & o, size_t flows) {
  return o.clean && o.drained && o.sent.size() == flows;
}

} // namespace

int main() {
  // Two classes of equal priority, both waiting from cycle 0. Without the
  // fix the link alternated between the class queues every flit.
  {
    std::vector<Flow> fl;
    fl.push_back(F(0, 1, 0));
    fl.push_back(F(0, 2, 1));
    Outcome two = Run(Vc0(true), fl);
    CHECK(CleanRun(two, 2));
    // The link alternates whole TLPs, as one class with two flows does.
    fl[1].cls = 0;
    Outcome one = Run(Vc0(true), fl);
    CHECK(CleanRun(one, 2));
    if(two.sent.size() == 2 && one.sent.size() == 2) {
      CHECK_EQ(two.sent[0] + two.sent[1], one.sent[0] + one.sent[1]);
      long long gap = two.sent[1] - two.sent[0];
      CHECK(gap == kTlp || gap == -kTlp);
    }
  }

  // Class 1 outranks class 0 and arrives while class 0's first TLP is on
  // the wire: it waits for that TLP's tail, then takes the link (priority
  // still holds at TLP boundaries), and class 0 resumes after it.
  for(uint64_t at = 1; at < (uint64_t)kTlp; ++at) {
    std::vector<Flow> fl;
    fl.push_back(F(0, 1, 0));
    fl.push_back(F(0, 2, 1, at));
    Outcome o = Run(HighClass1(Vc0(true)), fl);
    CHECK(CleanRun(o, 2));
    if(o.sent.size() == 2) {
      CHECK(o.sent[1] < o.sent[0]);
      CHECK_EQ(o.sent[0] - o.sent[1], (kTlps - 1) * kTlp);
    }
  }

  // The same with BookSim's own hold on: it alone did not stop the
  // higher class.
  {
    std::vector<Flow> fl;
    fl.push_back(F(0, 1, 0));
    fl.push_back(F(0, 2, 1, 2));
    Overrides ov = HighClass1(Vc0(true));
    Set(ov, "hold_switch_for_packet", "1");
    CHECK(CleanRun(Run(ov, fl), 2));
  }

  // Two GPUs meet at the switch output to G2, the higher class arriving
  // second: the switch keeps each TLP whole too.
  {
    std::vector<Flow> fl;
    fl.push_back(F(0, 2, 0));
    fl.push_back(F(1, 2, 1, 2));
    CHECK(CleanRun(Run(HighClass1(Vc0(true)), fl), 2));
  }

  // Negative control: with whole_tlp = 0 the same two-class source splits
  // TLPs, and the check must stop it.
  {
    std::vector<Flow> fl;
    fl.push_back(F(0, 1, 0));
    fl.push_back(F(0, 2, 1));
    std::cout << "  (a split is expected next)" << std::endl;
    Outcome o = Run(Vc0(false), fl);
    CHECK(!o.clean);
    CHECK(o.out.find("check_whole_tlp") != std::string::npos);
  }

  return TestSummary("test_whole_tlp_inject");
}
