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

#ifndef _FLOWTRAFFICMANAGER_HPP_
#define _FLOWTRAFFICMANAGER_HPP_

#include <deque>
#include <map>
#include <vector>

#include "trafficmanager.hpp"

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
  // undivided VC pool, matching pre-existing behavior), 1 (Posted), 2
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

  // flow,src,dst,bytes,packets,t_inject,t_sent,t_done,mean_hops,trace_id
  void DumpFlowCsv( ostream & os ) const;

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
  void _GenerateFlowPacket( FlowRecord * fr, int size_flits );
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

  // One entry per flit currently sitting in _partial_packets[src][cl], holding
  // the flow that flit belongs to. _Step() pops at most one flit per (src,cl)
  // per cycle from the front, so comparing sizes after the step tells us
  // exactly how many left -- which is how "the last byte left the source" is
  // detected without modifying the non-virtual _Step().
  vector<vector<deque<FlowId> > > _shadow_queues;

  map<FlowId, FlowRecord *> _flows;
  vector<FlowRecord *>      _retired_flows;   // kept for DumpFlowCsv

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

  FlowCallback _on_send;  void * _on_send_ctx;
  FlowCallback _on_recv;  void * _on_recv_ctx;
};

#endif
