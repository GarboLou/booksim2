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

#ifndef _PCINET_HPP_
#define _PCINET_HPP_

#include "network.hpp"
#include "routefunc.hpp"
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

  void _ComputeSize( const Configuration &config );
  void _BuildNet( const Configuration &config );
  void readFile();
  void buildRoutingTable();
  void route(int r_start);

public:
  PCINet( const Configuration &config, const string & name );
  ~PCINet();

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

#endif
