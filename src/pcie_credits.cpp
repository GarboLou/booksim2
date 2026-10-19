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

#include "pcie_credits.hpp"
#include "flit.hpp"

namespace {

// Storage for an infinite pool: large enough never to bind, small enough for
// int arithmetic over three pools.
const int kInfiniteFlits = 1 << 20;

void Fail( const string & msg )
{
  cout << "PCIe credits: " << msg << endl;
  exit(-1);
}

const char * const kCreditKeys[PCIE_NUM_TYPES][2] = {
  { "pcie_ph",   "pcie_pd_bytes"   },
  { "pcie_nph",  "pcie_npd_bytes"  },
  { "pcie_cplh", "pcie_cpld_bytes" },
};

int Flits( int payload, int overhead, int flit_bytes )
{
  return (payload + overhead + flit_bytes - 1) / flit_bytes;
}

string IntArray( const vector<int> & v )
{
  ostringstream os;
  os << "{";
  for(size_t i = 0; i < v.size(); ++i) os << (i ? "," : "") << v[i];
  os << "}";
  return os.str();
}

} // namespace

const char * PcieTypeName( int type )
{
  switch(type) {
  case PCIE_P:   return "Posted";
  case PCIE_NP:  return "Non-Posted";
  case PCIE_CPL: return "Completion";
  }
  return "?";
}

int PcieTypeOfFlit( int flit_type )
{
  switch(flit_type) {
  case Flit::WRITE_REQUEST: return PCIE_P;
  case Flit::READ_REQUEST:  return PCIE_NP;
  case Flit::READ_REPLY:    return PCIE_CPL;
  }
  return -1;
}

void PcieTypeVcRange( const Configuration & config, int type, int & begin, int & end )
{
  switch(type) {
  case PCIE_P:
    begin = config.GetInt("write_request_begin_vc");
    end   = config.GetInt("write_request_end_vc");
    return;
  case PCIE_NP:
    begin = config.GetInt("read_request_begin_vc");
    end   = config.GetInt("read_request_end_vc");
    return;
  case PCIE_CPL:
    begin = config.GetInt("read_reply_begin_vc");
    end   = config.GetInt("read_reply_end_vc");
    return;
  }
  begin = end = -1;
}

SharedPoolLayout ReadSharedPoolLayout( const Configuration & config )
{
  // Mirrors BufferState::SharedBufferPolicy's constructor.
  SharedPoolLayout l;
  const int vcs = config.GetInt("num_vcs");
  int n = config.GetInt("private_bufs");
  if(n < 0) n = vcs;
  else if(n == 0) n = 1;

  l.buf_size = config.GetInt("buf_size");
  if(l.buf_size < 0) l.buf_size = vcs * config.GetInt("vc_buf_size");

  l.size = config.GetIntArray("private_buf_size");
  if(l.size.empty()) {
    const int bs = config.GetInt("private_buf_size");
    l.size.push_back(bs < 0 ? l.buf_size / n : bs);
  }
  l.size.resize(n, l.size.back());

  l.start_vc = config.GetIntArray("private_buf_start_vc");
  if(l.start_vc.empty()) {
    const int sv = config.GetInt("private_buf_start_vc");
    if(sv < 0) {
      for(int i = 0; i < n; ++i) l.start_vc.push_back(i * vcs / n);
    } else {
      l.start_vc.push_back(sv);
    }
  }
  l.end_vc = config.GetIntArray("private_buf_end_vc");
  if(l.end_vc.empty()) {
    const int ev = config.GetInt("private_buf_end_vc");
    if(ev < 0) {
      for(int i = 0; i < n; ++i) l.end_vc.push_back((i + 1) * vcs / n - 1);
    } else {
      l.end_vc.push_back(ev);
    }
  }
  l.shared = l.buf_size;
  for(int i = 0; i < n; ++i) l.shared -= l.size[i];
  return l;
}

string CheckPoolsMatchTypes( const Configuration & config )
{
  const SharedPoolLayout l = ReadSharedPoolLayout(config);
  ostringstream why;
  if(l.shared != 0) {
    why << "the common shared pool is " << l.shared << " flits (buf_size minus the "
        << "private pools), which any type could take; it must be 0";
    return why.str();
  }
  for(size_t i = 0; i < l.size.size(); ++i) {
    if(i >= l.start_vc.size() || i >= l.end_vc.size()) {
      why << "private pool " << i << " has no start/end VC";
      return why.str();
    }
    bool match = false;
    for(int t = 0; t < PCIE_NUM_TYPES; ++t) {
      int b, e;
      PcieTypeVcRange(config, t, b, e);
      if(l.start_vc[i] == b && l.end_vc[i] == e) match = true;
    }
    if(!match) {
      why << "private pool " << i << " covers VCs [" << l.start_vc[i] << ","
          << l.end_vc[i] << "], which is not exactly one type's VC range";
      return why.str();
    }
  }
  return "";
}

PcieCreditLimits ReadPcieCreditLimits( const Configuration & config )
{
  PcieCreditLimits lim;
  lim.enabled = (config.GetInt("pcie_credits") == 1);
  for(int t = 0; t < PCIE_NUM_TYPES; ++t) {
    const int h = config.GetInt(kCreditKeys[t][0]);
    const int d = config.GetInt(kCreditKeys[t][1]);
    lim.hdr[t]  = (h == 0) ? -1 : h;
    lim.data[t] = (d == 0) ? -1 : d / 16;
  }
  return lim;
}

vector<int> PcieDataReturns( int payload_bytes, int overhead_bytes, int flit_bytes,
                             int flits )
{
  vector<int> ret(flits, 0);
  int returned = 0;
  for(int i = 0; i < flits; ++i) {
    int units;
    if(i == flits - 1) {
      units = PcieDataUnits(payload_bytes);   // the tail settles the partial unit
    } else {
      int covered = (i + 1) * flit_bytes - overhead_bytes;
      covered = max(0, min(covered, payload_bytes));
      units = covered / 16;
    }
    ret[i] = units - returned;
    returned = units;
  }
  return ret;
}

int PcieStorageFlits( int hdr, int data_units, int mps, int overhead_bytes, int flit_bytes )
{
  // A set of admitted TLPs holds at most hdr * (flits of a full TLP) flits,
  // and at most data_units * max_p(flits(p) / units(p)) flits.
  long long bound = kInfiniteFlits;
  if(hdr >= 0) {
    bound = min(bound, (long long)hdr * Flits(mps, overhead_bytes, flit_bytes));
  }
  if(data_units >= 0) {
    long long by_data = 0;
    for(int p = 1; p <= mps; ++p) {
      const long long f = Flits(p, overhead_bytes, flit_bytes);
      const long long u = PcieDataUnits(p);
      by_data = max(by_data, ((long long)data_units * f + u - 1) / u);
    }
    bound = min(bound, by_data);
  }
  return (int)bound;
}

void ApplyPcieCreditConfig( Configuration & config )
{
  const int enabled = config.GetInt("pcie_credits");
  if(enabled == 0) return;
  if(enabled != 1) Fail("pcie_credits must be 0 or 1");

  const string bp = config.GetStr("buffer_policy");
  if(bp != "shared" && bp != "limited") {
    Fail("pcie_credits = 1 needs buffer_policy = shared (or limited, the capped "
         "variant); got " + bp);
  }
  if(config.GetInt("buf_size") >= 0 || config.GetInt("private_bufs") >= 0 ||
     !config.GetStr("private_buf_size").empty() ||
     config.GetInt("private_buf_size") != 1 ||
     !config.GetStr("private_buf_start_vc").empty() ||
     config.GetInt("private_buf_start_vc") >= 0 ||
     !config.GetStr("private_buf_end_vc").empty() ||
     config.GetInt("private_buf_end_vc") >= 0) {
    Fail("pcie_credits = 1 derives buf_size and the private_buf* keys from the "
         "pcie_* credits; remove them from the config");
  }
  // Two heads granted to one output in the same cycle would both pass the
  // credit check before either reserves; one grant per output avoids that.
  if(config.GetInt("output_speedup") != 1) {
    Fail("pcie_credits = 1 needs output_speedup = 1 (otherwise two TLPs could "
         "claim the same last credits in one cycle)");
  }
  if(config.GetInt("output_buffer_size") <= 0) {
    Fail("pcie_credits = 1 needs a finite output_buffer_size, so a congested "
         "egress eventually stops input dequeue (and with it credit return)");
  }

  const int vcs = config.GetInt("num_vcs");
  int begin[PCIE_NUM_TYPES], end[PCIE_NUM_TYPES];
  for(int t = 0; t < PCIE_NUM_TYPES; ++t) {
    PcieTypeVcRange(config, t, begin[t], end[t]);
    if(begin[t] < 0 || begin[t] > end[t] || end[t] >= vcs) {
      ostringstream os;
      os << PcieTypeName(t) << " VC range [" << begin[t] << "," << end[t]
         << "] is invalid for num_vcs = " << vcs << "; set write_request_*, "
         << "read_request_* and read_reply_*_vc to disjoint ranges";
      Fail(os.str());
    }
    for(int u = 0; u < t; ++u) {
      if(begin[t] <= end[u] && begin[u] <= end[t]) {
        ostringstream os;
        os << PcieTypeName(t) << " and " << PcieTypeName(u) << " VC ranges overlap";
        Fail(os.str());
      }
    }
  }

  const int mps        = config.GetInt("pcie_mps");
  const int overhead   = config.GetInt("pcie_tlp_overhead_bytes");
  const int flit_bytes = config.GetInt("flit_bytes");
  if(mps <= 0 || overhead < 0 || flit_bytes <= 0) {
    Fail("pcie_mps, pcie_tlp_overhead_bytes and flit_bytes must be valid");
  }

  vector<int> size(PCIE_NUM_TYPES), start(PCIE_NUM_TYPES), stop(PCIE_NUM_TYPES);
  ostringstream budget;
  int total = 0;
  for(int t = 0; t < PCIE_NUM_TYPES; ++t) {
    const int h = config.GetInt(kCreditKeys[t][0]);
    const int d = config.GetInt(kCreditKeys[t][1]);
    if(h < 0 || d < 0) Fail(string(kCreditKeys[t][0]) + " / " + kCreditKeys[t][1] +
                            " must be non-negative (0 = infinite)");
    if(d % 16 != 0) Fail(string(kCreditKeys[t][1]) + " must be a multiple of 16 "
                         "(one data credit is 16 B)");
    if(d != 0 && d < 16 * PcieDataUnits(mps)) {
      ostringstream os;
      os << kCreditKeys[t][1] << " = " << d << " cannot admit even one full TLP "
         << "(pcie_mps = " << mps << " needs " << 16 * PcieDataUnits(mps) << ")";
      Fail(os.str());
    }
    size[t]  = PcieStorageFlits(h == 0 ? -1 : h, d == 0 ? -1 : d / 16, mps, overhead,
                                flit_bytes);
    start[t] = begin[t];
    stop[t]  = end[t];
    total   += size[t];
    budget << "  " << PcieTypeName(t) << ": "
           << (h ? to_string(h) : string("infinite")) << " header credits, "
           << (d ? to_string(d) + " B (" + to_string(d / 16) + " data credits)"
                 : string("infinite data"))
           << " -> " << size[t] << " flits of input storage, VCs " << begin[t]
           << "-" << end[t] << endl;
  }

  config.Assign("private_bufs", PCIE_NUM_TYPES);
  config.Assign("private_buf_size", IntArray(size));
  config.Assign("private_buf_start_vc", IntArray(start));
  config.Assign("private_buf_end_vc", IntArray(stop));
  config.Assign("buf_size", total);

  const int xbar = config.GetInt("st_prepare_delay") + config.GetInt("st_final_delay");
  const int speedup = config.GetInt("output_speedup");
  cout << "PCIe credits (pcie_credits = 1), per link direction:" << endl
       << budget.str()
       << "  output queue: " << config.GetInt("output_buffer_size")
       << " flits per port, plus up to " << (xbar * speedup + speedup - 1)
       << " flits already in the crossbar pipeline" << endl;
}
