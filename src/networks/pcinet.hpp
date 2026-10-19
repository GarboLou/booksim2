// $Id$

/*
 Copyright (c) 2007-2015, Trustees of The Leland Stanford Junior University
 Copyright (c) 2026, Jiaqi Lou
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

 Redistributions of source code must retain the above copyright notice, this 
 list of conditions and the following disclaimer.
 Redistributions in binary form must reproduce the above copyright notice, this
 list of conditions and the following disclaimer in the documentation and/or
 other materials provided with the distribution.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef _PCINET_HPP_
#define _PCINET_HPP_

#include "network.hpp"
#include "routefunc.hpp"
#include "turn_graph.hpp"
#include <cassert>
#include <string>
#include <map>
#include <vector>
#include <list>

enum PCIePortType {
  PORT_UPSTREAM = 0,
  PORT_DOWNSTREAM,
  PORT_NTB,
  PORT_GENERIC
};

enum PCIeLinkType {
  LINK_TRANSPARENT = 0,
  LINK_NON_TRANSPARENT
};

struct PCIeLinkInfo {
  int port;
  int latency;
  // Link bandwidth as cycles occupied per flit. 1 == the reference full-rate
  // link (by default PCIe Gen5 x16 at a 64 B flit and a 1 ns cycle); 2 == half
  // that rate, and so on. See PCINet::_ComputeSize for the unit system.
  int cycles_per_flit;
  PCIePortType port_type;
  PCIeLinkType link_type;
  PCIeLinkInfo() : port(-1), latency(1), cycles_per_flit(1),
                   port_type(PORT_GENERIC), link_type(LINK_TRANSPARENT) {}
  PCIeLinkInfo(int p, int l, int cpf, PCIePortType pt, PCIeLinkType lt)
    : port(p), latency(l), cycles_per_flit(cpf), port_type(pt), link_type(lt) {}
};

// One end-to-end shortest path between two routers. A sender pins a packet to
// one of these (Flit::path_key) and every switch on it forwards accordingly.
// Kept as router and port sequences rather than router pairs so parallel links
// between one switch pair would still give distinct paths.
struct PCIePath {
  vector<int> routers;  // routers[0] is the source router, back() the destination
  vector<int> ports;    // ports[i]: output port at routers[i] toward routers[i+1]
  // Pass-throughs (turns): (in channel, out channel) at every intermediate
  // router, as router-to-router channel ids (PCINet::ChannelId). These are
  // exactly the channel dependency graph edges this path can create.
  vector<pair<int, int> > turns;
  // Slowest router-to-router link on the path, in cycles per flit (1 if the
  // path has no such link). multipath_select = weighted uses 1 / this.
  int bottleneck_cpf;
  // Is this a shortest path between its end routers? Only
  // pcinet_cross_domain_paths = per_bridge produces paths that are not.
  bool minimal;
  PCIePath() : bottleneck_cpf(1), minimal(true) {}
};

class PCINet : public Network {

  string file_name;
  // Bytes carried by one BookSim flit. Sets the cycles-per-flit scale for every
  // link in the topology file; read from the "flit_bytes" config option.
  int flit_bytes;
  // association between nodes and routers
  map<int, int> node_list;
  // domain mapping
  map<int, int> node_domain;
  map<int, int> router_domain;
  
  // [0: node links, 1: router links][src router][dest router or node] = PCIeLinkInfo
  vector<map<int, map<int, PCIeLinkInfo> > > router_list;
  
  // stores multipath routing information: [router][dest_node] = vector of candidate output ports
  vector<map<int, vector<int> > > routing_table;

  // [src router][dst router] shortest-path latency, INT_MAX if unreachable.
  vector<vector<int> > router_dist;
  // [src router][dst router] the most router-to-router hops any shortest path
  // takes, -1 if unreachable. Strictly decreases along every path a routing
  // function can pick, which is what makes it a deadlock-free VC class.
  vector<vector<int> > hops_left;
  // [router][output port] -> router that port leads to, -1 for a node port.
  vector<vector<int> > port_next_router;
  // [node] -> router it is attached to.
  vector<int> node_router;
  // Distance classes the VC space is split into. 1 == no split.
  int vc_classes;
  // pcinet_deadlock_avoidance = loopfree: the flow traffic manager keeps the
  // in-flight pass-throughs acyclic instead of splitting VCs.
  bool loopfree;

  // [router][output port] -> dense id of that router-to-router channel, -1
  // for a node port. Ids run 0..num_router_channels-1.
  vector<vector<int> > port_channel;
  int num_router_channels;
  // [node] -> output port that ejects to it at its router.
  vector<int> node_port;
  // [src router][dst router] -> every shortest path, in deterministic
  // neighbor order, capped at max_paths_per_pair.
  vector<vector<vector<PCIePath> > > paths;
  int max_paths_per_pair;
  // pcinet_cross_domain_paths = per_bridge: a pair in different domains
  // (sockets) gets the shortest route through each bridge between the
  // domains (UPI or NTB), crossing exactly once; a pair in one domain gets
  // its shortest routes inside that domain.
  bool per_bridge_paths;
  // pcinet_min_tiebreak = cross_first: among equal-length next hops toward a
  // node in another domain, those already in the destination's domain come
  // first, so min crosses sockets at the earliest hop.
  bool cross_first;
  bool nonminimal_paths;            // some enumerated path is not shortest
  vector<int> router_dom;           // [router] -> domain
  vector<vector<int> > intra_dist;  // [s][t] latency inside dom(s), INT_MAX if none
  const LoopFreeUnits * lf_verify;

  void _ComputeSize( const Configuration &config );
  void _BuildNet( const Configuration &config );
  void readFile();
  void buildRoutingTable();
  void computeDistances();
  void route(int r_start);
  void enumeratePaths();
  void enumerateFrom(int r, int t, PCIePath & cur, vector<PCIePath> & out,
                     bool & truncated, const vector<vector<int> > & dist,
                     int domain) const;
  void computeIntraDomainDistances();
  void enumerateCrossDomain(int s, int t, vector<PCIePath> & out, bool & truncated) const;
  void finishPath(int s, int t, PCIePath & p) const;
  void setupDeadlockAvoidance( const Configuration &config );

public:
  PCINet( const Configuration &config, const string & name );
  ~PCINet();

  // VC class for a packet to dest_node leaving router r through out_port, or
  // -1 when the hop needs none (no split configured, or the port ejects).
  int VcClass( int r, int out_port, int dest_node ) const {
    if(vc_classes <= 1) return -1;
    int next = port_next_router[r][out_port];
    if(next < 0) return -1;
    return hops_left[next][node_router[dest_node]];
  }
  int NumVcClasses( ) const { return vc_classes; }
  bool LoopFree( ) const { return loopfree; }

  // Topology queries for pinned multipath (routes chosen by the source).
  int NodeRouter( int node ) const { return node_router[node]; }
  int NodePort( int node ) const { return node_port[node]; }
  // Router the given output port leads to, -1 for a node port.
  int NextRouter( int r, int port ) const { return port_next_router[r][port]; }
  int RouterDistance( int s, int t ) const { return router_dist[s][t]; }
  int RouterDomain( int r ) const { return router_dom[r]; }
  bool PerBridgePaths( ) const { return per_bridge_paths; }
  int NumRouters( ) const { return _size; }
  int NumRouterChannels( ) const { return num_router_channels; }
  int ChannelId( int r, int port ) const { return port_channel[r][port]; }
  int NumPaths( int src_node, int dst_node ) const {
    return (int)paths[node_router[src_node]][node_router[dst_node]].size();
  }
  // The path a packet with this key takes: key modulo the path count.
  const PCIePath & GetPath( int src_node, int dst_node, int key ) const;
  // Output port at router r for a head flit pinned with f->path_key. Aborts
  // if r is not on the flit's path: that is a bug, never a fallback.
  int PinnedOutputPort( int r, const Flit * f ) const;

  // Debug (loopfree_check_interval > 0): every pass-through a pinned packet
  // makes must belong to a live, registered loop-free unit.
  void SetLoopFreeVerifier( const LoopFreeUnits * lf ) { lf_verify = lf; }

  // Does this routing function's channel dependency graph have a cycle?
  bool dependencyGraphIsCyclic( const string & routing ) const;

  int GetN( ) const { return -1; }
  int GetK( ) const { return -1; }

  static void RegisterRoutingFunctions();
  double Capacity( ) const { return -1; }
  void InsertRandomFaults( const Configuration &config ) {}
};

void min_pcinet( const Router *r, const Flit *f, int in_channel, 
                 OutputSet *outputs, bool inject );

void ecmp_pcinet( const Router *r, const Flit *f, int in_channel, 
                  OutputSet *outputs, bool inject );

void adaptive_pcinet( const Router *r, const Flit *f, int in_channel, 
                      OutputSet *outputs, bool inject );

void pinned_pcinet( const Router *r, const Flit *f, int in_channel,
                    OutputSet *outputs, bool inject );

#endif
