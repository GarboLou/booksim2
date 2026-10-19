// $Id$

/*
 Copyright (c) 2026, Jiaqi Lou
 All rights reserved. Distributed under the same BSD license as BookSim; see
 LICENSE.md.
*/

#ifndef _TURN_GRAPH_HPP_
#define _TURN_GRAPH_HPP_

#include <string>
#include <utility>
#include <vector>

using namespace std;

// The pass-throughs (turns) of everything in flight, as a reference-counted
// graph over router-to-router channels: an edge a->b means some registered
// packet may hold a buffer behind channel a while it waits for channel b.
// Loop-free path selection (pcinet_deadlock_avoidance = loopfree) admits a path
// only if its turns keep this graph acyclic, which keeps whatever is in flight
// deadlock-free (Dally & Seitz) without extra VCs.
class TurnGraph {
public:
  typedef pair<int, int> Turn;   // (in channel, out channel)

  explicit TurnGraph( int channels = 0 );

  // Would registering these turns keep the graph acyclic? Turns already in
  // use add no edge, so a path made only of those is always admissible.
  bool CanAdd( const vector<Turn> & turns ) const;
  void Add( const vector<Turn> & turns );
  // Releases one reference per turn; aborts if a reference goes negative.
  void Remove( const vector<Turn> & turns );

  // Full check (Kahn's algorithm) for the debug option.
  bool IsAcyclic( ) const;

  int Refs( int a, int b ) const { return _refs[a * _n + b]; }
  long long TotalRefs( ) const { return _total; }
  int NumChannels( ) const { return _n; }

private:
  int _n;
  vector<int> _refs;            // [a * _n + b]
  vector<vector<int> > _succ;   // b for every edge a->b with refs > 0
  long long _total;
};

// Lifetime of loop-free admission units (a request, flow or packet). A unit's
// turns are registered when it is admitted, before its first packet is
// queued, and released exactly once: when it is fully generated and its last
// outstanding packet has retired. A unit whose packets have all retired while
// more remain to be generated keeps its turns, since its next packets will
// use the same path.
class LoopFreeUnits {
public:
  LoopFreeUnits( int pools = 0, int channels = 0 );

  bool CanAdmit( int pool, const vector<TurnGraph::Turn> & turns ) const {
    return _graphs[pool].CanAdd(turns);
  }
  // Registers the turns (which must outlive the unit) and returns the unit
  // id, or -1 when there are none: such a path can never close a loop.
  int  Admit( int pool, const vector<TurnGraph::Turn> * turns );
  void PacketGenerated( int unit );
  void UnitGenerated( int unit );     // no more packets will be generated
  void PacketRetired( int unit );     // a packet's tail left the network

  // Is unit live and registered for exactly this path's turns?
  bool Holds( int unit, const vector<TurnGraph::Turn> * turns ) const {
    return unit >= 0 && unit < (int)_units.size() && _units[unit].live &&
           _units[unit].turns == turns;
  }

  long long LiveUnits( ) const { return _live; }
  long long LiveRefs( ) const;
  long long Releases( ) const { return _releases; }
  const TurnGraph & Graph( int pool ) const { return _graphs[pool]; }
  int  NumPools( ) const { return (int)_graphs.size(); }
  // Empty if consistent; else what is wrong (cyclic graph, stray references).
  string Check( ) const;

private:
  struct Unit {
    int pool;
    const vector<TurnGraph::Turn> * turns;
    unsigned long long outstanding;
    bool fully_generated;
    bool live;
  };
  void _Release( int unit );

  vector<TurnGraph> _graphs;
  vector<Unit> _units;
  vector<int> _free;
  long long _live;
  long long _releases;
};

#endif
