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

/* pcinet
 *
 * Network setup file format:
 *
 * Node attachments:
 * router <r_id> node <n_id> [port_type <upstream|downstream|ntb>] [domain <d_id>] [lat <l>]
 * Example:
 * router 0 node 0 port_type downstream domain 0 lat 1
 *
 * Router-to-router links:
 * router <src_r> router <dst_r> [port_type <upstream|downstream|ntb>] [link_type <transparent|nontransparent>] [domain <d_id>] [lat <l>]
 * Example:
 * router 0 router 1 port_type downstream link_type transparent lat 2
 * router 0 router 2 port_type ntb link_type nontransparent lat 5
 *
 * Also supports backward-compatible simple anynet format:
 * router 0 node 0 node 1 5
 * router 0 router 1 10 router 2
 */

#include "pcinet.hpp"
#include <fstream>
#include <sstream>
#include <limits>
#include <algorithm>
#include <queue>
#include <set>
#include <iostream>
#include <cctype>

// Global pointer to the multipath routing table [router_id][dest_node] = vector<int> candidate ports
static vector<map<int, vector<int> > >* global_pcinet_routing_table = NULL;
// The network itself, for the VC distance classes. BookSim runs one network
// per process, the same assumption the routing-table pointer already makes.
static const PCINet * global_pcinet = NULL;

PCINet::PCINet( const Configuration &config, const string & name )
  : Network( config, name ), vc_classes(1), loopfree(false),
    num_router_channels(0), max_paths_per_pair(0), per_bridge_paths(false),
    cross_first(false), nonminimal_paths(false), lf_verify(NULL) {

  router_list.resize(2);
  _ComputeSize( config );
  _Alloc( );
  _BuildNet( config );
}

PCINet::~PCINet() {
  if(global_pcinet == this) {
    global_pcinet = NULL;
    global_pcinet_routing_table = NULL;
  }
  for(int i = 0; i < 2; ++i) {
    for(map<int, map<int, PCIeLinkInfo> >::iterator iter = router_list[i].begin();
        iter != router_list[i].end();
        ++iter) {
      iter->second.clear();
    }
  }
}

void PCINet::_ComputeSize( const Configuration &config ) {
  // Unit system: one flit is flit_bytes bytes and one cycle is one ns, so a
  // link running at N bytes/ns costs flit_bytes/N cycles per flit. With the
  // defaults (64 B, 1 ns) PCIe Gen5 x16 comes out at 1 flit/cycle.
  flit_bytes = config.GetInt("flit_bytes");
  if(flit_bytes <= 0) {
    cout << "PCINet: flit_bytes must be positive" << endl;
    exit(-1);
  }

  file_name = config.GetStr("network_file");
  if(file_name == "") {
    cout << "PCINet: No network file name provided" << endl;
    exit(-1);
  }
  
  readFile();

  _channels = 0;
  cout << "======================== PCINet File Parsed =================\n";
  cout << "****************** Node listing **********************\n";
  for(map<int, int>::iterator iter = node_list.begin(); iter != node_list.end(); iter++) {
    cout << "Node " << iter->first 
         << "\tRouter " << iter->second 
         << "\tDomain " << node_domain[iter->first] << endl;
  }

  cout << "\n**************** Router to Node listing *************\n";
  for(map<int, map<int, PCIeLinkInfo> >::iterator iter3 = router_list[0].begin(); iter3 != router_list[0].end(); iter3++) {
    cout << "Router " << iter3->first << " (Domain " << router_domain[iter3->first] << ")" << endl;
    for(map<int, PCIeLinkInfo>::iterator iter2 = iter3->second.begin(); iter2 != iter3->second.end(); iter2++) {
      cout << "\t Node " << iter2->first 
           << " lat " << iter2->second.latency 
           << " cpf " << iter2->second.cycles_per_flit
           << " port_type " << iter2->second.port_type << endl;
    }
  }

  cout << "\n***************** Router to Router listing ************\n";
  for(map<int, map<int, PCIeLinkInfo> >::iterator iter3 = router_list[1].begin(); iter3 != router_list[1].end(); iter3++) {
    cout << "Router " << iter3->first << endl;
    if(iter3->second.size() == 0) {
      cout << "Caution Router " << iter3->first << " is not connected to any other Router\n" << endl;
    }
    for(map<int, PCIeLinkInfo>::iterator iter2 = iter3->second.begin(); iter2 != iter3->second.end(); iter2++) {
      cout << "\t Router " << iter2->first 
           << " lat " << iter2->second.latency 
           << " port_type " << iter2->second.port_type 
           << " cpf " << iter2->second.cycles_per_flit
           << " link_type " << (iter2->second.link_type == LINK_TRANSPARENT ? "transparent" : "nontransparent") << endl;
      _channels++;
    }
  }

  _size = router_list[1].size();
  _nodes = node_list.size();
}

void PCINet::_BuildNet( const Configuration &config ) {
  // A fixed switch delay on every data channel into a switch, but not on its
  // credit channel: it then sits once in the upstream credit loop, as a real
  // switch's forwarding delay does. Routing still weighs links by `lat` only.
  const int switch_latency = config.GetInt("pcinet_switch_latency");
  if(switch_latency < 0) {
    cout << "PCINet: pcinet_switch_latency must be non-negative" << endl;
    exit(-1);
  }

  int * outport = (int*)malloc(sizeof(int)*_size);
  for(int i = 0; i < _size; i++) { outport[i] = 0; }

  cout << "========================== Node to Router =====================\n";
  for(map<int, map<int, PCIeLinkInfo> >::iterator niter = router_list[0].begin(); niter != router_list[0].end(); niter++) {
    map<int, map<int, PCIeLinkInfo> >::iterator riter = router_list[1].find(niter->first);
    int radix = niter->second.size() + riter->second.size();
    int node = niter->first;
    cout << "router " << node << " radix " << radix << endl;
    
    ostringstream router_name;
    router_name << "router_" << node;
    _routers[node] = Router::NewRouter( config, this, router_name.str( ), node, radix, radix );
    _timed_modules.push_back(_routers[node]);

    for(map<int, PCIeLinkInfo>::iterator nniter = niter->second.begin(); nniter != niter->second.end(); nniter++) {
      int link = nniter->first;
      (niter->second)[link].port = outport[node];
      outport[node]++;
      cout << "\t connected to node " << link << " at outport " << nniter->second.port
           << " lat " << nniter->second.latency << endl;
      
      _inject[link]->SetLatency(nniter->second.latency + switch_latency);
      _inject_cred[link]->SetLatency(nniter->second.latency);
      _eject[link]->SetLatency(nniter->second.latency);
      _eject_cred[link]->SetLatency(nniter->second.latency);
      // Rate-limit the endpoint links; credit channels stay at one per
      // cycle because a PCIe credit is a DLLP, not a data payload.
      _inject[link]->SetCyclesPerFlit(nniter->second.cycles_per_flit);
      _eject[link]->SetCyclesPerFlit(nniter->second.cycles_per_flit);

      _routers[node]->AddInputChannel( _inject[link], _inject_cred[link] );
      _routers[node]->AddOutputChannel( _eject[link], _eject_cred[link] );
    }
  }

  cout << "========================== Router to Router =====================\n";
  int channel_count = 0; 
  for(map<int, map<int, PCIeLinkInfo> >::iterator niter = router_list[0].begin(); niter != router_list[0].end(); niter++) {
    map<int, map<int, PCIeLinkInfo> >::iterator riter = router_list[1].find(niter->first);
    int node = niter->first;
    cout << "router " << node << endl;
    for(map<int, PCIeLinkInfo>::iterator rriter = riter->second.begin(); rriter != riter->second.end(); rriter++) {
      int other_node = rriter->first;
      int link = channel_count;
      (riter->second)[other_node].port = outport[node];
      outport[node]++;
      cout << "\t connected to router " << other_node << " using link " << link
           << " at outport " << rriter->second.port
           << " lat " << rriter->second.latency << endl;

      _chan[link]->SetLatency(rriter->second.latency + switch_latency);
      _chan_cred[link]->SetLatency(rriter->second.latency);
      _chan[link]->SetCyclesPerFlit(rriter->second.cycles_per_flit);

      _routers[node]->AddOutputChannel( _chan[link], _chan_cred[link] );
      _routers[other_node]->AddInputChannel( _chan[link], _chan_cred[link] );
      channel_count++;
    }
  }

  free(outport);

  // A rate-limited output channel makes flits queue in IQRouter::_output_buffer.
  // With output_buffer_size == -1 (the default) all four backpressure checks in
  // iq_router.cpp are disabled, so that queue grows until the downstream VC
  // credit pool runs dry: throughput still looks right but latency is silently
  // wrong. Require a finite buffer whenever any link is derated.
  bool derated = false;
  for(int lt = 0; lt < 2 && !derated; ++lt) {
    for(map<int, map<int, PCIeLinkInfo> >::iterator ri = router_list[lt].begin();
        ri != router_list[lt].end() && !derated; ++ri) {
      for(map<int, PCIeLinkInfo>::iterator li = ri->second.begin();
          li != ri->second.end(); ++li) {
        if(li->second.cycles_per_flit > 1) { derated = true; break; }
      }
    }
  }
  if(derated && config.GetInt("output_buffer_size") <= 0) {
    cout << "PCINet: this topology has links slower than 1 flit/cycle, which "
         << "requires a finite output_buffer_size (currently "
         << config.GetInt("output_buffer_size") << "). Set e.g. "
         << "output_buffer_size = 4; in the config." << endl;
    exit(-1);
  }

  max_paths_per_pair = config.GetInt("max_paths_per_pair");
  if(max_paths_per_pair < 1) {
    cout << "PCINet: max_paths_per_pair must be at least 1" << endl;
    exit(-1);
  }

  const string xdom = config.GetStr("pcinet_cross_domain_paths");
  if(xdom == "per_bridge") {
    per_bridge_paths = true;
    if(config.GetStr("routing_function") != "pinned") {
      cout << "PCINet: pcinet_cross_domain_paths = per_bridge needs routing_function "
           << "= pinned: only a sender-chosen path can take a route that is not "
           << "shortest (got " << config.GetStr("routing_function") << ")" << endl;
      exit(-1);
    }
  } else if(xdom != "shortest") {
    cout << "PCINet: pcinet_cross_domain_paths must be shortest or per_bridge (got "
         << xdom << ")" << endl;
    exit(-1);
  }

  const string tiebreak = config.GetStr("pcinet_min_tiebreak");
  if(tiebreak == "cross_first") {
    cross_first = true;
    if(config.GetStr("routing_function") != "min") {
      cout << "PCINet: pcinet_min_tiebreak = cross_first orders the candidates "
           << "routing_function = min chooses from (got "
           << config.GetStr("routing_function") << ")" << endl;
      exit(-1);
    }
  } else if(tiebreak != "port") {
    cout << "PCINet: pcinet_min_tiebreak must be port or cross_first (got "
         << tiebreak << ")" << endl;
    exit(-1);
  }

  buildRoutingTable();
  setupDeadlockAvoidance(config);
}

void PCINet::RegisterRoutingFunctions() {
  gRoutingFunctionMap["min_pcinet"] = &min_pcinet;
  gRoutingFunctionMap["ecmp_pcinet"] = &ecmp_pcinet;
  gRoutingFunctionMap["adaptive_pcinet"] = &adaptive_pcinet;
  gRoutingFunctionMap["pinned_pcinet"] = &pinned_pcinet;
}

// Narrow [vcBegin, vcEnd] to the slice owned by distance class cls of classes.
// Class 0 (the last router-to-router hop, which every multi-switch packet
// takes) gets any remainder VCs.
static void narrow_to_vc_class( int & vcBegin, int & vcEnd, int cls, int classes ) {
  int span = vcEnd - vcBegin + 1;
  if(span < classes) {
    cout << "PCINet: deadlock avoidance needs " << classes << " VCs per "
         << "traffic type, but VC range [" << vcBegin << "," << vcEnd
         << "] has only " << span << ". Raise num_vcs (and widen the "
         << "*_begin_vc/*_end_vc ranges if P/NP/Cpl classes are in use)." << endl;
    exit(-1);
  }
  int per = span / classes;
  int rem = span % classes;
  vcBegin += cls * per + min(cls, rem);
  vcEnd = vcBegin + per + (cls < rem ? 1 : 0) - 1;
}

static void apply_vc_range( const Router *r, const Flit *f, OutputSet *outputs,
                            int out_port ) {
  int vcBegin = 0, vcEnd = gNumVCs - 1;
  if ( f->type == Flit::READ_REQUEST ) {
    vcBegin = gReadReqBeginVC;
    vcEnd   = gReadReqEndVC;
  } else if ( f->type == Flit::WRITE_REQUEST ) {
    vcBegin = gWriteReqBeginVC;
    vcEnd   = gWriteReqEndVC;
  } else if ( f->type ==  Flit::READ_REPLY ) {
    vcBegin = gReadReplyBeginVC;
    vcEnd   = gReadReplyEndVC;
  } else if ( f->type ==  Flit::WRITE_REPLY ) {
    vcBegin = gWriteReplyBeginVC;
    vcEnd   = gWriteReplyEndVC;
  }

  // Injection (out_port == -1) and ejection hops keep the whole range: neither
  // can be part of a dependency cycle.
  if(out_port >= 0 && global_pcinet != NULL) {
    int cls = global_pcinet->VcClass(r->GetID(), out_port, f->dest);
    if(cls >= 0) {
      narrow_to_vc_class(vcBegin, vcEnd, cls, global_pcinet->NumVcClasses());
    }
  }

  outputs->Clear( );
  outputs->AddRange( out_port, vcBegin, vcEnd );
}

void min_pcinet( const Router *r, const Flit *f, int in_channel,
                 OutputSet *outputs, bool inject ) {
  int out_port = -1;
  if(!inject) {
    assert(global_pcinet_routing_table != NULL);
    assert((*global_pcinet_routing_table)[r->GetID()].count(f->dest) != 0);
    const vector<int> &cand = (*global_pcinet_routing_table)[r->GetID()][f->dest];
    assert(!cand.empty());
    out_port = cand[0]; // Primary / first shortest path
  }
  apply_vc_range(r, f, outputs, out_port);
}

void ecmp_pcinet( const Router *r, const Flit *f, int in_channel, 
                  OutputSet *outputs, bool inject ) {
  int out_port = -1;
  if(!inject) {
    assert(global_pcinet_routing_table != NULL);
    assert((*global_pcinet_routing_table)[r->GetID()].count(f->dest) != 0);
    const vector<int> &cand = (*global_pcinet_routing_table)[r->GetID()][f->dest];
    assert(!cand.empty());
    // Packet-consistent hashing across equal-cost candidate paths
    out_port = cand[f->pid % cand.size()];
  }
  apply_vc_range(r, f, outputs, out_port);
}

void adaptive_pcinet( const Router *r, const Flit *f, int in_channel, 
                      OutputSet *outputs, bool inject ) {
  int out_port = -1;
  if(!inject) {
    assert(global_pcinet_routing_table != NULL);
    assert((*global_pcinet_routing_table)[r->GetID()].count(f->dest) != 0);
    const vector<int> &cand = (*global_pcinet_routing_table)[r->GetID()][f->dest];
    assert(!cand.empty());
    if (cand.size() == 1) {
      out_port = cand[0];
    } else {
      // Pick the least congested candidate. Router::GetUsedCredit() returns the
      // downstream buffer's occupancy (iq_router.cpp: dest_buf->Occupancy()),
      // i.e. credits consumed -- so the least congested port is the minimum.
      // Taking the maximum here steered every packet at the most backed-up port.
      int min_used = numeric_limits<int>::max();
      int best_port = cand[0];
      for (size_t i = 0; i < cand.size(); ++i) {
        int p = cand[i];
        int used = r->GetUsedCredit(p);
        if (used < min_used) {
          min_used = used;
          best_port = p;
        }
      }
      out_port = best_port;
    }
  }
  apply_vc_range(r, f, outputs, out_port);
}

// The sender chose the path (Flit::path_key); every switch just follows it.
void pinned_pcinet( const Router *r, const Flit *f, int in_channel,
                    OutputSet *outputs, bool inject ) {
  int out_port = -1;
  if(!inject) {
    assert(global_pcinet != NULL);
    out_port = global_pcinet->PinnedOutputPort(r->GetID(), f);
  }
  apply_vc_range(r, f, outputs, out_port);
}

const PCIePath & PCINet::GetPath( int src_node, int dst_node, int key ) const {
  const vector<PCIePath> & P = paths[node_router[src_node]][node_router[dst_node]];
  if(P.empty() || key < 0) {
    cout << "PCINet: no path from node " << src_node << " to node " << dst_node
         << " (key " << key << ")" << endl;
    exit(-1);
  }
  return P[key % P.size()];
}

int PCINet::PinnedOutputPort( int r, const Flit * f ) const {
  if(f->path_key < 0) {
    cout << "PCINet: routing_function = pinned needs every packet to carry a "
         << "path key, which only the flow traffic manager (sim_type = flow) "
         << "assigns; packet " << f->pid << " has none." << endl;
    exit(-1);
  }
  const PCIePath & p = GetPath(f->src, f->dest, f->path_key);
  for(size_t i = 0; i < p.routers.size(); ++i) {
    if(p.routers[i] == r) {
      // A pass-through: the packet reaches r from routers[i-1] and leaves
      // toward routers[i+1]. Under loopfree it must still be registered.
      if(lf_verify && i >= 1 && i < p.ports.size() &&
         !lf_verify->Holds(f->lf_unit, &p.turns)) {
        cout << "PCINet: loopfree violation: packet " << f->pid << " ("
             << f->src << "->" << f->dest << ") passes through router " << r
             << " without a registered unit (lf_unit " << f->lf_unit << ")" << endl;
        exit(-1);
      }
      return (i < p.ports.size()) ? p.ports[i] : node_port[f->dest];
    }
  }
  // A hard check rather than assert(): an embedding build may define NDEBUG,
  // and a packet off its pinned path would silently corrupt every result.
  cout << "PCINet: path-consistency violation: packet " << f->pid << " ("
       << f->src << "->" << f->dest << ", key " << f->path_key
       << ") is at router " << r << ", which is not on its path [";
  for(size_t i = 0; i < p.routers.size(); ++i) {
    cout << (i ? " " : "") << p.routers[i];
  }
  cout << "]" << endl;
  exit(-1);
  return -1;
}

void PCINet::buildRoutingTable() {
  cout << "========================== PCINet Routing Table Generation =====================\n";

  // Port maps for the routing functions. Ports were assigned in _BuildNet.
  node_router.assign(_nodes, -1);
  for(map<int, int>::iterator ni = node_list.begin(); ni != node_list.end(); ++ni) {
    node_router[ni->first] = ni->second;
  }
  port_next_router.assign(_size, vector<int>());
  port_channel.assign(_size, vector<int>());
  num_router_channels = 0;
  for(int r = 0; r < _size; ++r) {
    port_next_router[r].assign(router_list[0][r].size() + router_list[1][r].size(), -1);
    port_channel[r].assign(port_next_router[r].size(), -1);
    for(map<int, PCIeLinkInfo>::iterator li = router_list[1][r].begin();
        li != router_list[1][r].end(); ++li) {
      port_next_router[r][li->second.port] = li->first;
    }
    for(size_t p = 0; p < port_next_router[r].size(); ++p) {
      if(port_next_router[r][p] >= 0) port_channel[r][p] = num_router_channels++;
    }
  }
  node_port.assign(_nodes, -1);
  for(int r = 0; r < _size; ++r) {
    for(map<int, PCIeLinkInfo>::iterator li = router_list[0][r].begin();
        li != router_list[0][r].end(); ++li) {
      node_port[li->first] = li->second.port;
    }
  }

  computeDistances();

  routing_table.resize(_size);
  for(int i = 0; i < _size; i++) {
    route(i);
  }
  enumeratePaths();
  global_pcinet_routing_table = &routing_table;
  global_pcinet = this;
}

// Depth-first over next hops that stay on a shortest path to t, in
// neighbor-map order, so the path order (and hence what a path key selects)
// is deterministic.
void PCINet::enumerateFrom( int r, int t, PCIePath & cur, vector<PCIePath> & out,
                            bool & truncated, const vector<vector<int> > & dist,
                            int domain ) const {
  if(r == t) {
    if((int)out.size() >= max_paths_per_pair) {
      truncated = true;
      return;
    }
    out.push_back(cur);
    return;
  }
  const map<int, PCIeLinkInfo> & nbrs = router_list[1].find(r)->second;
  for(map<int, PCIeLinkInfo>::const_iterator li = nbrs.begin(); li != nbrs.end(); ++li) {
    int n = li->first;
    if(domain >= 0 && router_dom[n] != domain) continue;
    if(dist[n][t] == numeric_limits<int>::max() ||
       li->second.latency + dist[n][t] != dist[r][t]) {
      continue;
    }
    cur.ports.push_back(li->second.port);
    cur.routers.push_back(n);
    enumerateFrom(n, t, cur, out, truncated, dist, domain);
    cur.routers.pop_back();
    cur.ports.pop_back();
    if(truncated) return;
  }
}

// Only between routers that have nodes attached: no packet starts or ends
// anywhere else, and paths between, say, two spines would add pass-throughs
// no packet can make -- enough to make an acyclic spine-leaf look cyclic.
void PCINet::enumeratePaths() {
  const int inf = numeric_limits<int>::max();
  router_dom.assign(_size, 0);
  for(int r = 0; r < _size; ++r) {
    map<int, int>::const_iterator di = router_domain.find(r);
    if(di != router_domain.end()) router_dom[r] = di->second;
  }
  if(per_bridge_paths) computeIntraDomainDistances();

  paths.assign(_size, vector<vector<PCIePath> >(_size));
  size_t pairs = 0, total = 0, most = 0, unsupported = 0;
  pair<int, int> first_unsupported(-1, -1);
  for(int s = 0; s < _size; ++s) {
    for(int t = 0; t < _size; ++t) {
      if(router_dist[s][t] == inf) continue;
      if(router_list[0][s].empty() || router_list[0][t].empty()) continue;
      bool truncated = false;
      if(per_bridge_paths) {
        // The policy holds per pair, with no fallback: a pair that no single
        // bridge connects, or a same-domain pair with no route inside its
        // domain, gets no path, and InjectFlow rejects pinned flows for it.
        if(router_dom[s] != router_dom[t]) {
          enumerateCrossDomain(s, t, paths[s][t], truncated);
        } else if(intra_dist[s][t] != inf) {
          PCIePath cur;
          cur.routers.push_back(s);
          enumerateFrom(s, t, cur, paths[s][t], truncated, intra_dist, router_dom[s]);
        }
        if(paths[s][t].empty()) {
          if(unsupported == 0) {
            first_unsupported = make_pair(s, t);
          }
          ++unsupported;
          continue;
        }
      } else {
        PCIePath cur;
        cur.routers.push_back(s);
        enumerateFrom(s, t, cur, paths[s][t], truncated, router_dist, -1);
      }
      if(truncated) {
        cout << "PCINet: WARNING: router " << s << " -> " << t << " has more than "
             << max_paths_per_pair << " paths; keeping the first "
             << max_paths_per_pair << " (max_paths_per_pair)" << endl;
      }
      for(size_t k = 0; k < paths[s][t].size(); ++k) {
        finishPath(s, t, paths[s][t][k]);
        if(!paths[s][t][k].minimal) nonminimal_paths = true;
      }
      ++pairs;
      total += paths[s][t].size();
      most = max(most, paths[s][t].size());
    }
  }
  if(unsupported > 0) {
    cout << "PCINet: per_bridge: " << unsupported << " router pair(s) have no "
         << "route under the policy (one bridge crossing between domains, none "
         << "inside one), e.g. router " << first_unsupported.first << " -> "
         << first_unsupported.second << "; pinned flows between their nodes "
         << "are rejected" << endl;
  }
  cout << "PCINet: enumerated " << total << (per_bridge_paths ? " per-bridge" : " shortest")
       << " paths over " << pairs
       << " router pairs (max " << most << ", mean "
       << (pairs ? (double)total / (double)pairs : 0.0) << " per pair)" << endl;
}

// Turns (pass-throughs), the slowest router-to-router link, and whether the
// path is a shortest one.
void PCINet::finishPath( int s, int t, PCIePath & p ) const {
  assert(p.ports.size() + 1 == p.routers.size());
  assert(p.routers.front() == s && p.routers.back() == t);
  p.turns.clear();
  for(size_t i = 1; i < p.ports.size(); ++i) {
    int in_ch  = port_channel[p.routers[i-1]][p.ports[i-1]];
    int out_ch = port_channel[p.routers[i]][p.ports[i]];
    assert(in_ch >= 0 && out_ch >= 0);
    p.turns.push_back(make_pair(in_ch, out_ch));
  }
  int lat = 0;
  p.bottleneck_cpf = 1;
  for(size_t i = 0; i < p.ports.size(); ++i) {
    const PCIeLinkInfo & li = router_list[1].find(p.routers[i])->second.find(p.routers[i+1])->second;
    lat += li.latency;
    p.bottleneck_cpf = max(p.bottleneck_cpf, li.cycles_per_flit);
  }
  p.minimal = (lat == router_dist[s][t]);
}

// Shortest latency between routers of one domain using only links inside it.
void PCINet::computeIntraDomainDistances() {
  const int inf = numeric_limits<int>::max();
  intra_dist.assign(_size, vector<int>(_size, inf));
  for(int src = 0; src < _size; ++src) {
    vector<int> & dist = intra_dist[src];
    const int dom = router_dom[src];
    dist[src] = 0;
    priority_queue<pair<int, int>, vector<pair<int, int> >, greater<pair<int, int> > > pq;
    pq.push(make_pair(0, src));
    while(!pq.empty()) {
      int d = pq.top().first;
      int u = pq.top().second;
      pq.pop();
      if(d > dist[u]) continue;
      for(map<int, PCIeLinkInfo>::const_iterator li = router_list[1][u].begin();
          li != router_list[1][u].end(); ++li) {
        int v = li->first;
        if(router_dom[v] != dom) continue;
        int nd = d + li->second.latency;
        if(nd < dist[v]) {
          dist[v] = nd;
          pq.push(make_pair(nd, v));
        }
      }
    }
  }
}

// s and t in different domains: for each bridge (a -> b) with a in s's domain
// and b in t's, every shortest route s -> a inside s's domain, the bridge, then
// every shortest route b -> t inside t's domain. Bridges in order of total
// latency (ties by router id), so key 0 is the shortest such route.
void PCINet::enumerateCrossDomain( int s, int t, vector<PCIePath> & out,
                                   bool & truncated ) const {
  const int inf = numeric_limits<int>::max();
  vector<pair<int, pair<int, int> > > bridges;   // (latency, (a, b))
  for(int a = 0; a < _size; ++a) {
    if(router_dom[a] != router_dom[s] || intra_dist[s][a] == inf) continue;
    const map<int, PCIeLinkInfo> & nbrs = router_list[1].find(a)->second;
    for(map<int, PCIeLinkInfo>::const_iterator li = nbrs.begin(); li != nbrs.end(); ++li) {
      int b = li->first;
      if(router_dom[b] != router_dom[t] || intra_dist[b][t] == inf) continue;
      bridges.push_back(make_pair(intra_dist[s][a] + li->second.latency + intra_dist[b][t],
                                  make_pair(a, b)));
    }
  }
  sort(bridges.begin(), bridges.end());
  for(size_t bi = 0; bi < bridges.size() && !truncated; ++bi) {
    int a = bridges[bi].second.first, b = bridges[bi].second.second;
    vector<PCIePath> pre, suf;
    bool tp = false, ts = false;
    PCIePath cur;
    cur.routers.push_back(s);
    enumerateFrom(s, a, cur, pre, tp, intra_dist, router_dom[s]);
    cur = PCIePath();
    cur.routers.push_back(b);
    enumerateFrom(b, t, cur, suf, ts, intra_dist, router_dom[t]);
    const int bridge_port = router_list[1].find(a)->second.find(b)->second.port;
    for(size_t i = 0; i < pre.size() && !truncated; ++i) {
      for(size_t j = 0; j < suf.size(); ++j) {
        if((int)out.size() >= max_paths_per_pair) { truncated = true; break; }
        PCIePath p = pre[i];
        p.ports.push_back(bridge_port);
        p.routers.insert(p.routers.end(), suf[j].routers.begin(), suf[j].routers.end());
        p.ports.insert(p.ports.end(), suf[j].ports.begin(), suf[j].ports.end());
        out.push_back(p);
      }
    }
    if(tp || ts) truncated = true;
  }
}

// All-pairs shortest-path latency (one Dijkstra per router), then, per
// destination, the longest hop count among the shortest paths to it.
void PCINet::computeDistances() {
  const int inf = numeric_limits<int>::max();
  router_dist.assign(_size, vector<int>(_size, inf));

  for(int src = 0; src < _size; ++src) {
    vector<int> & dist = router_dist[src];
    dist[src] = 0;

    // Min-heap for Dijkstra: pair<distance, router_id>
    priority_queue<pair<int, int>, vector<pair<int, int> >, greater<pair<int, int> > > pq;
    pq.push(make_pair(0, src));

    while(!pq.empty()) {
      int d = pq.top().first;
      int u = pq.top().second;
      pq.pop();

      if(d > dist[u]) continue;

      for(map<int, PCIeLinkInfo>::iterator iter = router_list[1][u].begin();
          iter != router_list[1][u].end();
          iter++) {
        int v = iter->first;
        int new_dist = dist[u] + iter->second.latency;
        if(new_dist < dist[v]) {
          dist[v] = new_dist;
          pq.push(make_pair(new_dist, v));
        }
      }
    }
  }

  // Channel latencies are positive (Channel::SetLatency enforces it), so every
  // next hop on a shortest path is strictly closer to the target: visiting
  // routers in increasing distance computes each neighbor's value first.
  hops_left.assign(_size, vector<int>(_size, -1));
  for(int t = 0; t < _size; ++t) {
    vector<pair<int, int> > order;
    for(int r = 0; r < _size; ++r) {
      if(router_dist[r][t] != inf) order.push_back(make_pair(router_dist[r][t], r));
    }
    sort(order.begin(), order.end());
    for(size_t i = 0; i < order.size(); ++i) {
      int r = order[i].second;
      if(r == t) { hops_left[r][t] = 0; continue; }
      for(map<int, PCIeLinkInfo>::iterator li = router_list[1][r].begin();
          li != router_list[1][r].end(); ++li) {
        int n = li->first;
        if(router_dist[n][t] != inf &&
           li->second.latency + router_dist[n][t] == router_dist[r][t]) {
          hops_left[r][t] = max(hops_left[r][t], 1 + hops_left[n][t]);
        }
      }
    }
  }
}

// Extract all equal-cost candidate next-hops from the all-pairs distances
void PCINet::route(int r_start) {
  // First, handle locally attached nodes at r_start
  for(map<int, PCIeLinkInfo>::iterator iter = router_list[0][r_start].begin();
      iter != router_list[0][r_start].end();
      iter++) {
    routing_table[r_start][iter->first].push_back(iter->second.port);
  }

  const vector<int> & dist = router_dist[r_start];

  // Find all equal-cost candidate next hops from r_start toward each router target
  for(int target_router = 0; target_router < _size; target_router++) {
    if(target_router == r_start || dist[target_router] == numeric_limits<int>::max()) {
      continue;
    }

    // A neighbor 'nbr' of r_start is an equal-cost first hop toward target_router if:
    // dist(r_start, nbr) + dist(nbr, target_router) == dist(r_start, target_router)
    for(map<int, PCIeLinkInfo>::iterator nbr_iter = router_list[1][r_start].begin();
        nbr_iter != router_list[1][r_start].end();
        nbr_iter++) {
      int nbr = nbr_iter->first;
      int edge_lat = nbr_iter->second.latency;
      int port = nbr_iter->second.port;
      int nbr_to_target = router_dist[nbr][target_router];

      if(nbr_to_target != numeric_limits<int>::max() &&
         edge_lat + nbr_to_target == dist[target_router]) {
        // Valid equal-cost candidate next hop
        // Add candidate port for all destination nodes attached to target_router
        for(map<int, PCIeLinkInfo>::iterator node_iter = router_list[0][target_router].begin();
            node_iter != router_list[0][target_router].end();
            node_iter++) {
          int dest_node = node_iter->first;
          vector<int> &cand = routing_table[r_start][dest_node];
          if(find(cand.begin(), cand.end(), port) == cand.end()) {
            cand.push_back(port);
          }
        }
      }
    }
  }

  // min takes the first candidate, which is otherwise the lowest port (the
  // neighbor with the lowest router id). cross_first moves the next hops that
  // are already in the destination's domain to the front, keeping port order
  // within each group: e.g. in a 2x2 mesh whose two horizontal links both
  // cross sockets, each switch crosses at its own level, and the two bridges
  // share the diagonal traffic instead of the lower-id one taking it all.
  if(cross_first) {
    map<int, int>::const_iterator ri = router_domain.find(r_start);
    const int here = (ri == router_domain.end()) ? 0 : ri->second;
    for(map<int, vector<int> >::iterator it = routing_table[r_start].begin();
        it != routing_table[r_start].end(); ++it) {
      map<int, int>::const_iterator di = router_domain.find(node_list[it->first]);
      const int dest_dom = (di == router_domain.end()) ? 0 : di->second;
      if(dest_dom == here) continue;
      vector<int> first, rest;
      for(size_t p = 0; p < it->second.size(); ++p) {
        int nbr = port_next_router[r_start][it->second[p]];
        map<int, int>::const_iterator ni = router_domain.find(nbr);
        bool in_dest = nbr >= 0 && ni != router_domain.end() && ni->second == dest_dom;
        (in_dest ? first : rest).push_back(it->second[p]);
      }
      first.insert(first.end(), rest.begin(), rest.end());
      it->second = first;
    }
  }

  // Print summary of paths discovered
  for(map<int, vector<int> >::iterator it = routing_table[r_start].begin(); it != routing_table[r_start].end(); it++) {
    cout << "Router " << r_start << " -> Dest Node " << it->first << " candidate ports: [";
    for(size_t p = 0; p < it->second.size(); p++) {
      cout << (p > 0 ? ", " : "") << it->second[p];
    }
    cout << "]\n";
  }
}

// Does the routing function create a cyclic channel dependency graph? Vertices
// are router-to-router channels; an edge a->b means some packet may hold a
// buffer behind channel a while waiting for one behind channel b. Injection
// and ejection channels can never close a cycle and are left out. An acyclic
// graph is deadlock-free as is (Dally & Seitz).
bool PCINet::dependencyGraphIsCyclic( const string & routing ) const {
  // min only ever takes the first candidate; ecmp and adaptive may take any.
  const bool first_only = (routing == "min");

  // channel (r -> n) is vertex r * _size + n
  map<int, set<int> > edges;
  if(routing == "pinned") {
    // Exactly the pass-throughs of the enumerated paths, which is all a
    // pinned packet can ever do -- tighter than the all-candidates graph, and
    // exact once a path filter removes some paths.
    for(int s = 0; s < _size; ++s) {
      for(int t = 0; t < _size; ++t) {
        const vector<PCIePath> & P = paths[s][t];
        for(size_t k = 0; k < P.size(); ++k) {
          for(size_t i = 0; i < P[k].turns.size(); ++i) {
            edges[P[k].turns[i].first].insert(P[k].turns[i].second);
          }
        }
      }
    }
  }
  for(int r = 0; r < _size && routing != "pinned"; ++r) {
    for(map<int, vector<int> >::const_iterator di = routing_table[r].begin();
        di != routing_table[r].end(); ++di) {
      int dest = di->first;
      int t = node_router[dest];
      const vector<int> & cand = di->second;
      size_t nc = first_only ? 1 : cand.size();
      for(size_t i = 0; i < nc; ++i) {
        int n = port_next_router[r][cand[i]];
        if(n < 0 || n == t) continue;   // ejects here, or ejects at the next router
        map<int, vector<int> >::const_iterator ni = routing_table[n].find(dest);
        if(ni == routing_table[n].end()) continue;
        const vector<int> & next_cand = ni->second;
        size_t nnc = first_only ? 1 : next_cand.size();
        for(size_t j = 0; j < nnc; ++j) {
          int m = port_next_router[n][next_cand[j]];
          if(m >= 0) edges[r * _size + n].insert(n * _size + m);
        }
      }
    }
  }

  // Kahn's algorithm: whatever cannot be peeled off lies on a cycle.
  map<int, int> indeg;
  for(map<int, set<int> >::iterator ei = edges.begin(); ei != edges.end(); ++ei) {
    indeg[ei->first] += 0;
    for(set<int>::iterator si = ei->second.begin(); si != ei->second.end(); ++si) {
      ++indeg[*si];
    }
  }
  vector<int> ready;
  for(map<int, int>::iterator ii = indeg.begin(); ii != indeg.end(); ++ii) {
    if(ii->second == 0) ready.push_back(ii->first);
  }
  size_t peeled = 0;
  while(!ready.empty()) {
    int v = ready.back();
    ready.pop_back();
    ++peeled;
    map<int, set<int> >::iterator ei = edges.find(v);
    if(ei == edges.end()) continue;
    for(set<int>::iterator si = ei->second.begin(); si != ei->second.end(); ++si) {
      if(--indeg[*si] == 0) ready.push_back(*si);
    }
  }
  return peeled < indeg.size();
}

// Every routing function here stays on shortest paths, so hops_left toward the
// destination strictly decreases hop by hop. Giving each router-to-router hop
// its own slice of VCs, indexed by hops_left at the next router, therefore
// orders the buffers and makes the dependency graph acyclic -- for every
// topology, without forbidding any of the equal-cost paths that multipath
// routing exists to use.
void PCINet::setupDeadlockAvoidance( const Configuration &config ) {
  const string mode    = config.GetStr("pcinet_deadlock_avoidance");
  const string routing = config.GetStr("routing_function");

  int max_hops = 0;
  for(int r = 0; r < _size; ++r) {
    for(int t = 0; t < _size; ++t) {
      max_hops = max(max_hops, hops_left[r][t]);
    }
  }
  bool cyclic = dependencyGraphIsCyclic(routing);

  if(mode == "none") {
    vc_classes = 1;
  } else if(mode == "loopfree") {
    // No VC split: the flow traffic manager admits a path only if the
    // pass-throughs of everything in flight stay acyclic (see turn_graph.hpp).
    if(routing != "pinned") {
      cout << "PCINet: pcinet_deadlock_avoidance = loopfree needs "
           << "routing_function = pinned (got " << routing << "): only a "
           << "sender-chosen path can be checked before it is used" << endl;
      exit(-1);
    }
    vc_classes = 1;
    loopfree = true;
  } else if(mode == "auto") {
    vc_classes = cyclic ? max_hops : 1;
  } else if(mode == "vc_classes") {
    vc_classes = max(1, max_hops);
  } else {
    cout << "PCINet: unknown pcinet_deadlock_avoidance '" << mode
         << "' (expected auto, vc_classes, loopfree or none)" << endl;
    exit(-1);
  }

  // Distance classes are indexed by hops_left, which decreases only along
  // shortest routes, so they order nothing once pinned routes are not
  // shortest. Decide on the requested mode and on cyclicity, never on the
  // computed class count: a full mesh has max_hops = 1 (one class) although
  // its per-bridge two-hop routes can close a cycle.
  if(nonminimal_paths && (mode == "vc_classes" || (mode == "auto" && cyclic))) {
    cout << "PCINet: pcinet_cross_domain_paths = per_bridge gives routes that are "
         << "not shortest, and distance classes only order shortest routes; the "
         << "pinned graph here is " << (cyclic ? "CYCLIC" : "acyclic") << ". Use "
         << "pcinet_deadlock_avoidance = loopfree (or none to accept the "
         << "deadlock risk)" << endl;
    exit(-1);
  }

  cout << "PCINet: routing '" << routing << "' channel dependency graph is "
       << (cyclic ? "CYCLIC" : "acyclic") << "; ";
  if(vc_classes > 1) {
    int vcs = config.GetInt("num_vcs");
    if(vcs < vc_classes) {
      cout << endl << "PCINet: deadlock avoidance needs num_vcs >= "
           << vc_classes << " (the longest shortest path is " << max_hops
           << " router-to-router hops), got " << vcs << endl;
      exit(-1);
    }
    // Shared buffer policies let one class's flits take another class's
    // space, which reintroduces exactly the cyclic wait the classes remove.
    if(config.GetStr("buffer_policy") != "private") {
      cout << endl << "PCINet: deadlock avoidance requires buffer_policy = "
           << "private; (got " << config.GetStr("buffer_policy") << ")" << endl;
      exit(-1);
    }
    cout << "deadlock avoidance ON: " << vc_classes
         << " distance classes over " << vcs << " VCs" << endl;
  } else if(loopfree) {
    cout << "loop-free path selection ON (no VC partitioning)" << endl;
  } else if(cyclic) {
    cout << "deadlock avoidance OFF (pcinet_deadlock_avoidance = " << mode
         << "): this run CAN deadlock" << endl;
  } else {
    cout << "no VC partitioning needed" << endl;
  }
}

// ---------------------------------------------------------------------------
// Topology file parsing
// ---------------------------------------------------------------------------

// Tokenize a line into whitespace-separated fields, dropping any '#' comment.
static void pcinet_tokenize(const string & line, vector<string> & tok) {
  string::size_type hash = line.find('#');
  string body = (hash == string::npos) ? line : line.substr(0, hash);
  stringstream ss(body);
  string t;
  tok.clear();
  while(ss >> t) tok.push_back(t);
}

static void pcinet_parse_error(int line_no, const string & msg) {
  cout << "PCINet: parse error on line " << line_no << ": " << msg << endl;
  exit(-1);
}

// Per-lane effective throughput in GB/s (== bytes per ns), after line coding.
// Gen1/2 are 8b/10b; Gen3-5 are 128b/130b; Gen6 is PAM4 in FLIT mode, where the
// encoding overhead is replaced by FEC and CRC.
static double pcinet_gen_bytes_per_ns_per_lane(int gen, int line_no) {
  switch(gen) {
  case 1: return 0.250;
  case 2: return 0.500;
  case 3: return 0.985;
  case 4: return 1.969;
  case 5: return 3.938;
  case 6: return 7.563;
  default: break;
  }
  ostringstream os;
  os << "unsupported PCIe generation " << gen << " (expected 1-6)";
  pcinet_parse_error(line_no, os.str());
  return 0.0;
}

// Attributes accumulated across one node or router clause.
struct PCIeAttrs {
  PCIePortType ptype;
  PCIeLinkType ltype;
  int    lat;
  int    domain_id;
  int    gen;             // -1 == unset
  int    width;           // -1 == unset (lane count)
  double bw_bytes_per_ns; // -1 == unset
  int    cpf;             // -1 == unset (explicit override)
  PCIeAttrs(PCIePortType pt, PCIeLinkType lt, int l, int d)
    : ptype(pt), ltype(lt), lat(l), domain_id(d),
      gen(-1), width(-1), bw_bytes_per_ns(-1.0), cpf(-1) {}
};

// Collapse whatever bandwidth spec was given into cycles-per-flit.
// Precedence: explicit cpf > bw_gbps > gen/width > default (full rate).
static int pcinet_resolve_cpf(const PCIeAttrs & a, int flit_bytes, int line_no) {
  if(a.cpf > 0) return a.cpf;

  double bytes_per_ns = -1.0;
  if(a.bw_bytes_per_ns > 0.0) {
    bytes_per_ns = a.bw_bytes_per_ns;
  } else if(a.gen > 0 || a.width > 0) {
    if(a.gen <= 0) pcinet_parse_error(line_no, "width given without gen");
    if(a.width <= 0) pcinet_parse_error(line_no, "gen given without width");
    bytes_per_ns = pcinet_gen_bytes_per_ns_per_lane(a.gen, line_no) * (double)a.width;
  } else {
    return 1;  // unspecified links run at the reference full rate
  }

  // One cycle is one ns, so bytes/ns and the flit size give cycles per flit.
  double exact = (double)flit_bytes / bytes_per_ns;
  int cpf = (int)(exact + 0.5);
  if(cpf < 1) {
    ostringstream os;
    os << "link needs " << exact << " cycles/flit, which rounds below 1 at "
       << "flit_bytes=" << flit_bytes << "; lower flit_bytes (e.g. 32) to model "
       << "links this fast";
    pcinet_parse_error(line_no, os.str());
  }
  return cpf;
}

// Parse the attributes that may follow a node or router clause. Walks tok by
// index so an unrecognized token -- which is the start of the next clause -- is
// left unconsumed for the caller. The original stringstream version consumed it
// and then broke, silently dropping every clause after the first on a
// multi-item line.
static void pcinet_parse_attrs(const vector<string> & tok, size_t & i, int line_no,
                               PCIeAttrs & a, bool allow_domain) {
  while(i < tok.size()) {
    const string & k = tok[i];
    bool needs_value = (k == "port_type" || k == "link_type" || k == "lat" ||
                        k == "gen" || k == "width" || k == "bw_gbps" ||
                        k == "cpf" || (k == "domain" && allow_domain));
    if(needs_value && i + 1 >= tok.size()) {
      pcinet_parse_error(line_no, k + " needs a value");
    }

    if(k == "port_type") {
      const string & v = tok[i+1];
      if(v == "upstream")        a.ptype = PORT_UPSTREAM;
      else if(v == "downstream") a.ptype = PORT_DOWNSTREAM;
      else if(v == "ntb")        a.ptype = PORT_NTB;
      else if(v == "generic")    a.ptype = PORT_GENERIC;
      else pcinet_parse_error(line_no, "unknown port_type " + v);
      i += 2;
    } else if(k == "link_type") {
      const string & v = tok[i+1];
      if(v == "nontransparent" || v == "ntb") a.ltype = LINK_NON_TRANSPARENT;
      else if(v == "transparent")             a.ltype = LINK_TRANSPARENT;
      else pcinet_parse_error(line_no, "unknown link_type " + v);
      i += 2;
    } else if(k == "domain" && allow_domain) {
      a.domain_id = atoi(tok[i+1].c_str());
      i += 2;
    } else if(k == "lat") {
      a.lat = atoi(tok[i+1].c_str());
      i += 2;
    } else if(k == "gen") {
      a.gen = atoi(tok[i+1].c_str());
      i += 2;
    } else if(k == "width") {
      const string & v = tok[i+1];       // accepts "16" or "x16"
      const char * digits = v.c_str();
      if(v.size() > 1 && (v[0] == 'x' || v[0] == 'X')) digits = v.c_str() + 1;
      a.width = atoi(digits);
      if(a.width <= 0) pcinet_parse_error(line_no, "width must be positive");
      i += 2;
    } else if(k == "bw_gbps") {
      a.bw_bytes_per_ns = atof(tok[i+1].c_str());
      if(a.bw_bytes_per_ns <= 0.0) pcinet_parse_error(line_no, "bw_gbps must be positive");
      i += 2;
    } else if(k == "cpf") {
      a.cpf = atoi(tok[i+1].c_str());
      if(a.cpf < 1) pcinet_parse_error(line_no, "cpf must be >= 1");
      i += 2;
    } else if(isdigit((unsigned char)k[0])) {
      a.lat = atoi(k.c_str());   // bare integer == latency (anynet compat)
      i += 1;
    } else {
      break;                     // start of the next clause; leave it unconsumed
    }
  }
}

void PCINet::readFile() {
  ifstream network_list(file_name.c_str());
  if(!network_list.is_open()) {
    cout << "PCINet: Can't open network file " << file_name << endl;
    exit(-1);
  }

  string line;
  int line_no = 0;
  vector<string> tok;

  while(getline(network_list, line)) {
    ++line_no;
    pcinet_tokenize(line, tok);
    if(tok.empty()) continue;

    size_t i = 0;
    if(tok[i] != "router") {
      pcinet_parse_error(line_no, "line must begin with router, got " + tok[i]);
    }
    ++i;
    if(i >= tok.size()) pcinet_parse_error(line_no, "router needs an id");
    int head_id = atoi(tok[i].c_str());
    ++i;

    if(router_list[0].count(head_id) == 0) router_list[0][head_id] = map<int, PCIeLinkInfo>();
    if(router_list[1].count(head_id) == 0) router_list[1][head_id] = map<int, PCIeLinkInfo>();
    if(router_domain.count(head_id) == 0) router_domain[head_id] = 0;

    while(i < tok.size()) {
      const string kw = tok[i];

      if(kw == "node") {
        ++i;
        if(i >= tok.size()) pcinet_parse_error(line_no, "node needs an id");
        int node_id = atoi(tok[i].c_str());
        ++i;

        PCIeAttrs a(PORT_DOWNSTREAM, LINK_TRANSPARENT, 1, router_domain[head_id]);
        pcinet_parse_attrs(tok, i, line_no, a, true);
        int cpf = pcinet_resolve_cpf(a, flit_bytes, line_no);

        if(node_list.count(node_id) != 0 && node_list[node_id] != head_id) {
          cout << "PCINet: Node " << node_id << " connected to multiple routers ("
               << node_list[node_id] << " and " << head_id << ")" << endl;
          exit(-1);
        }
        node_list[node_id] = head_id;
        node_domain[node_id] = a.domain_id;
        router_list[0][head_id][node_id] = PCIeLinkInfo(-1, a.lat, cpf, a.ptype, a.ltype);

      } else if(kw == "router") {
        ++i;
        if(i >= tok.size()) pcinet_parse_error(line_no, "router link needs an id");
        int other_router_id = atoi(tok[i].c_str());
        ++i;

        if(router_list[0].count(other_router_id) == 0) router_list[0][other_router_id] = map<int, PCIeLinkInfo>();
        if(router_list[1].count(other_router_id) == 0) router_list[1][other_router_id] = map<int, PCIeLinkInfo>();
        if(router_domain.count(other_router_id) == 0) router_domain[other_router_id] = 0;

        PCIeAttrs a(PORT_GENERIC, LINK_TRANSPARENT, 1, 0);
        pcinet_parse_attrs(tok, i, line_no, a, false);
        int cpf = pcinet_resolve_cpf(a, flit_bytes, line_no);

        router_list[1][head_id][other_router_id] = PCIeLinkInfo(-1, a.lat, cpf, a.ptype, a.ltype);
        if(router_list[1][other_router_id].count(head_id) == 0) {
          router_list[1][other_router_id][head_id] = PCIeLinkInfo(-1, a.lat, cpf, a.ptype, a.ltype);
        }

      } else if(kw == "domain") {
        ++i;
        if(i >= tok.size()) pcinet_parse_error(line_no, "domain needs a value");
        router_domain[head_id] = atoi(tok[i].c_str());
        ++i;

      } else {
        pcinet_parse_error(line_no, "unknown keyword " + kw);
      }
    }
  }

  // anynet keeps the router and node adjacency maps in lockstep; check the same
  // here so a desynchronized key set is caught rather than indexed out of bounds
  // in _BuildNet, which dereferences router_list[1].find() without checking end().
  // A hard check rather than assert(): an embedding build may define NDEBUG.
  if(router_list[0].size() != router_list[1].size()) {
    cout << "PCINet: internal error, node/router adjacency maps desynchronized ("
         << router_list[0].size() << " vs " << router_list[1].size() << ")" << endl;
    exit(-1);
  }

  // _routers[] and the outport[] scratch array in _BuildNet are indexed by router
  // id, so ids must be dense and zero-based -- the same rule the node check below
  // enforces for nodes.
  int max_router = -1;
  for(map<int, map<int, PCIeLinkInfo> >::iterator ri = router_list[1].begin();
      ri != router_list[1].end(); ++ri) {
    if(ri->first < 0) {
      cout << "PCINet: negative router id " << ri->first << endl;
      exit(-1);
    }
    if(ri->first > max_router) max_router = ri->first;
  }
  if(max_router + 1 != (int)router_list[1].size()) {
    cout << "PCINet: router ids must be contiguous starting at 0 (max id "
         << max_router << " but " << router_list[1].size() << " routers)" << endl;
    exit(-1);
  }

  // Verification: ensure sequential node numbering starting from 0
  vector<int> node_check;
  for(map<int, int>::iterator ni = node_list.begin(); ni != node_list.end(); ++ni) {
    node_check.push_back(ni->first);
  }
  sort(node_check.begin(), node_check.end());
  for(size_t j = 0; j < node_check.size(); ++j) {
    if((size_t)node_check[j] != j) {
      cout << "PCINet: BookSim requires sequential node numbering starting at 0\n";
      exit(-1);
    }
  }
}
