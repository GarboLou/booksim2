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

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>

#include "flowtrafficmanager.hpp"
#include "credit.hpp"
#include "globals.hpp"
#include "random_utils.hpp"
#include "routefunc.hpp"  // gNumVCs, g{Read,Write}Req{Begin,End}VC, gReadReplyBeginVC/EndVC

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

  // P/NP/Cpl credit-pool separation (see PCIE_MODEL_LIMITATIONS.md): a flow
  // tagged pcie_class=1/2/3 gets Flit::WRITE_REQUEST/READ_REQUEST/READ_REPLY,
  // which routes through PCINet::apply_vc_range() into the VC range the
  // matching g*BeginVC/g*EndVC pair names, and needs buffer_policy="private"
  // for those ranges' credit pools to be genuinely independent (see
  // _CheckPcieClassVcRange). Deliberately NOT validated eagerly here, unlike
  // the checks above: BookSim's stock config defaults for those VC-range
  // globals are concrete, non-negative values sized for num_vcs=16 (e.g.
  // write_request_end_vc defaults to 7), not the -1 "unconfigured" sentinel
  // -- every existing config in this project inherits those defaults
  // unchanged and uses num_vcs=4, so an eager check here would reject every
  // config that has never even heard of pcie_class. Checked lazily instead,
  // in InjectFlow, only the first time each class actually gets used.
  _buffer_policy = config.GetStr("buffer_policy");
  for(int i = 0; i < 4; ++i) _pcie_class_checked[i] = false;

  // A full-size TLP costs its payload plus the per-TLP overhead. Charging the
  // overhead as real flits (rather than derating the link rate) is what makes
  // small transfers correctly more expensive per byte than large ones, which is
  // where fabric topology differences actually show up.
  _flits_per_packet = (_mps + _tlp_overhead + _flit_bytes - 1) / _flit_bytes;

  _pending_flows.resize(_nodes);
  _shadow_queues.resize(_nodes);
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
  for(size_t i = 0; i < _retired_flows.size(); ++i) {
    delete _retired_flows[i];
  }
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
  if(pcie_class != 0) _CheckPcieClassVcRange(pcie_class);

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

  _flows[fr->id] = fr;
  _pending_flows[src][cls].push_back(fr);
  ++_flows_outstanding;

  return fr->id;
}

void FlowTrafficManager::_CheckPcieClassVcRange( int pcie_class )
{
  if(_pcie_class_checked[pcie_class]) return;
  _pcie_class_checked[pcie_class] = true;

  // buffer_policy is checked on first use of ANY class, not per-class: it's a
  // single global setting, not one this project could sensibly vary per class.
  if(_buffer_policy != "private") {
    ostringstream err;
    err << "FlowTrafficManager: pcie_class=" << pcie_class << " requires "
        << "buffer_policy = private; (got " << _buffer_policy << ") -- only "
        << "the private policy gives each VC range a fully independent "
        << "credit pool; shared (and the other feedback policies) share "
        << "buffer space across VC ranges, silently defeating the separation.";
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
}

// Top up each source's injection queue. Deliberately lazy: a 1 GB flow is
// millions of packets and materializing them all would exhaust memory long
// before the simulation finished.
void FlowTrafficManager::_ServiceFlowQueues( )
{
  for(int n = 0; n < _nodes; ++n) {
    for(int c = 0; c < _classes; ++c) {
      deque<FlowRecord *> & pending = _pending_flows[n][c];
      while(!pending.empty() &&
            (int)_partial_packets[n][c].size() < _max_queued_packets * _flits_per_packet) {
        FlowRecord * fr = pending.front();

        // Last packet of a flow carries whatever is left, so a 64 B transfer
        // costs one small packet rather than a full MPS-sized one.
        unsigned long long payload = (unsigned long long)_mps;
        if(fr->bytes_remaining < payload) payload = fr->bytes_remaining;

        int size_flits = (int)((payload + (unsigned long long)_tlp_overhead
                                + (unsigned long long)_flit_bytes - 1)
                               / (unsigned long long)_flit_bytes);
        if(size_flits < 1) size_flits = 1;

        _GenerateFlowPacket(fr, size_flits);

        fr->bytes_remaining -= payload;
        ++fr->packets_made;
        if(fr->packets_made == fr->packets_total) {
          assert(fr->bytes_remaining == 0);
          pending.pop_front();
        }
      }
    }
  }
}

// Adapted from TrafficManager::_GeneratePacket. That method is non-virtual and
// takes its destination from _traffic_pattern, which we do not use, so the body
// is reproduced here with the destination supplied explicitly.
//
// f->type is set from fr->pcie_class (see FlowRecord/InjectFlow) rather than
// always Flit::ANY_TYPE: this is what feeds PCINet::apply_vc_range()'s VC-range
// selection and thus the P/NP/Cpl credit-pool separation (PCIE_MODEL_LIMITATIONS.md).
// `subnetwork` below deliberately stays indexed by Flit::ANY_TYPE regardless of
// pcie_class -- the four `*_subnet` config keys all default to 0 and nothing in
// this project sets `subnets` above its default of 1, so per-type subnetwork
// routing is out of scope; if that ever changes, this must be revisited too.
// Every member touched below is protected in TrafficManager.
void FlowTrafficManager::_GenerateFlowPacket( FlowRecord * fr, int size_flits )
{
  static const Flit::FlitType kPcieClassToFlitType[4] = {
    Flit::ANY_TYPE,       // 0: unset -- full VC pool, pre-existing behavior
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

  TrafficManager::_RetireFlit(f, dest);
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

    // Retire the record but keep it for reporting. Removing it from _flows
    // means a late duplicate tail (which would be a bug) is ignored rather
    // than double-counted.
    _flows.erase(fr->id);
    _retired_flows.push_back(fr);
  }
}

void FlowTrafficManager::Step( )
{
  _ServiceFlowQueues();
  TrafficManager::_Step();
  _PopShadowQueues();
  _DrainCompletions();
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

void FlowTrafficManager::DumpFlowCsv( ostream & os ) const
{
  os << "flow,src,dst,bytes,packets,t_inject,t_sent,t_done,mean_hops,trace_id"
     << endl;
  for(size_t i = 0; i < _retired_flows.size(); ++i) {
    const FlowRecord * fr = _retired_flows[i];
    double mean_hops = fr->packets_total
      ? (double)fr->hops_total / (double)fr->packets_total : 0.0;
    // `user` is an opaque caller tag (see FlowDesc); booksim_flowtest stuffs
    // the trace file's own flow id in there so a merged multi-collective run
    // can be split back apart per constituent trace.
    os << fr->id << "," << fr->src << "," << fr->dst << "," << fr->bytes << ","
       << fr->packets_total << "," << fr->t_inject << "," << fr->t_sent << ","
       << fr->t_done << "," << mean_hops << ","
       << reinterpret_cast<intptr_t>(fr->user) << endl;
  }
}
