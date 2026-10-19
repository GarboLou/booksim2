// TurnGraph: the admission test behind loop-free path selection
// (pcinet_deadlock_avoidance = loopfree).

#include <cstdlib>

#include "test_util.hpp"
#include "turn_graph.hpp"

namespace {

typedef TurnGraph::Turn Turn;

// Reference answer: Kahn's algorithm on the edges in use plus the candidate's.
bool BruteCanAdd(const TurnGraph & g, const std::vector<Turn> & cand) {
  int n = g.NumChannels();
  std::vector<std::vector<char> > adj(n, std::vector<char>(n, 0));
  for(int a = 0; a < n; ++a)
    for(int b = 0; b < n; ++b)
      if(g.Refs(a, b) > 0) adj[a][b] = 1;
  for(size_t i = 0; i < cand.size(); ++i) adj[cand[i].first][cand[i].second] = 1;
  std::vector<int> indeg(n, 0);
  for(int a = 0; a < n; ++a)
    for(int b = 0; b < n; ++b)
      indeg[b] += adj[a][b];
  std::vector<int> ready;
  for(int v = 0; v < n; ++v) if(!indeg[v]) ready.push_back(v);
  int peeled = 0;
  while(!ready.empty()) {
    int v = ready.back(); ready.pop_back(); ++peeled;
    for(int b = 0; b < n; ++b)
      if(adj[v][b] && --indeg[b] == 0) ready.push_back(b);
  }
  return peeled == n;
}

// Ring example: ring SW0-SW1-SW2-SW3-SW0, four a2a flows
// each passing through one switch clockwise. Any three are fine together; the
// fourth closes the loop.
void RingExample() {
  // clockwise channels: c01=0, c12=1, c23=2, c30=3
  TurnGraph g(4);
  std::vector<Turn> g0_g4(1, Turn(0, 1));   // SW0->SW1->SW2
  std::vector<Turn> g2_g6(1, Turn(1, 2));   // SW1->SW2->SW3
  std::vector<Turn> g4_g0(1, Turn(2, 3));   // SW2->SW3->SW0
  std::vector<Turn> g6_g2(1, Turn(3, 0));   // SW3->SW0->SW1
  CHECK(g.CanAdd(g0_g4)); g.Add(g0_g4);
  CHECK(g.CanAdd(g2_g6)); g.Add(g2_g6);
  CHECK(g.CanAdd(g4_g0)); g.Add(g4_g0);
  CHECK(!g.CanAdd(g6_g2));
  CHECK(g.IsAcyclic());
  // A second packet on a path already in use adds no edge: admissible.
  CHECK(g.CanAdd(g0_g4)); g.Add(g0_g4);
  CHECK_EQ(g.Refs(0, 1), 2);
  // Releasing one of two references keeps the edge, so G6->G2 still waits.
  g.Remove(g0_g4);
  CHECK_EQ(g.Refs(0, 1), 1);
  CHECK(!g.CanAdd(g6_g2));
  // Once G0->G4 has drained completely, G6->G2 may go.
  g.Remove(g0_g4);
  CHECK_EQ(g.Refs(0, 1), 0);
  CHECK(g.CanAdd(g6_g2)); g.Add(g6_g2);
  CHECK(g.IsAcyclic());
  CHECK_EQ(g.TotalRefs(), 3);
  g.Remove(g2_g6); g.Remove(g4_g0); g.Remove(g6_g2);
  CHECK_EQ(g.TotalRefs(), 0);
}

void EdgeCases() {
  TurnGraph g(4);
  // Empty candidate (a path of 0-1 switch hops) is always admissible.
  CHECK(g.CanAdd(std::vector<Turn>()));
  // A candidate whose own new edges close a cycle is refused.
  std::vector<Turn> self;
  self.push_back(Turn(0, 1));
  self.push_back(Turn(1, 0));
  CHECK(!g.CanAdd(self));
  // Duplicate turns in one candidate count once for the test, twice for refs.
  std::vector<Turn> dup(2, Turn(2, 3));
  CHECK(g.CanAdd(dup)); g.Add(dup);
  CHECK_EQ(g.Refs(2, 3), 2);
  g.Remove(dup);
  CHECK_EQ(g.Refs(2, 3), 0);
  // A cycle formed by one new edge plus edges already in use.
  std::vector<Turn> chain;
  chain.push_back(Turn(0, 1));
  chain.push_back(Turn(1, 2));
  g.Add(chain);
  CHECK(!g.CanAdd(std::vector<Turn>(1, Turn(2, 0))));
  CHECK(g.CanAdd(std::vector<Turn>(1, Turn(0, 2))));   // shortcut, no cycle
  g.Remove(chain);

  // Joint cycle: with c->d, d->e, e->f, f->a in use, the
  // candidate {a->b, b->c} closes a cycle only as a pair; each turn alone is
  // safe against the old graph.
  enum { a, b, c, d, e, f };
  TurnGraph six(6);
  std::vector<Turn> in_use;
  in_use.push_back(Turn(c, d));
  in_use.push_back(Turn(d, e));
  in_use.push_back(Turn(e, f));
  in_use.push_back(Turn(f, a));
  six.Add(in_use);
  CHECK(six.CanAdd(std::vector<Turn>(1, Turn(a, b))));
  CHECK(six.CanAdd(std::vector<Turn>(1, Turn(b, c))));
  std::vector<Turn> pair;
  pair.push_back(Turn(a, b));
  pair.push_back(Turn(b, c));
  CHECK(!six.CanAdd(pair));
  // The refused candidate left the graph untouched.
  CHECK_EQ(six.TotalRefs(), 4);
  CHECK_EQ(six.Refs(a, b), 0);
  CHECK(six.IsAcyclic());
}

// Randomized: interleave admissions and releases; CanAdd must agree with the
// brute-force check and the graph must stay acyclic throughout.
void Randomized() {
  srand(12345);
  const int n = 8;
  TurnGraph g(n);
  std::vector<std::vector<Turn> > live;
  int admitted = 0, refused = 0;
  for(int step = 0; step < 20000; ++step) {
    if(!live.empty() && (rand() % 3 == 0)) {
      size_t k = rand() % live.size();
      g.Remove(live[k]);
      live.erase(live.begin() + k);
    } else {
      std::vector<Turn> cand;
      int len = rand() % 4;
      for(int i = 0; i < len; ++i) {
        int a = rand() % n, b = rand() % n;
        if(a != b) cand.push_back(Turn(a, b));
      }
      bool ok = g.CanAdd(cand);
      CHECK_EQ(ok, BruteCanAdd(g, cand));
      if(ok) { g.Add(cand); live.push_back(cand); ++admitted; } else { ++refused; }
    }
    if(step % 97 == 0) CHECK(g.IsAcyclic());
  }
  for(size_t k = 0; k < live.size(); ++k) g.Remove(live[k]);
  CHECK_EQ(g.TotalRefs(), 0);
  CHECK(admitted > 1000 && refused > 1000);   // both branches exercised
}

// Unit lifetime: a unit's turns stay registered until it is fully generated
// and its last packet has retired, even if all earlier packets have retired.
void UnitLifetime() {
  std::vector<Turn> path(1, Turn(0, 1));
  std::vector<Turn> other(1, Turn(1, 0));   // would close a loop with `path`
  LoopFreeUnits lf(1, 4);

  int u = lf.Admit(0, &path);
  CHECK(u >= 0);
  CHECK_EQ(lf.Graph(0).Refs(0, 1), 1);

  // Generation pauses with every generated packet already retired: the unit is
  // not fully generated, so its turns must stay registered.
  lf.PacketGenerated(u);
  lf.PacketGenerated(u);
  lf.PacketRetired(u);
  lf.PacketRetired(u);
  CHECK_EQ(lf.Graph(0).Refs(0, 1), 1);
  CHECK_EQ(lf.LiveUnits(), 1);
  CHECK(!lf.CanAdmit(0, other));
  CHECK_EQ(lf.Releases(), 0);

  // Generation resumes and finishes while one packet is still in flight.
  lf.PacketGenerated(u);
  lf.UnitGenerated(u);
  CHECK_EQ(lf.Graph(0).Refs(0, 1), 1);
  CHECK_EQ(lf.Releases(), 0);
  // Its last tail retires: released exactly once.
  lf.PacketRetired(u);
  CHECK_EQ(lf.Graph(0).Refs(0, 1), 0);
  CHECK_EQ(lf.Releases(), 1);
  CHECK_EQ(lf.LiveUnits(), 0);
  CHECK(lf.CanAdmit(0, other));

  // Fully generated with nothing outstanding releases immediately.
  int v = lf.Admit(0, &other);
  lf.PacketGenerated(v);
  lf.PacketRetired(v);
  CHECK_EQ(lf.Releases(), 1);
  lf.UnitGenerated(v);
  CHECK_EQ(lf.Releases(), 2);

  // A path without pass-throughs is not tracked at all.
  std::vector<Turn> none;
  CHECK_EQ(lf.Admit(0, &none), -1);

  // Slots are reused; pools are independent.
  LoopFreeUnits two(2, 4);
  int a = two.Admit(0, &path);
  CHECK(two.CanAdmit(1, other));           // other pool: no conflict
  CHECK(!two.CanAdmit(0, other));
  two.UnitGenerated(a);                    // no packets: releases at once
  int b = two.Admit(1, &other);
  CHECK_EQ(a, b);                          // slot recycled
  CHECK_EQ(two.Graph(1).Refs(1, 0), 1);
  CHECK(two.Check().empty());
  two.PacketGenerated(b);
  two.UnitGenerated(b);
  two.PacketRetired(b);
  CHECK_EQ(two.LiveUnits(), 0);
  CHECK_EQ(two.LiveRefs(), 0);
  CHECK(two.Check().empty());
}

} // namespace

int main() {
  RingExample();
  EdgeCases();
  Randomized();
  UnitLifetime();
  return TestSummary("test_turn_graph");
}
