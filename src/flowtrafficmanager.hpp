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

#ifndef _FLOWTRAFFICMANAGER_HPP_
#define _FLOWTRAFFICMANAGER_HPP_

#include <deque>
#include <map>
#include <vector>

#include "trafficmanager.hpp"
#include "turn_graph.hpp"

class PCINet;

// An externally driven TrafficManager.
//
// Stock BookSim generates its own traffic from a TrafficPattern and runs its own
// convergence loop. Neither is wanted here: the workload is a list of explicit
// (src, dst, bytes) transfers with dependencies between them -- a collective
// communication schedule -- and the caller owns the clock so it can interleave
// BookSim cycles with another simulator's event queue.
//
// So this subclass:
//   * disables synthetic injection entirely (_empty_network),
//   * exposes Step() to advance exactly one cycle,
//   * accepts flows via InjectFlow() and packetizes them lazily,
//   * reports two completion events per flow, matching what ASTRA-sim's
//     AstraNetworkAPI expects: "the last byte left the source" (send) and
//     "the last byte arrived at the destination" (recv).
//
// BookSim knows only packets and flits; "flow" is entirely this class's notion.
class FlowTrafficManager : public TrafficManager {

public:

  typedef unsigned long long FlowId;
  static const FlowId kInvalidFlow = ~0ull;

  // Fired after the cycle in which the event happened, never from inside
  // _Step(). See _DrainCompletions() for why.
  typedef void (*FlowCallback)(FlowId id, void * user, unsigned long long time,
                               void * ctx);

  FlowTrafficManager( const Configuration & config, const vector<Network *> & net );
  virtual ~FlowTrafficManager( );

  // Advance the simulation by exactly one cycle, then deliver any completions
  // that cycle produced.
  void Step( );

  // Jump the clock forward. Only legal while IsIdle(), because BookSim's channel
  // and router pipelines are keyed on absolute time and would trip their
  // "arrived exactly now" assertions if time skipped past a queued item.
  void AdvanceTo( unsigned long long t );

  // Queue a transfer. Returns immediately; the flow is packetized on demand as
  // the source's injection queue drains. `pcie_class` is 0 (unset -- the full,
  // undivided VC pool, as stock BookSim), 1 (Posted), 2
  // (Non-Posted), or 3 (Completion); see _GenerateFlowPacket for how this maps
  // onto Flit::FlitType and thus which VC-range/credit pool a flow's flits use.
  FlowId InjectFlow( int src, int dst, unsigned long long bytes, int cls,
                     int pcie_class, void * user );

  // True when nothing is queued, nothing is in flight, and no credits are
  // outstanding -- i.e. every BookSim pipeline is empty. O(1).
  bool IsIdle( ) const;

  unsigned long long FlowsOutstanding( ) const { return _flows_outstanding; }
  unsigned long long Now( ) const { return (unsigned long long)_time; }

  void SetOnSendComplete( FlowCallback cb, void * ctx ) {
    _on_send = cb; _on_send_ctx = ctx;
  }
  void SetOnRecvComplete( FlowCallback cb, void * ctx ) {
    _on_recv = cb; _on_recv_ctx = ctx;
  }

  // Stream the per-flow CSV: the header now, then one row per flow as it
  // completes, in completion order:
  //   flow,src,dst,bytes,packets,t_inject,t_sent,t_done,mean_hops,trace_id
  //   (+ paths_used under routing_function = pinned)
  // Call before injecting flows. A completed flow's record is freed as soon
  // as its row is written, or at once with no stream set, so memory does not
  // grow with the number of completed flows.
  void SetFlowCsv( ostream * os );

  // Multipath settings and, under loopfree, the admission statistics.
  void DumpMultipathStats( ostream & os ) const;

  // Per-TLP latency and switch-count statistics over every packet retired.
  void DumpPacketStats( ostream & os ) const;

  // pcie_credits = 1: have all header and data credits come back, at every
  // source and switch output? Meaningful once IsIdle().
  bool PcieCreditsIdle( ) const;
  bool PcieCreditsEnabled( ) const { return _pcie_credits; }

protected:

  struct FlowRecord {
    FlowId             id;
    int                src;
    int                dst;
    int                cls;
    int                pcie_class;   // 0..3; see InjectFlow
    unsigned long long bytes;
    void *             user;

    unsigned long long packets_total;
    unsigned long long packets_made;    // handed to _partial_packets so far
    unsigned long long packets_done;    // tail flit retired at the destination
    unsigned long long bytes_remaining; // still to be packetized
    unsigned long long flits_queued;    // made but not yet handed to the network

    unsigned long long t_inject;        // InjectFlow() was called
    unsigned long long t_sent;          // last flit left the source
    unsigned long long t_done;          // last tail flit retired
    unsigned long long hops_total;      // summed over packets, for the mean

    // Pinned multipath (routing_function = pinned).
    int                cur_path_key;    // key of the unit being packetized
    unsigned long long units_started;   // requests (or packets) begun so far
    unsigned long long paths_used;      // bitmask over path indices 0..63
    int                lf_unit;         // loop-free unit being generated, -1 if none
    long long          lf_wait_since;   // cycle admission was first refused, -1 if not waiting
  };

  // Never fire a user callback from inside _RetireFlit: it runs mid-cycle,
  // before Network::Evaluate()/WriteOutputs() and before _time is incremented,
  // and a callback that re-entered InjectFlow() would mutate _partial_packets
  // while _Step() is iterating it.
  virtual void _RetireFlit( Flit * f, int dest );

  // Trap: the convergence loop must never run. Run() is non-virtual so it
  // cannot be overridden, but it reaches _SingleSim(), which can.
  virtual bool _SingleSim( );

  // Belt and braces. _Inject() is already suppressed via _empty_network.
  virtual int _IssuePacket( int source, int cl ) { return 0; }

private:

  void _ServiceFlowQueues( );
  void _ServiceSource( int n, int c );
  // Earliest refused-admission cycle among (n, c)'s pending flows, -1 if none.
  long long _OldestWait( int n, int c ) const;
  bool _UnitStartsHere( const FlowRecord * fr ) const;
  // false: loop-free admission refused every path; retry next cycle.
  bool _StartUnit( FlowRecord * fr );
  void _LfCheck( ) const;
  void _GenerateFlowPacket( FlowRecord * fr, int size_flits, int payload );
  void _PopShadowQueues( );
  void _DrainCompletions( );
  FlowRecord * _Lookup( FlowId id );

  // Validates, the first time a given pcie_class (1..3) is actually injected,
  // that its VC range fits within num_vcs and that buffer_policy is
  // "private" -- see the comment above _buffer_policy in the constructor for
  // why this can't be an eager, construction-time check. Error()s (aborts)
  // on failure; a no-op on every subsequent call for the same class.
  void _CheckPcieClassVcRange( int pcie_class );

  // Flows waiting to be packetized, per (source, class), in issue order.
  vector<vector<deque<FlowRecord *> > > _pending_flows;

  // source_interleave = tlp: a source takes one TLP from each of its pending
  // flows in turn (as a GPU's concurrent NCCL channels do), starting at
  // _rr_next[n][c]. flow: only the front flow, until it is fully packetized.
  bool _interleave_tlp;
  vector<vector<size_t> > _rr_next;

  // One entry per flit currently sitting in _partial_packets[src][cl], holding
  // the flow that flit belongs to. _Step() pops at most one flit per (src,cl)
  // per cycle from the front, so comparing sizes after the step tells us
  // exactly how many left -- which is how "the last byte left the source" is
  // detected without modifying the non-virtual _Step().
  vector<vector<deque<FlowId> > > _shadow_queues;

  map<FlowId, FlowRecord *> _flows;
  ostream *                 _flow_csv;          // SetFlowCsv, NULL = none
  unsigned long long        _units_retired;     // units_started of completed flows
  void _WriteFlowCsvRow( ostream & os, const FlowRecord * fr ) const;

  vector<FlowId> _completed_this_cycle;

  FlowId             _next_flow_id;
  unsigned long long _flows_outstanding;

  // O(1) IsIdle(): maintained rather than recomputed, because a long collective
  // is millions of cycles and a per-cycle scan over nodes dominates runtime.
  unsigned long long _outstanding_flits;
  unsigned long long _queued_flits;

  // Packetization, in bytes.
  int _flit_bytes;
  int _mps;              // PCIe Max Payload Size
  int _tlp_overhead;     // header + sequence + LCRC + framing
  int _flits_per_packet; // derived: ceil((mps + overhead) / flit_bytes)

  // How many packets to keep queued per (src,cl). Bounds memory: a 1 GB flow is
  // millions of packets and must not be materialized up front.
  int _max_queued_packets;

  // P/NP/Cpl lazy validation state -- see _CheckPcieClassVcRange.
  string _buffer_policy;
  bool   _pcie_class_checked[4];
  string _pools_vs_types;        // CheckPoolsMatchTypes for shared policies
  int    _pcie_default_class;    // class given to untagged flows, 0 = none
  bool   _pcie_credits;          // pcie_credits = 1: packets carry PCIe metadata

  // check_path_order: last packet id retired per (src, dst, path key).
  bool _check_path_order;
  map<pair<pair<int, int>, int>, int> _last_pid_on_path;

  // Deadlock watchdog -- see Step(). Counts consecutive cycles with flits
  // outstanding and none delivered.
  int  _deadlock_abort_cycles;   // 0 == disabled
  int  _stall_cycles;
  bool _retired_this_cycle;

  // Pinned multipath (routing_function = pinned): the source picks one of
  // PCINet's paths per unit (TLP, DMA request or flow).
  enum Granularity { kGranPacket, kGranRequest, kGranFlow };
  PCINet *    _pcinet;            // NULL unless topology = pcinet
  bool        _pinned;            // routing_function = pinned
  Granularity _granularity;
  int         _packets_per_request;
  enum Select { kSelRoundRobin, kSelHash, kSelWeighted };
  Select      _select;            // multipath_select
  // [src node][dst node] round-robin counters.
  vector<vector<unsigned long long> > _next_path_key;
  // multipath_select = weighted: smooth weighted round robin state per
  // (src node, dst node), one current weight per path.
  struct Swrr { vector<long long> w, cw; };   // weights, current weights
  map<pair<int, int>, Swrr> _swrr;
  vector<unsigned long long> _units_by_key;   // units started, by path key
  Swrr & _SwrrState( int src, int dst, int npaths );
  int  _WeightedPick( int src, int dst, int npaths );
  void _WeightedCommit( int src, int dst, int npaths, int key );

  // Loop-free path selection (pcinet_deadlock_avoidance = loopfree): admit a
  // path only if it keeps the in-flight turn graph acyclic. A unit's pass-throughs are registered before its first
  // packet is queued and released exactly once, when it is fully generated
  // and its last outstanding tail has retired (LoopFreeUnits).
  bool _loopfree;
  int  _lf_check_interval;          // debug full check every N cycles, 0 = off
  bool _lf_oldest_first;            // loopfree_retry_order = oldest_first
  LoopFreeUnits _lf;                // one turn graph per credit pool
  int _lf_pool_of_class[4];         // pcie_class -> pool
  unsigned long long _lf_admitted;      // units with at least one pass-through
  unsigned long long _lf_waited_units;
  unsigned long long _lf_wait_cycles;
  unsigned long long _lf_max_wait;
  unsigned long long _lf_alt_path;      // units that took a non-first-choice path

  // Per-TLP statistics, kept as histograms so memory does not grow with run
  // length. Network latency runs from the head's injection to the tail's
  // ejection; total latency from packet creation (queued at the source).
  map<int, int> _head_itime;                        // pid -> head injection cycle
  map<int, unsigned long long> _tlp_lat_net;        // cycles -> packets
  map<int, unsigned long long> _tlp_lat_total;
  unsigned long long _tlp_count;
  unsigned long long _tlp_switches;                 // sum of routers traversed

  FlowCallback _on_send;  void * _on_send_ctx;
  FlowCallback _on_recv;  void * _on_recv_ctx;
};

// Exact positive integer weights inversely proportional to cycles per flit
// (multipath_select = weighted). False if a cpf is not positive or the
// weights would exceed 2^40.
bool SwrrWeights( const vector<int> & cpf, vector<long long> & w );

#endif
