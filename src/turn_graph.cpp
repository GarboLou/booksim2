// $Id$

/*
 Copyright (c) 2026, Jiaqi Lou
 All rights reserved. Distributed under the same BSD license as BookSim; see
 LICENSE.md.
*/

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>

#include "turn_graph.hpp"

TurnGraph::TurnGraph( int channels )
  : _n(channels), _refs(channels * channels, 0), _succ(channels), _total(0)
{
}

bool TurnGraph::CanAdd( const vector<Turn> & turns ) const
{
  // The edges this candidate would create. The graph is acyclic now, so any
  // new cycle passes through at least one of them: adding a->b closes a cycle
  // iff a is reachable from b once all the new edges are in.
  vector<Turn> fresh;
  for(size_t i = 0; i < turns.size(); ++i) {
    if(Refs(turns[i].first, turns[i].second) == 0 &&
       find(fresh.begin(), fresh.end(), turns[i]) == fresh.end()) {
      fresh.push_back(turns[i]);
    }
  }
  if(fresh.empty()) return true;

  vector<char> seen(_n);
  vector<int> stack;
  for(size_t e = 0; e < fresh.size(); ++e) {
    const int target = fresh[e].first;
    fill(seen.begin(), seen.end(), 0);
    stack.assign(1, fresh[e].second);
    seen[fresh[e].second] = 1;
    while(!stack.empty()) {
      int v = stack.back();
      stack.pop_back();
      if(v == target) return false;
      for(size_t k = 0; k < _succ[v].size(); ++k) {
        int w = _succ[v][k];
        if(!seen[w]) { seen[w] = 1; stack.push_back(w); }
      }
      for(size_t k = 0; k < fresh.size(); ++k) {
        if(fresh[k].first == v && !seen[fresh[k].second]) {
          seen[fresh[k].second] = 1;
          stack.push_back(fresh[k].second);
        }
      }
    }
  }
  return true;
}

void TurnGraph::Add( const vector<Turn> & turns )
{
  for(size_t i = 0; i < turns.size(); ++i) {
    int a = turns[i].first, b = turns[i].second;
    if(_refs[a * _n + b]++ == 0) _succ[a].push_back(b);
    ++_total;
  }
}

void TurnGraph::Remove( const vector<Turn> & turns )
{
  for(size_t i = 0; i < turns.size(); ++i) {
    int a = turns[i].first, b = turns[i].second;
    int & r = _refs[a * _n + b];
    if(r <= 0) {
      cerr << "TurnGraph: released turn " << a << "->" << b
           << " that holds no reference" << endl;
      abort();
    }
    if(--r == 0) {
      vector<int> & s = _succ[a];
      s.erase(find(s.begin(), s.end(), b));
    }
    --_total;
  }
}

bool TurnGraph::IsAcyclic( ) const
{
  vector<int> indeg(_n, 0);
  for(int a = 0; a < _n; ++a) {
    for(size_t k = 0; k < _succ[a].size(); ++k) ++indeg[_succ[a][k]];
  }
  vector<int> ready;
  for(int v = 0; v < _n; ++v) {
    if(indeg[v] == 0) ready.push_back(v);
  }
  int peeled = 0;
  while(!ready.empty()) {
    int v = ready.back();
    ready.pop_back();
    ++peeled;
    for(size_t k = 0; k < _succ[v].size(); ++k) {
      if(--indeg[_succ[v][k]] == 0) ready.push_back(_succ[v][k]);
    }
  }
  return peeled == _n;
}

LoopFreeUnits::LoopFreeUnits( int pools, int channels )
  : _graphs(pools, TurnGraph(channels)), _live(0), _releases(0)
{
}

int LoopFreeUnits::Admit( int pool, const vector<TurnGraph::Turn> * turns )
{
  if(turns->empty()) return -1;
  int u;
  if(_free.empty()) {
    u = (int)_units.size();
    _units.push_back(Unit());
  } else {
    u = _free.back();
    _free.pop_back();
  }
  Unit & x = _units[u];
  x.pool = pool;
  x.turns = turns;
  x.outstanding = 0;
  x.fully_generated = false;
  x.live = true;
  _graphs[pool].Add(*turns);
  ++_live;
  return u;
}

void LoopFreeUnits::PacketGenerated( int unit )
{
  Unit & x = _units[unit];
  if(!x.live || x.fully_generated) {
    cerr << "LoopFreeUnits: packet generated for unit " << unit
         << (x.live ? " after it was fully generated" : " that is not live") << endl;
    abort();
  }
  ++x.outstanding;
}

void LoopFreeUnits::UnitGenerated( int unit )
{
  Unit & x = _units[unit];
  if(!x.live || x.fully_generated) {
    cerr << "LoopFreeUnits: unit " << unit << " finished generating twice" << endl;
    abort();
  }
  x.fully_generated = true;
  if(x.outstanding == 0) _Release(unit);
}

void LoopFreeUnits::PacketRetired( int unit )
{
  Unit & x = _units[unit];
  if(!x.live || x.outstanding == 0) {
    cerr << "LoopFreeUnits: packet retired for unit " << unit
         << " with nothing outstanding" << endl;
    abort();
  }
  if(--x.outstanding == 0 && x.fully_generated) _Release(unit);
}

void LoopFreeUnits::_Release( int unit )
{
  Unit & x = _units[unit];
  _graphs[x.pool].Remove(*x.turns);
  x.live = false;
  _free.push_back(unit);
  --_live;
  ++_releases;
}

long long LoopFreeUnits::LiveRefs( ) const
{
  long long refs = 0;
  for(size_t p = 0; p < _graphs.size(); ++p) refs += _graphs[p].TotalRefs();
  return refs;
}

string LoopFreeUnits::Check( ) const
{
  ostringstream err;
  vector<long long> expect(_graphs.size(), 0);
  long long live = 0;
  for(size_t u = 0; u < _units.size(); ++u) {
    if(!_units[u].live) continue;
    expect[_units[u].pool] += (long long)_units[u].turns->size();
    ++live;
  }
  if(live != _live) {
    err << live << " live units found, " << _live << " counted. ";
  }
  for(size_t p = 0; p < _graphs.size(); ++p) {
    if(!_graphs[p].IsAcyclic()) {
      err << "pool " << p << " pass-through graph has a cycle. ";
    }
    if(_graphs[p].TotalRefs() != expect[p]) {
      err << "pool " << p << " holds " << _graphs[p].TotalRefs()
          << " turn references, live units account for " << expect[p] << ". ";
    }
  }
  return err.str();
}
