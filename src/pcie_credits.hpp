// $Id$

/*
 Copyright (c) 2026, Jiaqi Lou
 All rights reserved. Distributed under the same BSD license as BookSim; see
 LICENSE.md.
*/

// PCIe VC0 credit model.
//
// Real PCIe VC0 has, per link direction, separate Posted / Non-Posted /
// Completion pools of header credits (one per TLP) and data credits (16 B
// each). Here each type owns a contiguous BookSim VC range (one internal
// queue per egress under NOQ) whose flit storage is one shared pool, and,
// with pcie_credits = 1, whole TLPs are admitted against header and data
// credits kept separately from that physical flit accounting.

#ifndef _PCIE_CREDITS_HPP_
#define _PCIE_CREDITS_HPP_

#include <string>
#include <vector>

#include "config_utils.hpp"

using namespace std;

enum { PCIE_P = 0, PCIE_NP = 1, PCIE_CPL = 2, PCIE_NUM_TYPES = 3 };

const char * PcieTypeName( int type );

// Flit::FlitType -> PCIe type: WRITE_REQUEST is Posted, READ_REQUEST
// Non-Posted, READ_REPLY Completion; -1 for anything else.
int PcieTypeOfFlit( int flit_type );

// The [begin, end] VC range configured for a type (write_request_begin_vc ...).
void PcieTypeVcRange( const Configuration & config, int type, int & begin, int & end );

// The private pools buffer_policy = shared (or limited) builds, computed the
// same way BufferState::SharedBufferPolicy does.
struct SharedPoolLayout {
  int buf_size;
  vector<int> size;
  vector<int> start_vc;
  vector<int> end_vc;
  int shared;      // common pool: buf_size minus every private pool
};
SharedPoolLayout ReadSharedPoolLayout( const Configuration & config );

// Empty if every private pool covers exactly one type's VC range and the
// common pool is 0, so storage never spans types; otherwise the reason.
string CheckPoolsMatchTypes( const Configuration & config );

// Protocol credits per type; -1 == infinite.
struct PcieCreditLimits {
  bool enabled;                   // pcie_credits = 1
  int  hdr[PCIE_NUM_TYPES];       // header credits (TLPs)
  int  data[PCIE_NUM_TYPES];      // data credits (16 B units)
};
PcieCreditLimits ReadPcieCreditLimits( const Configuration & config );

// Data credits a payload consumes: one per started 16 B.
inline int PcieDataUnits( int payload_bytes ) { return (payload_bytes + 15) / 16; }

// How a TLP's data credits come back as its flits leave an input queue. The
// overhead sits at the front of the first flit and the payload follows; flit
// i returns the 16 B units its bytes complete, and the tail returns the rest,
// including the final partial unit. Sums to PcieDataUnits(payload_bytes).
vector<int> PcieDataReturns( int payload_bytes, int overhead_bytes, int flit_bytes,
                             int flits );

// pcie_credits = 1: validate, then derive the storage keys (private_bufs,
// private_buf_size/start_vc/end_vc, buf_size) so each type's flit storage
// holds whatever its credits can admit and never binds first. Prints the
// storage budget. Exits with a message on an invalid or conflicting
// configuration. Does nothing when pcie_credits = 0.
void ApplyPcieCreditConfig( Configuration & config );

// Flit storage that can never bind before the protocol credits do. Exposed
// for the unit test.
int PcieStorageFlits( int hdr, int data_units, int mps, int overhead_bytes, int flit_bytes );

#endif
