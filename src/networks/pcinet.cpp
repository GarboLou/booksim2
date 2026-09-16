// $Id$

/*
 Copyright (c) 2007-2015, Trustees of The Leland Stanford Junior University
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
#include <iostream>
#include <cctype>

// Global pointer to the multipath routing table [router_id][dest_node] = vector<int> candidate ports
static vector<map<int, vector<int> > >* global_pcinet_routing_table = NULL;

PCINet::PCINet( const Configuration &config, const string & name )
  : Network( config, name ) {

  router_list.resize(2);
  _ComputeSize( config );
  _Alloc( );
  _BuildNet( config );
}

PCINet::~PCINet() {
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
      
      _inject[link]->SetLatency(nniter->second.latency);
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

      _chan[link]->SetLatency(rriter->second.latency);
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

  buildRoutingTable();
}

void PCINet::RegisterRoutingFunctions() {
  gRoutingFunctionMap["min_pcinet"] = &min_pcinet;
  gRoutingFunctionMap["ecmp_pcinet"] = &ecmp_pcinet;
  gRoutingFunctionMap["adaptive_pcinet"] = &adaptive_pcinet;
}

static void apply_vc_range( const Flit *f, OutputSet *outputs, int out_port ) {
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
  apply_vc_range(f, outputs, out_port);
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
  apply_vc_range(f, outputs, out_port);
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
      // downstream buffer's OCCUPANCY (iq_router.cpp: dest_buf->Occupancy()),
      // i.e. credits CONSUMED -- so the least congested port is the MINIMUM.
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
  apply_vc_range(f, outputs, out_port);
}

void PCINet::buildRoutingTable() {
  cout << "========================== PCINet Routing Table Generation =====================\n";
  routing_table.resize(_size);
  for(int i = 0; i < _size; i++) {
    route(i);
  }
  global_pcinet_routing_table = &routing_table;
}

// Compute shortest paths and extract all equal-cost candidate next-hops
void PCINet::route(int r_start) {
  // First, handle locally attached nodes at r_start
  for(map<int, PCIeLinkInfo>::iterator iter = router_list[0][r_start].begin();
      iter != router_list[0][r_start].end();
      iter++) {
    routing_table[r_start][iter->first].push_back(iter->second.port);
  }

  // Dijkstra algorithm to find minimal distances from r_start to all routers
  vector<int> dist(_size, numeric_limits<int>::max());
  dist[r_start] = 0;

  // Min-heap for Dijkstra: pair<distance, router_id>
  priority_queue<pair<int, int>, vector<pair<int, int> >, greater<pair<int, int> > > pq;
  pq.push(make_pair(0, r_start));

  while(!pq.empty()) {
    int d = pq.top().first;
    int u = pq.top().second;
    pq.pop();

    if(d > dist[u]) continue;

    for(map<int, PCIeLinkInfo>::iterator iter = router_list[1][u].begin();
        iter != router_list[1][u].end();
        iter++) {
      int v = iter->first;
      int weight = iter->second.latency;
      
      // PCIe check: cross-domain communication requires NTB link or explicit bridge
      // Intra-domain routing prefers transparent links
      int new_dist = dist[u] + weight;
      if(new_dist < dist[v]) {
        dist[v] = new_dist;
        pq.push(make_pair(new_dist, v));
      }
    }
  }

  // Find all equal-cost candidate next hops from r_start toward each router target
  for(int target_router = 0; target_router < _size; target_router++) {
    if(target_router == r_start || dist[target_router] == numeric_limits<int>::max()) {
      continue;
    }

    // A neighbor 'nbr' of r_start is an equal-cost first hop toward target_router if:
    // dist(r_start, nbr) + dist(nbr, target_router) == dist(r_start, target_router)
    // We compute this by running a reverse BFS/Dijkstra or by direct shortest path check
    for(map<int, PCIeLinkInfo>::iterator nbr_iter = router_list[1][r_start].begin();
        nbr_iter != router_list[1][r_start].end();
        nbr_iter++) {
      int nbr = nbr_iter->first;
      int edge_lat = nbr_iter->second.latency;
      int port = nbr_iter->second.port;

      // Dijkstra from nbr to target_router
      vector<int> nbr_dist(_size, numeric_limits<int>::max());
      nbr_dist[nbr] = 0;
      priority_queue<pair<int, int>, vector<pair<int, int> >, greater<pair<int, int> > > nbr_pq;
      nbr_pq.push(make_pair(0, nbr));
      while(!nbr_pq.empty()) {
        int cd = nbr_pq.top().first;
        int cu = nbr_pq.top().second;
        nbr_pq.pop();
        if(cd > nbr_dist[cu]) continue;
        for(map<int, PCIeLinkInfo>::iterator it = router_list[1][cu].begin();
            it != router_list[1][cu].end();
            it++) {
          int cv = it->first;
          int w = it->second.latency;
          if(nbr_dist[cu] + w < nbr_dist[cv]) {
            nbr_dist[cv] = nbr_dist[cu] + w;
            nbr_pq.push(make_pair(nbr_dist[cv], cv));
          }
        }
      }

      if(edge_lat + nbr_dist[target_router] == dist[target_router]) {
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

  // Print summary of paths discovered
  for(map<int, vector<int> >::iterator it = routing_table[r_start].begin(); it != routing_table[r_start].end(); it++) {
    cout << "Router " << r_start << " -> Dest Node " << it->first << " candidate ports: [";
    for(size_t p = 0; p < it->second.size(); p++) {
      cout << (p > 0 ? ", " : "") << it->second[p];
    }
    cout << "]\n";
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
// index so an unrecognized token -- which is the start of the NEXT clause -- is
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
