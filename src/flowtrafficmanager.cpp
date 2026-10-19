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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>

#include "flowtrafficmanager.hpp"
#include "buffer_state.hpp"
#include "credit.hpp"
#include "flitchannel.hpp"
#include "globals.hpp"
#include "packet_reply_info.hpp"
#include "pcie_credits.hpp"
#include "pcinet.hpp"
#include "random_utils.hpp"
#include "routefunc.hpp"  // gNumVCs, g{Read,Write}Req{Begin,End}VC, gReadReplyBeginVC/EndVC

namespace {

// Stateless 64-bit mixer (splitmix64 finalizer), for multipath_select = hash.
unsigned long long Mix64( unsigned long long x )
{
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

} // namespace

FlowTrafficManager::FlowTrafficManager( const Configuration & config,
                                        const vector<Network *> & net )
  : TrafficManager( config, net ),
    _next_flow_id(0), _flows_outstanding(0),
    _outstanding_flits(0), _queued_flits(0),
    _on_send(NULL), _on_send_ctx(NULL),
    _on_recv(NULL), _on_recv_ctx(NULL)
{
  _flit_bytes   = config.GetInt("flit_bytes");
  _mps          = config.GetInt("pcie_mps");
  _tlp_overhead = config.GetInt("pcie_tlp_overhead_bytes");
  _max_queued_packets = config.GetInt("max_queued_packets");

  if(_flit_bytes <= 0) Error("flit_bytes must be positive");
  if(_mps <= 0)        Error("pcie_mps must be positive");
  if(_tlp_overhead < 0) Error("pcie_tlp_overhead_bytes must be non-negative");
  if(_max_queued_packets <= 0) Error("max_queued_packets must be positive");

  _deadlock_abort_cycles = config.GetInt("deadlock_abort_cycles");
  if(_deadlock_abort_cycles < 0) Error("deadlock_abort_cycles must be non-negative");
  _stall_cycles = 0;
  _retired_this_cycle = false;

  // P/NP/Cpl credit-pool separation (separate pools per type; PCIe ordering
  // rules are not enforced): a flow
  // tagged pcie_class=1/2/3 gets Flit::WRITE_REQUEST/READ_REQUEST/READ_REPLY,
  // which routes through PCINet::apply_vc_range() into the VC range the
  // matching g*BeginVC/g*EndVC pair names, and needs storage of its own per
  // range for those ranges' credit pools to be independent (see
  // _CheckPcieClassVcRange). Deliberately not validated eagerly here, unlike
  // the checks above: BookSim's stock config defaults for those VC-range
  // globals are concrete, non-negative values sized for num_vcs=16 (e.g.
  // write_request_end_vc defaults to 7), not the -1 "unconfigured" sentinel
  // -- a config that never sets them inherits those defaults with its own,
  // often smaller, num_vcs, so an eager check here would reject every
  // config that has never even heard of pcie_class. Checked lazily instead,
  // in InjectFlow, only the first time each class actually gets used.
  _buffer_policy = config.GetStr("buffer_policy");
  for(int i = 0; i < 4; ++i) _pcie_class_checked[i] = false;
  if(_buffer_policy == "shared" || _buffer_policy == "limited") {
    _pools_vs_types = CheckPoolsMatchTypes(config);
  }

  // PCIe VC0 switch model: a type for untagged flows, and the
  // whole-TLP credit metadata.
  const string def = config.GetStr("pcie_default_class");
  if(def == "none") {
    _pcie_default_class = 0;
  } else if(def == "P") {
    _pcie_default_class = 1;
  } else if(def == "NP") {
    _pcie_default_class = 2;
  } else if(def == "Cpl") {
    _pcie_default_class = 3;
  } else {
    Error("pcie_default_class must be none, P, NP or Cpl (got " + def + ")");
  }
  _pcie_credits = ReadPcieCreditLimits(config).enabled;
  _check_path_order = (config.GetInt("check_path_order") > 0);

  // A full-size TLP costs its payload plus the per-TLP overhead. Charging the
  // overhead as real flits (rather than derating the link rate) is what makes
  // small transfers correctly more expensive per byte than large ones, which is
  // where fabric topology differences actually show up.
  _flits_per_packet = (_mps + _tlp_overhead + _flit_bytes - 1) / _flit_bytes;

  // Sender-pinned multipath (routing_function = pinned). Validated
  // even when unused, so a typo never silently runs the default.
  _pcinet = dynamic_cast<PCINet *>(_net[0]);
  _pinned = (config.GetStr("routing_function") == "pinned");
  if(_pinned && !_pcinet) {
    Error("routing_function = pinned needs topology = pcinet");
  }
  const string gran = config.GetStr("multipath_granularity");
  if(gran == "packet") {
    _granularity = kGranPacket;
  } else if(gran == "request") {
    _granularity = kGranRequest;
  } else if(gran == "flow") {
    _granularity = kGranFlow;
  } else {
    Error("multipath_granularity must be request, flow or packet (got " + gran + ")");
  }
  const int dma_request_bytes = config.GetInt("dma_request_bytes");
  if(dma_request_bytes <= 0 || dma_request_bytes % _mps != 0) {
    ostringstream err;
    err << "dma_request_bytes (" << dma_request_bytes << ") must be a positive "
        << "multiple of pcie_mps (" << _mps << ")";
    Error(err.str());
  }
  _packets_per_request = dma_request_bytes / _mps;
  const string sel = config.GetStr("multipath_select");
  if(sel == "round_robin") {
    _select = kSelRoundRobin;
  } else if(sel == "hash") {
    _select = kSelHash;
  } else if(sel == "weighted") {
    _select = kSelWeighted;
  } else {
    Error("multipath_select must be round_robin, hash or weighted (got " + sel + ")");
  }
  _next_path_key.assign(_nodes, vector<unsigned long long>(_nodes, 0));

  _loopfree = (_pcinet != NULL) && _pcinet->LoopFree();
  _lf_check_interval = config.GetInt("loopfree_check_interval");
  if(_lf_check_interval < 0) Error("loopfree_check_interval must be non-negative");
  const string retry = config.GetStr("loopfree_retry_order");
  if(retry != "rotate" && retry != "oldest_first") {
    Error("loopfree_retry_order must be rotate or oldest_first (got " + retry + ")");
  }
  _lf_oldest_first = _loopfree && (retry == "oldest_first");
  // Untagged flows may use any VC, so a wait cycle could cross per-type
  // graphs: one graph for everything unless every flow gets a type. Then
  // each type's VCs and storage are its own (_CheckPcieClassVcRange insists
  // on that at first use) and P, NP and Cpl get one graph each. Merging only
  // ever refuses more, so the single graph is always safe.
  const int lf_pools = (_pcie_default_class != 0) ? 3 : 1;
  _lf_pool_of_class[0] = 0;
  for(int c = 1; c < 4; ++c) _lf_pool_of_class[c] = (lf_pools == 3) ? c - 1 : 0;
  if(_loopfree) {
    _lf = LoopFreeUnits(lf_pools, _pcinet->NumRouterChannels());
    if(_lf_check_interval > 0) _pcinet->SetLoopFreeVerifier(&_lf);
  }
  _lf_admitted = _lf_waited_units = _lf_wait_cycles = _lf_max_wait = 0;
  _lf_alt_path = 0;
  _tlp_count = 0;
  _tlp_switches = 0;
  _flow_csv = NULL;
  _units_retired = 0;

  cout << "multipath: routing_function=" << config.GetStr("routing_function")
       << " multipath_granularity=" << gran
       << " dma_request_bytes=" << dma_request_bytes
       << " multipath_select=" << sel
       << " pcinet_deadlock_avoidance=" << config.GetStr("pcinet_deadlock_avoidance")
       << endl;

  const string order = config.GetStr("source_interleave");
  if(order != "tlp" && order != "flow") {
    Error("source_interleave must be tlp or flow (got " + order + ")");
  }
  _interleave_tlp = (order == "tlp");
  const bool whole_tlp = (config.GetInt("whole_tlp") > 0);
  FlitChannel::SetCheckWholePackets(config.GetInt("check_whole_tlp") > 0);
  cout << "source: source_interleave=" << order << " whole_tlp="
       << (whole_tlp ? 1 : 0)
       << (whole_tlp ? " (every link sends whole TLPs)"
                     : " (switches may split TLPs on a link)") << endl;

  _pending_flows.resize(_nodes);
  _shadow_queues.resize(_nodes);
  _rr_next.assign(_nodes, vector<size_t>(_classes, 0));
  for(int n = 0; n < _nodes; ++n) {
    _pending_flows[n].resize(_classes);
    _shadow_queues[n].resize(_classes);
  }

  // Run() normally does this before entering the sample loop. We never call
  // Run(), so set up the state it would have established: stats are live from
  // cycle 0 and no synthetic traffic is ever generated.
  _empty_network = true;
  _sim_state = running;
  _ClearStats();
}

FlowTrafficManager::~FlowTrafficManager( )
{
  for(map<FlowId, FlowRecord *>::iterator i = _flows.begin(); i != _flows.end(); ++i) {
    delete i->second;
  }
  FlitChannel::SetCheckWholePackets(false);
}

bool FlowTrafficManager::_SingleSim( )
{
  Error("FlowTrafficManager is externally driven; Run()/_SingleSim() must not be called. "
        "Use Step() instead.");
  return false;
}

FlowTrafficManager::FlowRecord * FlowTrafficManager::_Lookup( FlowId id )
{
  map<FlowId, FlowRecord *>::iterator i = _flows.find(id);
  return (i == _flows.end()) ? NULL : i->second;
}

FlowTrafficManager::FlowId
FlowTrafficManager::InjectFlow( int src, int dst, unsigned long long bytes,
                                int cls, int pcie_class, void * user )
{
  if(src < 0 || src >= _nodes) Error("InjectFlow: source out of range");
  if(dst < 0 || dst >= _nodes) Error("InjectFlow: destination out of range");
  if(cls < 0 || cls >= _classes) Error("InjectFlow: class out of range");
  if(pcie_class < 0 || pcie_class > 3) Error("InjectFlow: pcie_class out of range");
  if(bytes == 0) Error("InjectFlow: zero-byte flow");
  if(pcie_class == 0) pcie_class = _pcie_default_class;
  if(pcie_class == 0 && _pcie_credits) {
    Error("pcie_credits = 1 needs every flow to have a PCIe type: set "
          "pcie_default_class or tag the flow");
  }
  if(pcie_class != 0) _CheckPcieClassVcRange(pcie_class);
  if(_pinned && _pcinet->NumPaths(src, dst) == 0) {
    ostringstream err;
    err << "InjectFlow: no path from node " << src << " to node " << dst
        << (_pcinet->PerBridgePaths()
            ? " (no route under pcinet_cross_domain_paths = per_bridge, or "
              "disconnected topology)"
            : " (disconnected topology)");
    Error(err.str());
  }

  FlowRecord * fr = new FlowRecord;
  fr->id         = _next_flow_id++;
  fr->src        = src;
  fr->dst        = dst;
  fr->cls        = cls;
  fr->pcie_class = pcie_class;
  fr->bytes      = bytes;
  fr->user       = user;

  // Payload per packet is MPS; the last packet carries the remainder.
  unsigned long long payload = (unsigned long long)_mps;
  fr->packets_total   = (bytes + payload - 1) / payload;
  fr->packets_made    = 0;
  fr->packets_done    = 0;
  fr->bytes_remaining = bytes;
  fr->flits_queued    = 0;

  fr->t_inject   = (unsigned long long)_time;
  fr->t_sent     = 0;
  fr->t_done     = 0;
  fr->hops_total = 0;

  fr->cur_path_key  = -1;
  fr->units_started = 0;
  fr->paths_used    = 0;
  fr->lf_unit       = -1;
  fr->lf_wait_since = -1;

  _flows[fr->id] = fr;
  _pending_flows[src][cls].push_back(fr);
  ++_flows_outstanding;

  return fr->id;
}

void FlowTrafficManager::_CheckPcieClassVcRange( int pcie_class )
{
  if(_pcie_class_checked[pcie_class]) return;
  _pcie_class_checked[pcie_class] = true;

  // Each type's VCs must have storage of their own. private gives every VC
  // its own; shared (and limited, its capped variant) qualifies when every
  // private pool is exactly one type's VC range and the common pool is 0
  // (as in PCIe, where each type has its own buffers, so one type never
  // blocks another). The feedback policies share across ranges.
  const bool shared_ok = (_buffer_policy == "shared" || _buffer_policy == "limited") &&
                         _pools_vs_types.empty();
  if(_buffer_policy != "private" && !shared_ok) {
    ostringstream err;
    err << "FlowTrafficManager: pcie_class=" << pcie_class << " needs each "
        << "type's VC range to have storage of its own: buffer_policy = "
        << "private, or shared/limited with one private pool per type range "
        << "and no common pool (got " << _buffer_policy;
    if(!_pools_vs_types.empty()) err << ": " << _pools_vs_types;
    err << ").";
    Error(err.str());
  }

  const char * name = NULL;
  int begin = -1, end = -1;
  switch(pcie_class) {
  case 1: name = "Posted (write_request)";    begin = gWriteReqBeginVC;  end = gWriteReqEndVC;  break;
  case 2: name = "Non-Posted (read_request)"; begin = gReadReqBeginVC;   end = gReadReqEndVC;   break;
  case 3: name = "Completion (read_reply)";   begin = gReadReplyBeginVC; end = gReadReplyEndVC; break;
  default: return; // unreachable, pcie_class already validated by the caller
  }
  if(begin < 0 || end < 0 || begin > end || end >= gNumVCs) {
    ostringstream err;
    err << "FlowTrafficManager: " << name << " VC range [" << begin << ","
        << end << "] is invalid for num_vcs=" << gNumVCs << ". Set "
        << "read_request_begin_vc/_end_vc, write_request_begin_vc/_end_vc, "
        << "and read_reply_begin_vc/_end_vc to disjoint ranges within "
        << "[0, num_vcs-1] to use pcie_class tagging (see "
        << "runfiles/pcie_pnpcpl_example.cfg).";
    Error(err.str());
  }
  // Overlapping ranges would share VCs, and with them credit pools.
  for(int other = 1; other <= 3; ++other) {
    if(other == pcie_class || !_pcie_class_checked[other]) continue;
    int ob = -1, oe = -1;
    switch(other) {
    case 1: ob = gWriteReqBeginVC;  oe = gWriteReqEndVC;  break;
    case 2: ob = gReadReqBeginVC;   oe = gReadReqEndVC;   break;
    case 3: ob = gReadReplyBeginVC; oe = gReadReplyEndVC; break;
    }
    if(begin <= oe && ob <= end) {
      ostringstream err;
      err << "FlowTrafficManager: " << name << " VC range [" << begin << ","
          << end << "] overlaps the range of pcie_class=" << other << " ["
          << ob << "," << oe << "]; types must not share VCs.";
      Error(err.str());
    }
  }
}

// Top up each source's injection queue. Deliberately lazy: a 1 GB flow is
// millions of packets and materializing them all would exhaust memory long
// before the simulation finished.
void FlowTrafficManager::_ServiceFlowQueues( )
{
  // Under loopfree, sources compete for admission in this loop's order, so
  // rotate where it starts each cycle; otherwise keep the fixed order.
  const int first = _loopfree ? (int)(_time % _nodes) : 0;

  // loopfree_retry_order = oldest_first: units already waiting for admission
  // retry before anyone else may start a new unit, longest-waiting first.
  vector<char> done;
  if(_lf_oldest_first) {
    vector<pair<long long, int> > waiting;   // (waiting since, rotated slot)
    for(int i = 0; i < _nodes; ++i) {
      const int n = (first + i) % _nodes;
      for(int c = 0; c < _classes; ++c) {
        const long long since = _OldestWait(n, c);
        if(since >= 0) waiting.push_back(make_pair(since, i * _classes + c));
      }
    }
    sort(waiting.begin(), waiting.end());
    done.assign(_nodes * _classes, 0);
    for(size_t w = 0; w < waiting.size(); ++w) {
      const int slot = waiting[w].second;
      _ServiceSource((first + slot / _classes) % _nodes, slot % _classes);
      done[slot] = 1;
    }
  }

  for(int i = 0; i < _nodes; ++i) {
    const int n = (first + i) % _nodes;
    for(int c = 0; c < _classes; ++c) {
      if(done.empty() || !done[i * _classes + c]) _ServiceSource(n, c);
    }
  }
}

long long FlowTrafficManager::_OldestWait( int n, int c ) const
{
  const deque<FlowRecord *> & pending = _pending_flows[n][c];
  if(pending.empty()) return -1;
  // One flow at a time: only the front flow ever asks for admission.
  if(!_interleave_tlp) return pending.front()->lf_wait_since;
  long long oldest = -1;
  for(size_t i = 0; i < pending.size(); ++i) {
    const long long w = pending[i]->lf_wait_since;
    if(w >= 0 && (oldest < 0 || w < oldest)) oldest = w;
  }
  return oldest;
}

void FlowTrafficManager::_ServiceSource( int n, int c )
{
  deque<FlowRecord *> & pending = _pending_flows[n][c];
  size_t & next = _rr_next[n][c];
  size_t refused = 0;   // flows in a row whose next unit was refused
  while(!pending.empty() &&
        (int)_partial_packets[n][c].size() < _max_queued_packets * _flits_per_packet) {
    // source_interleave = flow always takes the front flow.
    if(!_interleave_tlp || next >= pending.size()) next = 0;
    FlowRecord * fr = pending[next];

    if(_pinned && _UnitStartsHere(fr) && !_StartUnit(fr)) {
      // No admissible path yet. One flow at a time: this source waits.
      // Interleaved: its other flows go on; stop once every one is refused.
      if(!_interleave_tlp || ++refused >= pending.size()) break;
      ++next;
      continue;
    }
    refused = 0;

    // Last packet of a flow carries whatever is left, so a 64 B transfer
    // costs one small packet rather than a full MPS-sized one.
    unsigned long long payload = (unsigned long long)_mps;
    if(fr->bytes_remaining < payload) payload = fr->bytes_remaining;

    int size_flits = (int)((payload + (unsigned long long)_tlp_overhead
                            + (unsigned long long)_flit_bytes - 1)
                           / (unsigned long long)_flit_bytes);
    if(size_flits < 1) size_flits = 1;

    _GenerateFlowPacket(fr, size_flits, (int)payload);

    fr->bytes_remaining -= payload;
    ++fr->packets_made;

    // The unit's last packet is queued. Its turns stay registered until
    // that packet's tail retires, however long it waits in the queue.
    if(fr->lf_unit >= 0 &&
       (fr->packets_made == fr->packets_total || _UnitStartsHere(fr))) {
      _lf.UnitGenerated(fr->lf_unit);
      fr->lf_unit = -1;
    }

    if(fr->packets_made == fr->packets_total) {
      assert(fr->bytes_remaining == 0);
      pending.erase(pending.begin() + next);   // next now names the flow after it
    } else if(_interleave_tlp) {
      ++next;
    }
  }
}

// Does the next packet of fr begin a new path-selection unit?
bool FlowTrafficManager::_UnitStartsHere( const FlowRecord * fr ) const
{
  switch(_granularity) {
  case kGranPacket:  return true;
  case kGranRequest: return (fr->packets_made % _packets_per_request) == 0;
  case kGranFlow:    return fr->packets_made == 0;
  }
  return true;
}

// Pick the path for the unit that starts with fr's next packet. round_robin
// rotates per (source, destination) node pair, like a driver striping
// requests across NTB windows; hash depends only on the flow and unit index.
bool FlowTrafficManager::_StartUnit( FlowRecord * fr )
{
  const int npaths = _pcinet->NumPaths(fr->src, fr->dst);
  if(npaths <= 0) Error("pinned flow without a path");   // InjectFlow checks this
  int preferred;
  if(_select == kSelWeighted) {
    preferred = _WeightedPick(fr->src, fr->dst, npaths);
  } else {
    const unsigned long long pick = (_select == kSelHash)
      ? Mix64(Mix64(fr->id) ^ fr->units_started)
      : _next_path_key[fr->src][fr->dst];
    preferred = (int)(pick % (unsigned long long)npaths);
  }
  int key = preferred;

  if(_loopfree) {
    // Candidates in the selector's order; take the first whose pass-throughs
    // keep the in-flight graph acyclic (a cycle could deadlock).
    const int pool = _lf_pool_of_class[fr->pcie_class];
    key = -1;
    for(int i = 0; i < npaths; ++i) {
      int k = (preferred + i) % npaths;
      if(_lf.CanAdmit(pool, _pcinet->GetPath(fr->src, fr->dst, k).turns)) {
        key = k;
        break;
      }
    }
    if(key < 0) {
      if(fr->lf_wait_since < 0) fr->lf_wait_since = _time;
      return false;
    }
    if(fr->lf_wait_since >= 0) {
      unsigned long long waited = (unsigned long long)(_time - fr->lf_wait_since);
      ++_lf_waited_units;
      _lf_wait_cycles += waited;
      _lf_max_wait = max(_lf_max_wait, waited);
      fr->lf_wait_since = -1;
    }
    if(key != preferred) ++_lf_alt_path;

    assert(fr->lf_unit < 0);
    fr->lf_unit = _lf.Admit(pool, &_pcinet->GetPath(fr->src, fr->dst, key).turns);
    if(fr->lf_unit >= 0) ++_lf_admitted;
  }

  if(_select == kSelRoundRobin) ++_next_path_key[fr->src][fr->dst];
  if(_select == kSelWeighted) _WeightedCommit(fr->src, fr->dst, npaths, key);
  if((size_t)key >= _units_by_key.size()) _units_by_key.resize(key + 1, 0);
  ++_units_by_key[key];
  fr->cur_path_key = key;
  ++fr->units_started;
  if(key < 64) fr->paths_used |= 1ull << key;
  return true;
}

// Smooth weighted round robin (as in nginx): each path's weight is inversely
// proportional to its slowest router-to-router link (SwrrWeights). Pick the
// path whose current weight plus its weight is largest (lowest key on ties)
// without changing state; commit only once a unit is admitted, for the key it
// took.
bool SwrrWeights( const vector<int> & cpf, vector<long long> & w )
{
  // Weight k = L / cpf[k] with L the least common multiple of all cpf, so
  // every weight is a positive integer and the ratios are exact. Bounded so
  // the running sums in the selector cannot overflow.
  const long long kMax = 1LL << 40;
  long long l = 1;
  for(size_t k = 0; k < cpf.size(); ++k) {
    if(cpf[k] < 1) return false;
    long long a = l, b = cpf[k];
    while(b != 0) { long long r = a % b; a = b; b = r; }      // gcd(l, cpf[k])
    // Check the bound before multiplying: l <= 2^40 and cpf[k] < 2^31, so
    // the product itself could overflow 64 bits.
    const long long factor = l / a;
    if(factor > kMax / cpf[k]) return false;
    l = factor * cpf[k];
  }
  w.resize(cpf.size());
  for(size_t k = 0; k < cpf.size(); ++k) w[k] = l / cpf[k];
  return true;
}

FlowTrafficManager::Swrr & FlowTrafficManager::_SwrrState( int src, int dst, int npaths )
{
  Swrr & st = _swrr[make_pair(src, dst)];
  if((int)st.w.size() != npaths) {
    vector<int> cpf(npaths);
    for(int k = 0; k < npaths; ++k) cpf[k] = _pcinet->GetPath(src, dst, k).bottleneck_cpf;
    if(!SwrrWeights(cpf, st.w)) {
      ostringstream err;
      err << "multipath_select = weighted: the paths from node " << src << " to node "
          << dst << " differ too much in speed for exact integer weights";
      Error(err.str());
    }
    st.cw.assign(npaths, 0);
  }
  return st;
}

int FlowTrafficManager::_WeightedPick( int src, int dst, int npaths )
{
  const Swrr & st = _SwrrState(src, dst, npaths);
  int best = 0;
  long long best_val = 0;
  for(int k = 0; k < npaths; ++k) {
    long long v = st.cw[k] + st.w[k];
    if(k == 0 || v > best_val) { best = k; best_val = v; }
  }
  return best;
}

void FlowTrafficManager::_WeightedCommit( int src, int dst, int npaths, int key )
{
  Swrr & st = _SwrrState(src, dst, npaths);
  long long total = 0;
  for(int k = 0; k < npaths; ++k) {
    st.cw[k] += st.w[k];
    total += st.w[k];
  }
  st.cw[key] -= total;
}

// Debug: the registered graph is acyclic, and every reference belongs to a
// live unit.
void FlowTrafficManager::_LfCheck( ) const
{
  string err = _lf.Check();
  if(!err.empty()) {
    ostringstream os;
    os << "loopfree consistency check failed at cycle " << _time << ": " << err;
    Error(os.str());
  }
}

// Adapted from TrafficManager::_GeneratePacket. That method is non-virtual and
// takes its destination from _traffic_pattern, which we do not use, so the body
// is reproduced here with the destination supplied explicitly.
//
// f->type is set from fr->pcie_class (see FlowRecord/InjectFlow) rather than
// always Flit::ANY_TYPE: this is what feeds PCINet::apply_vc_range()'s VC-range
// selection and thus the P/NP/Cpl credit-pool separation.
// `subnetwork` below deliberately stays indexed by Flit::ANY_TYPE regardless of
// pcie_class -- the four `*_subnet` config keys all default to 0 and nothing in
// this project sets `subnets` above its default of 1, so per-type subnetwork
// routing is out of scope; if that ever changes, this must be revisited too.
// Every member touched below is protected in TrafficManager.
void FlowTrafficManager::_GenerateFlowPacket( FlowRecord * fr, int size_flits,
                                              int payload )
{
  // pcie_credits: each flit's share of the data credits it returns on
  // leaving an input queue (the head reserves them all, plus one header).
  vector<int> data_ret;
  if(_pcie_credits) {
    data_ret = PcieDataReturns(payload, _tlp_overhead, _flit_bytes, size_flits);
  }

  static const Flit::FlitType kPcieClassToFlitType[4] = {
    Flit::ANY_TYPE,       // 0: unset -- full VC pool, as stock BookSim
    Flit::WRITE_REQUEST,  // 1: Posted
    Flit::READ_REQUEST,   // 2: Non-Posted
    Flit::READ_REPLY,     // 3: Completion
  };

  int source = fr->src;
  int cl     = fr->cls;
  int time   = _time;

  int pid = _cur_pid++;
  assert(_cur_pid);

  bool record = _measure_stats[cl];
  bool watch  = gWatchOut && (_packets_to_watch.count(pid) > 0);

  int subnetwork = _subnet[Flit::ANY_TYPE];

  for(int i = 0; i < size_flits; ++i) {
    Flit * f = Flit::New();
    f->id    = _cur_id++;
    assert(_cur_id);
    f->pid   = pid;
    f->watch = watch | (gWatchOut && (_flits_to_watch.count(f->id) > 0));
    f->subnetwork = subnetwork;
    f->src   = source;
    f->ctime = time;
    f->record = record;
    f->cl    = cl;
    f->type  = kPcieClassToFlitType[fr->pcie_class];
    f->path_key = fr->cur_path_key;
    f->lf_unit  = fr->lf_unit;
    if(_pcie_credits) {
      f->pcie_payload  = payload;
      f->pcie_data_ret = data_ret[i];
    }

    // The flow id rides along on the otherwise unused Flit::data pointer, so
    // _RetireFlit can attribute an arriving tail flit to its flow. Stored as an
    // integer, not a FlowRecord*, so a completed-and-freed flow can never be
    // dereferenced through a stale flit.
    f->data = (void *)(uintptr_t)fr->id;

    _total_in_flight_flits[f->cl].insert(make_pair(f->id, f));
    if(record) {
      _measured_in_flight_flits[f->cl].insert(make_pair(f->id, f));
    }

    if(gTrace) {
      cout << "New Flit " << f->src << endl;
    }

    if(i == 0) {
      f->head = true;
      f->dest = fr->dst;
    } else {
      f->head = false;
      f->dest = -1;
    }

    switch(_pri_type) {
    case class_based:
      f->pri = _class_priority[cl];
      break;
    case age_based:
      f->pri = numeric_limits<int>::max() - time;
      break;
    case sequence_based:
      f->pri = numeric_limits<int>::max() - _packet_seq_no[source];
      break;
    default:
      f->pri = 0;
    }
    assert(f->pri >= 0);

    f->tail = (i == size_flits - 1);
    f->vc   = -1;

    if(f->watch) {
      *gWatchOut << GetSimTime() << " | node" << source << " | Enqueuing flit "
                 << f->id << " (packet " << f->pid << ", flow " << fr->id
                 << ") at time " << time << "." << endl;
    }

    _partial_packets[source][cl].push_back(f);
    _shadow_queues[source][cl].push_back(fr->id);
    ++fr->flits_queued;
    ++_queued_flits;
    ++_outstanding_flits;
  }

  ++_packet_seq_no[source];
  if(fr->lf_unit >= 0) _lf.PacketGenerated(fr->lf_unit);
}

// _Step() drains _partial_packets[n][c] from the front, at most one flit per
// (node, class) per cycle. Comparing the shadow queue's length against the real
// one therefore tells us exactly which flits were injected this cycle, without
// having to modify the non-virtual _Step().
void FlowTrafficManager::_PopShadowQueues( )
{
  for(int n = 0; n < _nodes; ++n) {
    for(int c = 0; c < _classes; ++c) {
      deque<FlowId> & shadow = _shadow_queues[n][c];
      size_t real = _partial_packets[n][c].size();
      while(shadow.size() > real) {
        FlowId id = shadow.front();
        shadow.pop_front();
        assert(_queued_flits > 0);
        --_queued_flits;

        FlowRecord * fr = _Lookup(id);
        if(!fr) continue;

        assert(fr->flits_queued > 0);
        --fr->flits_queued;

        // Every flit of this flow has now been handed to the network: the
        // source is done transmitting. This is the analogue of ns-3's
        // send_finish, and it is what ASTRA-sim's sim_send callback expects.
        if((fr->packets_made == fr->packets_total) && (fr->flits_queued == 0)) {
          fr->t_sent = (unsigned long long)_time;
          if(_on_send) {
            _on_send(fr->id, fr->user, (unsigned long long)_time, _on_send_ctx);
          }
        }
      }
    }
  }
}

void FlowTrafficManager::_RetireFlit( Flit * f, int dest )
{
  if(f->tail && f->lf_unit >= 0) {
    _lf.PacketRetired(f->lf_unit);
  }
  if(f->tail && _check_path_order) {
    // pids grow in generation order, so packets of one (source,
    // destination, path) must retire with growing pids.
    int & last = _last_pid_on_path[make_pair(make_pair(f->src, dest), f->path_key)];
    if(f->pid < last) {
      ostringstream err;
      err << "check_path_order: packet " << f->pid << " (" << f->src << "->" << dest
          << ", path key " << f->path_key << ") retired after packet " << last;
      Error(err.str());
    }
    last = f->pid;
  }
  if(f->head) _head_itime[f->pid] = f->itime;
  if(f->tail) {
    map<int, int>::iterator h = _head_itime.find(f->pid);
    if(h == _head_itime.end()) Error("_RetireFlit: tail without a retired head");
    ++_tlp_lat_net[f->atime - h->second];
    ++_tlp_lat_total[f->atime - f->ctime];
    ++_tlp_count;
    _tlp_switches += f->hops;
    _head_itime.erase(h);
  }
  if(f->tail) {
    // Read the flow id before chaining: the base implementation frees f.
    FlowId id = (FlowId)(uintptr_t)f->data;
    FlowRecord * fr = _Lookup(id);
    if(fr) {
      fr->hops_total += f->hops;
      ++fr->packets_done;
      if(fr->packets_done == fr->packets_total) {
        fr->t_done = (unsigned long long)_time;
        _completed_this_cycle.push_back(id);
      }
    }
  }
  assert(_outstanding_flits > 0);
  --_outstanding_flits;
  _retired_this_cycle = true;

  TrafficManager::_RetireFlit(f, dest);

  // The base class queues a reply for every retired READ_REQUEST and
  // WRITE_REQUEST packet (Posted and Non-Posted flows), for its own
  // request/reply generator. Flow mode never issues those replies, so the
  // queue would grow by one record per TLP for the whole run.
  list<PacketReplyInfo *> & pending = _repliesPending[dest];
  while(!pending.empty()) {
    pending.front()->Free();
    pending.pop_front();
  }
}

// Callbacks run here, after the cycle is complete, never from _RetireFlit.
// A callback may synchronously call InjectFlow(); the new flow lands in
// _pending_flows and is serviced on the next cycle. That 1 ns quantization is
// irrelevant against 100+ ns switch hops.
void FlowTrafficManager::_DrainCompletions( )
{
  if(_completed_this_cycle.empty()) return;

  vector<FlowId> done;
  done.swap(_completed_this_cycle);

  for(size_t i = 0; i < done.size(); ++i) {
    FlowRecord * fr = _Lookup(done[i]);
    if(!fr) continue;

    assert(_flows_outstanding > 0);
    --_flows_outstanding;

    if(_on_recv) {
      _on_recv(fr->id, fr->user, (unsigned long long)_time, _on_recv_ctx);
    }

    // Report and free the record. Removing it from _flows means a late
    // duplicate tail (which would be a bug) is ignored rather than
    // double-counted.
    _flows.erase(fr->id);
    _units_retired += fr->units_started;
    if(_flow_csv) _WriteFlowCsvRow(*_flow_csv, fr);
    delete fr;
  }
}

void FlowTrafficManager::Step( )
{
  _ServiceFlowQueues();
  TrafficManager::_Step();
  _PopShadowQueues();
  _DrainCompletions();
  if(_loopfree && _lf_check_interval > 0 && (_time % _lf_check_interval) == 0) {
    _LfCheck();
  }

  // A deadlocked network keeps every router busy re-arbitrating while nothing
  // moves, so without this the caller spins until its own (much coarser)
  // stall limit. Any delivery anywhere resets the count, so a merely
  // congested network never trips it.
  if(_outstanding_flits == 0 || _retired_this_cycle) {
    _stall_cycles = 0;
  } else {
    ++_stall_cycles;
  }
  _retired_this_cycle = false;
  if(_deadlock_abort_cycles > 0 && _stall_cycles >= _deadlock_abort_cycles) {
    ostringstream err;
    err << "routing deadlock: no flit delivered in " << _stall_cycles
        << " cycles with " << _outstanding_flits << " flits of "
        << _flows_outstanding << " flows outstanding (cycle " << _time
        << "). On a topology with cycles use pcinet_deadlock_avoidance = "
        << "auto, or loopfree with routing_function = pinned (required for "
        << "pcinet_cross_domain_paths = per_bridge); raise deadlock_abort_cycles "
        << "only if links are slow enough that one packet legitimately takes "
        << "that long.";
    Error(err.str());
  }
}

bool FlowTrafficManager::IsIdle( ) const
{
  // _flows_outstanding is load-bearing, not redundant. A flow accepted by
  // InjectFlow() sits in _pending_flows until _ServiceFlowQueues() packetizes
  // it at the top of the next Step(), so between those two points it has no
  // flits anywhere and the other three counters are all zero. Without this
  // term IsIdle() reports idle while a flow is waiting to start, and a caller
  // using AdvanceTo() to skip to its next event silently jumps the clock past
  // the entire transfer.
  return (_flows_outstanding == 0) &&
         (_outstanding_flits == 0) &&
         (_queued_flits == 0) &&
         (Credit::OutStanding() == 0);
}

void FlowTrafficManager::AdvanceTo( unsigned long long t )
{
  // Safe only when every channel wait queue and router pipeline is empty.
  // Those are keyed on absolute time (Channel::WriteOutputs asserts the item is
  // due exactly now), so skipping cycles with anything in flight would fire
  // those assertions. IsIdle() is exactly that condition -- it is the same
  // invariant TrafficManager::Run() relies on in its drain loops.
  assert(IsIdle());
  assert(t >= (unsigned long long)_time);
  _time = (int)t;
}

void FlowTrafficManager::SetFlowCsv( ostream * os )
{
  _flow_csv = os;
  if(_flow_csv) {
    *_flow_csv << "flow,src,dst,bytes,packets,t_inject,t_sent,t_done,mean_hops,trace_id"
               << (_pinned ? ",paths_used" : "") << "\n";
  }
}

void FlowTrafficManager::_WriteFlowCsvRow( ostream & os, const FlowRecord * fr ) const
{
  double mean_hops = fr->packets_total
    ? (double)fr->hops_total / (double)fr->packets_total : 0.0;
  // `user` is an opaque caller tag (see FlowDesc); booksim_flowtest stuffs
  // the trace file's own flow id in there so a merged multi-collective run
  // can be split back apart per constituent trace.
  os << fr->id << "," << fr->src << "," << fr->dst << "," << fr->bytes << ","
     << fr->packets_total << "," << fr->t_inject << "," << fr->t_sent << ","
     << fr->t_done << "," << mean_hops << ","
     << reinterpret_cast<intptr_t>(fr->user);
  if(_pinned) {
    os << "," << __builtin_popcountll(fr->paths_used);
  }
  os << "\n";
}

bool FlowTrafficManager::PcieCreditsIdle( ) const
{
  for(int s = 0; s < _subnets; ++s) {
    for(int n = 0; n < _nodes; ++n) {
      if(!_buf_states[n][s]->PcieCreditsIdle()) return false;
    }
    const vector<Router *> & routers = _net[s]->GetRouters();
    for(size_t r = 0; r < routers.size(); ++r) {
      if(!routers[r]->PcieCreditsIdle()) return false;
    }
  }
  return true;
}

namespace {

// Mean, nearest-rank 50th and 99th percentiles, and maximum of a histogram.
void PrintHistogram( ostream & os, const string & name,
                     const map<int, unsigned long long> & h, unsigned long long n )
{
  double sum = 0.0;
  for(map<int, unsigned long long>::const_iterator i = h.begin(); i != h.end(); ++i) {
    sum += (double)i->first * (double)i->second;
  }
  const double ps[2] = { 0.50, 0.99 };
  int pv[2] = { 0, 0 };
  for(int k = 0; k < 2; ++k) {
    unsigned long long rank = (unsigned long long)ceil(ps[k] * (double)n);
    if(rank == 0) rank = 1;
    unsigned long long seen = 0;
    for(map<int, unsigned long long>::const_iterator i = h.begin(); i != h.end(); ++i) {
      seen += i->second;
      if(seen >= rank) { pv[k] = i->first; break; }
    }
  }
  os << left << setw(21) << (name + "_mean") << sum / (double)n << endl;
  os << left << setw(21) << (name + "_p50") << pv[0] << endl;
  os << left << setw(21) << (name + "_p99") << pv[1] << endl;
  os << left << setw(21) << (name + "_max") << h.rbegin()->first << endl;
}

} // namespace

void FlowTrafficManager::DumpPacketStats( ostream & os ) const
{
  os << "tlp_count            " << _tlp_count << endl;
  if(_tlp_count == 0) return;
  os << "tlp_switches_mean    " << (double)_tlp_switches / (double)_tlp_count << endl;
  PrintHistogram(os, "tlp_lat_net", _tlp_lat_net, _tlp_count);
  PrintHistogram(os, "tlp_lat_total", _tlp_lat_total, _tlp_count);
}

void FlowTrafficManager::DumpMultipathStats( ostream & os ) const
{
  if(!_pinned) return;
  os << "mp_units             " << _units_retired << endl;
  os << "mp_units_by_key     ";
  for(size_t k = 0; k < _units_by_key.size(); ++k) os << " " << _units_by_key[k];
  os << endl;
  if(!_loopfree) return;
  _LfCheck();
  os << "lf_units             " << _lf_admitted << endl;
  os << "lf_waited_units      " << _lf_waited_units << endl;
  os << "lf_wait_cycles       " << _lf_wait_cycles << endl;
  os << "lf_max_wait          " << _lf_max_wait << endl;
  os << "lf_alt_path          " << _lf_alt_path << endl;
  os << "lf_live_units        " << _lf.LiveUnits() << endl;
  os << "lf_live_turn_refs    " << _lf.LiveRefs() << endl;
}
