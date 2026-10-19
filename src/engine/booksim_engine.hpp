// $Id$

/*
 Copyright (c) 2026, Jiaqi Lou
 All rights reserved. Distributed under the same BSD license as BookSim; see
 LICENSE.md.
*/

#ifndef _BOOKSIM_ENGINE_HPP_
#define _BOOKSIM_ENGINE_HPP_

#include <cstdint>
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

// Embeddable front door to BookSim.
//
// Deliberately exposes no BookSim types: booksim.hpp puts "using namespace std"
// at global scope and globals.hpp declares a bare ::GetSimTime(), neither of
// which should leak into a host program's translation units (in particular
// ASTRA-sim's, which has its own notion of simulation time).
//
// One Engine per process. BookSim keeps the live TrafficManager, the Flit and
// Credit free pools, the routing function map and the topology globals in
// process-wide state, so a second concurrent instance is not supported.
// Sequential create/destroy/create is.
namespace booksim {

typedef uint64_t FlowId;
extern const FlowId kInvalidFlow;

struct FlowDesc {
  int      src;    // BookSim node id
  int      dst;
  uint64_t bytes;
  int      cls;    // traffic class; 0 unless the config defines more
  // PCIe ordering class, for the VC-range/credit-pool separation that keeps
  // a flood of one category from starving another. It separates credit pools
  // but does not enforce PCIe's transaction ordering rules.
  //   0 = unset (gets the full, undivided VC pool, as stock BookSim)
  //   1 = Posted     (maps to Flit::WRITE_REQUEST)
  //   2 = Non-Posted (maps to Flit::READ_REQUEST)
  //   3 = Completion (maps to Flit::READ_REPLY)
  int      pcie_class;
  void *   user;   // opaque, echoed back to the callbacks verbatim
  FlowDesc() : src(-1), dst(-1), bytes(0), cls(0), pcie_class(0), user(0) {}
};

// time_ns is the engine clock at which the event occurred. ctx is the cookie
// supplied at registration.
typedef void (*FlowCb)(FlowId id, void * user, uint64_t time_ns, void * ctx);

class Engine {
public:
  // Builds the network and traffic manager from a BookSim config file.
  // `overrides` are applied after parsing, as (key, value) pairs in the same
  // syntax the file uses. Returns NULL on failure; note that BookSim reports
  // most configuration errors by terminating the process.
  static Engine * Create(const std::string & config_file,
                         const std::vector<std::pair<std::string, std::string> > & overrides);
  ~Engine();

  int      NumNodes() const;
  uint64_t Time() const;    // cycles; one cycle is ns_per_cycle nanoseconds
  bool     IsIdle() const;  // nothing queued, in flight, or awaiting credit

  void     Step();                  // advance exactly one cycle
  void     AdvanceTo(uint64_t t);   // clock jump; requires IsIdle()

  FlowId   InjectFlow(const FlowDesc & d);

  // Fired after the cycle in which they occur, never re-entrantly from inside
  // a cycle. A callback may call InjectFlow(); the flow starts the next cycle.
  void     SetOnSendComplete(FlowCb cb, void * ctx);  // last byte left the source
  void     SetOnRecvComplete(FlowCb cb, void * ctx);  // last byte reached the sink

  uint64_t FlowsOutstanding() const;

  void     DumpStats(std::ostream & os) const;
  // Per-flow CSV, streamed: header now, one row per flow as it completes.
  // Call before injecting flows; os must outlive the Engine or a later
  // SetFlowCsv(NULL). Completed flows are not retained, CSV or not.
  void     SetFlowCsv(std::ostream * os);
  // Pinned-multipath unit counts and loop-free admission statistics.
  void     DumpMultipathStats(std::ostream & os) const;
  // Per-TLP latency (network and total) and switches-traversed statistics.
  void     DumpPacketStats(std::ostream & os) const;

  // pcie_credits = 1: once idle, every header and data credit must be back.
  bool     PcieCreditsEnabled() const;
  bool     PcieCreditsIdle() const;

private:
  struct Impl;
  Impl * p_;
  Engine();
  Engine(const Engine &);
  Engine & operator=(const Engine &);
};

} // namespace booksim

#endif
