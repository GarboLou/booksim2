// Path enumeration and pinned routing on the hand-authored PCINet topologies
// and generated test topologies.

#include <set>
#include <sys/wait.h>
#include <unistd.h>

#include "test_util.hpp"
#include "pcinet.hpp"
#include "flit.hpp"
#include "flowtrafficmanager.hpp"

namespace {

// Routers diagonal to each other on the 4-switch cycle: 2 shortest paths.
struct TopoCase {
  const char * cfg;
  std::set<std::pair<int, int> > diagonal;
  bool pinned_cyclic;
  bool min_cyclic;
  TopoCase() : cfg(NULL), pinned_cyclic(false), min_cyclic(false) {}
};

PCINet * Build(BookSimConfig & config, const std::string & cfg,
               const std::vector<std::pair<std::string, std::string> > & ov) {
  LoadConfig(config, cfg, ov);
  BooksimGlobalInit(config);
  QuietCout quiet;
  Network * n = Network::New(config, "network_0");
  return dynamic_cast<PCINet *>(n);
}

// Walk a pinned packet hop by hop through PinnedOutputPort and check it
// visits exactly the routers of its path and ends at the destination's port.
void WalkPinned(const PCINet * net, int src, int dst, int key) {
  Flit * f = Flit::New();
  f->src = src;
  f->dest = dst;
  f->head = true;
  f->path_key = key;
  f->pid = 0;
  const PCIePath & p = net->GetPath(src, dst, key);
  int r = net->NodeRouter(src);
  size_t hop = 0;
  while(true) {
    CHECK(hop < p.routers.size());
    if(hop >= p.routers.size()) break;
    CHECK_EQ(r, p.routers[hop]);
    int port = net->PinnedOutputPort(r, f);
    if(r == net->NodeRouter(dst)) {
      CHECK_EQ(port, net->NodePort(dst));
      break;
    }
    int next = net->NextRouter(r, port);
    CHECK(next >= 0);
    r = next;
    ++hop;
  }
  CHECK_EQ(hop + 1, p.routers.size());
  f->Free();
}

void CheckTopology(const TopoCase & tc) {
  std::cout << "-- " << tc.cfg << std::endl;
  BookSimConfig config;
  std::vector<std::pair<std::string, std::string> > ov;
  ov.push_back(std::make_pair("routing_function", "pinned"));
  PCINet * net = Build(config, tc.cfg, ov);
  CHECK(net != NULL);
  if(!net) return;

  const int nodes = net->NumNodes();
  for(int s = 0; s < nodes; ++s) {
    for(int t = 0; t < nodes; ++t) {
      int rs = net->NodeRouter(s), rt = net->NodeRouter(t);
      bool diag = tc.diagonal.count(std::make_pair(std::min(rs, rt), std::max(rs, rt))) > 0;
      CHECK_EQ(net->NumPaths(s, t), diag ? 2 : 1);

      std::set<std::vector<int> > distinct;
      for(int k = 0; k < net->NumPaths(s, t); ++k) {
        const PCIePath & p = net->GetPath(s, t, k);
        distinct.insert(p.routers);
        CHECK_EQ(p.routers.front(), rs);
        CHECK_EQ(p.routers.back(), rt);
        CHECK_EQ(p.ports.size() + 1, p.routers.size());
        CHECK_EQ(p.turns.size() + (p.ports.empty() ? 0 : 1), p.ports.size());
        for(size_t i = 0; i < p.ports.size(); ++i) {
          // each hop stays on a shortest path
          CHECK_EQ(net->NextRouter(p.routers[i], p.ports[i]), p.routers[i + 1]);
          CHECK_EQ(net->RouterDistance(p.routers[i], p.routers[i + 1]) +
                   net->RouterDistance(p.routers[i + 1], rt),
                   net->RouterDistance(p.routers[i], rt));
        }
        for(size_t i = 0; i < p.turns.size(); ++i) {
          CHECK_EQ(p.turns[i].first, net->ChannelId(p.routers[i], p.ports[i]));
          CHECK_EQ(p.turns[i].second, net->ChannelId(p.routers[i + 1], p.ports[i + 1]));
        }
        WalkPinned(net, s, t, k);
        // keys wrap modulo the path count
        WalkPinned(net, s, t, k + 7 * net->NumPaths(s, t));
      }
      CHECK_EQ((int)distinct.size(), net->NumPaths(s, t));
    }
  }
  CHECK_EQ(net->dependencyGraphIsCyclic("pinned"), tc.pinned_cyclic);
  // min's graph only ever uses the first candidate: acyclic on the 4-switch
  // cycles, but on an 8-switch ring its 3-hop routes chain all the way round.
  // The all-candidates graph of ecmp is cyclic exactly on the cycles.
  CHECK_EQ(net->dependencyGraphIsCyclic("min"), tc.min_cyclic);
  CHECK_EQ(net->dependencyGraphIsCyclic("ecmp"), tc.pinned_cyclic);

  delete net;
  BooksimGlobalCleanup();
}

void CheckTruncation() {
  std::cout << "-- max_paths_per_pair = 1" << std::endl;
  BookSimConfig config;
  std::vector<std::pair<std::string, std::string> > ov;
  ov.push_back(std::make_pair("routing_function", "pinned"));
  ov.push_back(std::make_pair("max_paths_per_pair", "1"));
  LoadConfig(config, "runfiles/simai_ring_torus.cfg", ov);
  BooksimGlobalInit(config);
  PCINet * net;
  std::string text;
  {
    QuietCout quiet;
    net = dynamic_cast<PCINet *>(Network::New(config, "network_0"));
    text = quiet.Text();
  }
  CHECK(net != NULL);
  if(!net) return;
  // G0 (SW0) -> G4 (SW2) is a diagonal pair: two paths, capped to one.
  CHECK_EQ(net->NumPaths(0, 4), 1);
  CHECK(text.find("WARNING: router 0 -> 2 has more than 1") != std::string::npos);
  // With one path per pair the pinned graph is min's graph: acyclic.
  CHECK_EQ(net->dependencyGraphIsCyclic("pinned"), false);
  delete net;
  BooksimGlobalCleanup();
}

// Two-socket dual-tree with an NTB (utils/gen_dual_socket.py, placement B,
// UPI at 32 GB/s): routers SW0 = 0, SW1 = 1, SW2 = 2, SW3 = 3, RC0 = 4,
// RC1 = 5; socket 0 = {0, 2, 4}, socket 1 = {1, 3, 5}. G0, G1 on SW0; G2, G3
// on SW2; G4, G5 on SW1; G6, G7 on SW3.
std::vector<int> R(int a, int b, int c = -1, int d = -1, int e = -1, int f = -1) {
  int v[6] = { a, b, c, d, e, f };
  std::vector<int> out;
  for(int i = 0; i < 6 && v[i] >= 0; ++i) out.push_back(v[i]);
  return out;
}

void CheckDualSocket() {
  std::cout << "-- tests/topo/dual4B_ntb.cfg, shortest" << std::endl;
  {
    BookSimConfig config;
    std::vector<std::pair<std::string, std::string> > ov;
    ov.push_back(std::make_pair("routing_function", "pinned"));
    PCINet * net = Build(config, "tests/topo/dual4B_ntb.cfg", ov);
    CHECK(net != NULL);
    if(!net) return;
    // Default: only the NTB route, the shortest one.
    CHECK_EQ(net->NumPaths(0, 4), 1);
    CHECK(net->GetPath(0, 4, 0).routers == R(0, 1));
    CHECK(net->GetPath(2, 6, 0).routers == R(2, 0, 1, 3));
    CHECK(net->GetPath(0, 4, 0).minimal);
    delete net;
    BooksimGlobalCleanup();
  }

  std::cout << "-- tests/topo/dual4B_ntb.cfg, per_bridge" << std::endl;
  BookSimConfig config;
  std::vector<std::pair<std::string, std::string> > ov;
  ov.push_back(std::make_pair("routing_function", "pinned"));
  ov.push_back(std::make_pair("pcinet_cross_domain_paths", "per_bridge"));
  PCINet * net = Build(config, "tests/topo/dual4B_ntb.cfg", ov);
  CHECK(net != NULL);
  if(!net) return;

  // Cross-socket: key 0 through the NTB (shortest), key 1 through RC0, UPI
  // and RC1. The UPI link runs at 2 cycles per flit.
  CHECK_EQ(net->NumPaths(0, 4), 2);
  CHECK(net->GetPath(0, 4, 0).routers == R(0, 1));
  CHECK(net->GetPath(0, 4, 1).routers == R(0, 4, 5, 1));
  CHECK_EQ(net->GetPath(0, 4, 0).bottleneck_cpf, 1);
  CHECK_EQ(net->GetPath(0, 4, 1).bottleneck_cpf, 2);
  CHECK(net->GetPath(0, 4, 0).minimal);
  CHECK(!net->GetPath(0, 4, 1).minimal);
  CHECK(net->GetPath(2, 6, 0).routers == R(2, 0, 1, 3));
  CHECK(net->GetPath(2, 6, 1).routers == R(2, 0, 4, 5, 1, 3));
  CHECK(net->GetPath(4, 0, 0).routers == R(1, 0));
  CHECK(net->GetPath(4, 0, 1).routers == R(1, 5, 4, 0));
  // Same socket: one route, inside the socket.
  CHECK_EQ(net->NumPaths(0, 2), 1);
  CHECK(net->GetPath(0, 2, 0).routers == R(0, 2));

  const int gpus = 8;   // node 8 is the inert placeholder on RC0
  for(int s = 0; s < gpus; ++s) {
    for(int t = 0; t < gpus; ++t) {
      int rs = net->NodeRouter(s), rt = net->NodeRouter(t);
      bool cross = net->RouterDomain(rs) != net->RouterDomain(rt);
      CHECK_EQ(net->NumPaths(s, t), cross ? 2 : 1);
      for(int k = 0; k < net->NumPaths(s, t); ++k) {
        const PCIePath & p = net->GetPath(s, t, k);
        std::set<int> seen(p.routers.begin(), p.routers.end());
        CHECK_EQ(seen.size(), p.routers.size());          // no router twice
        int crossings = 0;
        for(size_t i = 0; i + 1 < p.routers.size(); ++i) {
          CHECK_EQ(net->NextRouter(p.routers[i], p.ports[i]), p.routers[i + 1]);
          if(net->RouterDomain(p.routers[i]) != net->RouterDomain(p.routers[i + 1])) ++crossings;
        }
        CHECK_EQ(crossings, cross ? 1 : 0);
        CHECK_EQ(p.turns.size() + (p.ports.empty() ? 0 : 1), p.ports.size());
        WalkPinned(net, s, t, k);
      }
    }
  }
  // Every route crosses sockets once, so no pass-through continues from one
  // bridge into the other: no loop.
  CHECK_EQ(net->dependencyGraphIsCyclic("pinned"), false);
  delete net;
  BooksimGlobalCleanup();
}

// Building the network in a child must exit nonzero with `expect` printed.
void ExpectBuildError(const char * cfg,
                      const std::vector<std::pair<std::string, std::string> > & ov,
                      const char * expect) {
  int fds[2];
  if(pipe(fds) != 0) { CHECK(false); return; }
  pid_t pid = fork();
  if(pid == 0) {
    dup2(fds[1], 1);
    close(fds[0]);
    BookSimConfig config;
    LoadConfig(config, cfg, ov);
    BooksimGlobalInit(config);
    Network::New(config, "network_0");
    std::cout.flush();
    _exit(0);
  }
  close(fds[1]);
  std::string out;
  char buf[512];
  ssize_t n;
  while((n = read(fds[0], buf, sizeof(buf))) > 0) out.append(buf, n);
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) != 0);
  if(out.find(expect) == std::string::npos) {
    std::cout << "  expected '" << expect << "'" << std::endl;
    CHECK(false);
  }
}

void CheckDualSocketErrors() {
  std::cout << "-- per_bridge configuration errors" << std::endl;
  std::vector<std::pair<std::string, std::string> > ov;
  ov.push_back(std::make_pair("routing_function", "pinned"));
  ov.push_back(std::make_pair("pcinet_cross_domain_paths", "per_bridge"));
  // Distance classes assume shortest routes.
  ov.push_back(std::make_pair("pcinet_deadlock_avoidance", "vc_classes"));
  const char * dual = "tests/topo/dual4B_ntb.cfg";
  ExpectBuildError(dual, ov, "only order shortest routes");
  ov[0].second = "min";
  ov.pop_back();
  ExpectBuildError(dual, ov, "needs routing_function = pinned");
  ov[0].second = "pinned";
  ov[1].second = "bogus";
  ExpectBuildError(dual, ov, "must be shortest or per_bridge");
}

// Deadlock-guard case: a full
// mesh split into domains {0,1} and {2,3}. Every global shortest route is one
// hop, so the distance-class count is 1, yet the per-bridge routes with key 2
// close the cycle 0->1->2->3->0. The guard must not depend on the class count.
void CheckGuardMesh() {
  std::cout << "-- tests/topo/guard_mesh4.cfg, per_bridge" << std::endl;
  const char * cfg = "tests/topo/guard_mesh4.cfg";
  std::vector<std::pair<std::string, std::string> > ov;
  ov.push_back(std::make_pair("routing_function", "pinned"));
  ov.push_back(std::make_pair("pcinet_cross_domain_paths", "per_bridge"));
  ov.push_back(std::make_pair("pcinet_deadlock_avoidance", "auto"));
  ExpectBuildError(cfg, ov, "only order shortest routes");
  ov[2].second = "vc_classes";
  ExpectBuildError(cfg, ov, "only order shortest routes");

  // loopfree and the explicit opt-out build; the path set is as documented.
  const char * ok_modes[2] = { "loopfree", "none" };
  for(int m = 0; m < 2; ++m) {
    BookSimConfig config;
    ov[2].second = ok_modes[m];
    PCINet * net = Build(config, cfg, ov);
    CHECK(net != NULL);
    if(!net) continue;
    // 0 -> 2: the direct bridge, then (latency 2) bridges 0-3 and 1-2 in
    // router order, then bridge 1-3.
    CHECK_EQ(net->NumPaths(0, 2), 4);
    CHECK(net->GetPath(0, 2, 0).routers == R(0, 2));
    CHECK(net->GetPath(0, 2, 1).routers == R(0, 3, 2));
    CHECK(net->GetPath(0, 2, 2).routers == R(0, 1, 2));
    CHECK(net->GetPath(0, 2, 3).routers == R(0, 1, 3, 2));
    // The four key-2 routes that close the cycle.
    CHECK(net->GetPath(1, 3, 2).routers == R(1, 2, 3));
    CHECK(net->GetPath(2, 0, 2).routers == R(2, 3, 0));
    CHECK(net->GetPath(3, 1, 2).routers == R(3, 0, 1));
    CHECK(!net->GetPath(0, 2, 2).minimal);
    CHECK_EQ(net->dependencyGraphIsCyclic("pinned"), true);
    CHECK_EQ(net->LoopFree(), m == 0);
    delete net;
    BooksimGlobalCleanup();
  }
}

// per_bridge holds per pair, with no fallback to other routes when a pair has
// no single bridge (cross) or no route inside its domain (same).
void CheckNoFallback() {
  const char * cfgs[2] = { "tests/topo/fallback_same.cfg", "tests/topo/fallback_cross.cfg" };
  for(int c = 0; c < 2; ++c) {
    std::cout << "-- " << cfgs[c] << std::endl;
    for(int pb = 0; pb < 2; ++pb) {
      BookSimConfig config;
      std::vector<std::pair<std::string, std::string> > ov;
      ov.push_back(std::make_pair("routing_function", "pinned"));
      ov.push_back(std::make_pair("pcinet_cross_domain_paths", pb ? "per_bridge" : "shortest"));
      LoadConfig(config, cfgs[c], ov);
      BooksimGlobalInit(config);
      PCINet * net;
      std::string text;
      {
        QuietCout quiet;
        net = dynamic_cast<PCINet *>(Network::New(config, "network_0"));
        text = quiet.Text();
      }
      CHECK(net != NULL);
      if(!net) continue;
      // 1 -> 2 needs 1 -> 0 -> 2: it leaves and re-enters domain 1, or (cross)
      // crosses two bridges. Allowed by default, rejected under per_bridge.
      CHECK_EQ(net->NumPaths(1, 2), pb ? 0 : 1);
      CHECK_EQ(net->NumPaths(2, 1), pb ? 0 : 1);
      CHECK_EQ(net->NumPaths(0, 1), 1);               // one bridge: supported
      CHECK_EQ(net->NumPaths(1, 0), 1);
      CHECK_EQ(text.find("have no route under the policy") != std::string::npos, pb == 1);
      delete net;
      BooksimGlobalCleanup();
    }
  }
}

void CheckSwrrWeights() {
  std::cout << "-- SwrrWeights" << std::endl;
  std::vector<long long> w;
  std::vector<int> c;
  c.push_back(1); c.push_back(2);
  CHECK(SwrrWeights(c, w)); CHECK_EQ(w[0], 2); CHECK_EQ(w[1], 1);
  c[0] = 2; c[1] = 3;
  CHECK(SwrrWeights(c, w)); CHECK_EQ(w[0], 3); CHECK_EQ(w[1], 2);
  // Boundary: a very slow path keeps a positive weight.
  c[0] = 1; c[1] = 720721;
  CHECK(SwrrWeights(c, w)); CHECK_EQ(w[0], 720721); CHECK_EQ(w[1], 1);
  c.push_back(4); c[0] = 1; c[1] = 2;
  CHECK(SwrrWeights(c, w)); CHECK_EQ(w[0], 4); CHECK_EQ(w[1], 2); CHECK_EQ(w[2], 1);
  // Every pair of cpf in 1..16: exact inverse ratio, all weights positive.
  for(int a = 1; a <= 16; ++a) {
    for(int b = 1; b <= 16; ++b) {
      std::vector<int> ab(2);
      ab[0] = a; ab[1] = b;
      CHECK(SwrrWeights(ab, w));
      CHECK(w[0] > 0 && w[1] > 0);
      CHECK_EQ(w[0] * a, w[1] * b);
    }
  }
  std::vector<int> bad(1, 0);
  CHECK(!SwrrWeights(bad, w));
  std::vector<int> huge;                            // pairwise coprime, lcm > 2^40
  huge.push_back(1048573); huge.push_back(1048571); huge.push_back(1048559);
  CHECK(!SwrrWeights(huge, w));
  // Overflow: the third
  // product overflows 64 bits; it must be rejected before multiplying, not
  // wrap to negative weights.
  std::vector<int> wrap;
  wrap.push_back(1000003); wrap.push_back(1000033); wrap.push_back(1234567891);
  CHECK(!SwrrWeights(wrap, w));
  std::vector<int> big(1, 2147483647);              // one path: L = cpf, weight 1
  CHECK(SwrrWeights(big, w)); CHECK_EQ(w[0], 1);
  // Exactly at the bound is accepted, one past it is not.
  std::vector<int> edge;
  edge.push_back(1 << 20); edge.push_back(3);       // L = 3 * 2^20 < 2^40
  CHECK(SwrrWeights(edge, w)); CHECK_EQ(w[0], 3); CHECK_EQ(w[1], 1 << 20);
  std::vector<int> at(2);
  at[0] = 1 << 20; at[1] = 1 << 20;                 // L = 2^20
  CHECK(SwrrWeights(at, w));
  at[0] = 1 << 30; at[1] = 1 << 11;                 // L = 2^30 (power of 2 divides)
  CHECK(SwrrWeights(at, w)); CHECK_EQ(w[1], 1 << 19);
  at[0] = 1073741789; at[1] = 1031;                 // coprime: L ~ 1.1e12 > 2^40
  CHECK(!SwrrWeights(at, w));
  for(size_t k = 0; k < w.size(); ++k) CHECK(w[k] > 0);
}

} // namespace

int main() {
  std::vector<TopoCase> cases(5);
  cases[0].cfg = "runfiles/simai_flat.cfg";       cases[0].pinned_cyclic = false;
  cases[1].cfg = "runfiles/simai_dual_tree.cfg";  cases[1].pinned_cyclic = false;
  cases[2].cfg = "runfiles/simai_ntb_tree.cfg";   cases[2].pinned_cyclic = false;
  cases[3].cfg = "runfiles/simai_mesh_grid.cfg";  cases[3].pinned_cyclic = true;
  cases[3].diagonal.insert(std::make_pair(0, 3));
  cases[3].diagonal.insert(std::make_pair(1, 2));
  cases[4].cfg = "runfiles/simai_ring_torus.cfg"; cases[4].pinned_cyclic = true;
  cases[4].diagonal.insert(std::make_pair(0, 2));
  cases[4].diagonal.insert(std::make_pair(1, 3));

  // Generated (utils/gen_topology.py): an 8-switch ring, where the opposite
  // switch is 4 hops away both ways, and a 4-switch full mesh (1 hop, no
  // pass-throughs at all).
  TopoCase ring8;
  ring8.cfg = "tests/topo/ring8.cfg"; ring8.pinned_cyclic = true; ring8.min_cyclic = true;
  for(int r = 0; r < 4; ++r) ring8.diagonal.insert(std::make_pair(r, r + 4));
  cases.push_back(ring8);
  TopoCase mesh4;
  mesh4.cfg = "tests/topo/full_mesh4.cfg"; mesh4.pinned_cyclic = false;
  cases.push_back(mesh4);

  // Topologies whose switch graph has cycles but whose routes form no cycle
  // of pass-throughs. Leaf pairs of a spine-leaf have one path per
  // spine; the ring + chord SW1-SW3 keeps two paths only for SW0-SW2. Paths
  // between the GPU-less spines must not be enumerated: they would make the
  // pinned graph look cyclic.
  TopoCase sl22;
  sl22.cfg = "tests/topo/spineleaf22.cfg";
  sl22.diagonal.insert(std::make_pair(2, 3));
  cases.push_back(sl22);
  TopoCase sl24;
  sl24.cfg = "tests/topo/spineleaf24.cfg";
  for(int a = 2; a < 6; ++a)
    for(int b = a + 1; b < 6; ++b) sl24.diagonal.insert(std::make_pair(a, b));
  cases.push_back(sl24);
  TopoCase chord;
  chord.cfg = "tests/topo/ringchord.cfg";
  chord.diagonal.insert(std::make_pair(0, 2));
  cases.push_back(chord);

  for(size_t i = 0; i < cases.size(); ++i) CheckTopology(cases[i]);
  CheckTruncation();
  CheckDualSocket();
  CheckDualSocketErrors();
  CheckGuardMesh();
  CheckNoFallback();
  CheckSwrrWeights();
  return TestSummary("test_pcinet_paths");
}
