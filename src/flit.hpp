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

#ifndef _FLIT_HPP_
#define _FLIT_HPP_

#include <iostream>
#include <stack>

#include "booksim.hpp"
#include "outputset.hpp"

class Flit {

public:

  const static int NUM_FLIT_TYPES = 5;
  enum FlitType { READ_REQUEST  = 0, 
		  READ_REPLY    = 1,
		  WRITE_REQUEST = 2,
		  WRITE_REPLY   = 3,
                  ANY_TYPE      = 4 };
  FlitType type;

  int vc;

  int cl;

  bool head;
  bool tail;
  
  int  ctime;
  int  itime;
  int  atime;

  int  id;
  int  pid;

  bool record;

  int  src;
  int  dest;

  int  pri;

  int  hops;
  bool watch;
  int  subnetwork;
  
  // intermediate destination (if any)
  mutable int intm;

  // phase in multi-phase algorithms
  mutable int ph;

  // Fields for arbitrary data
  void* data ;

  // Index of the end-to-end path the sender pinned this packet to, taken
  // modulo the number of paths for its (source, destination). -1 == not
  // pinned; only the pinned routing function reads it.
  int path_key;

  // Loop-free admission unit this packet belongs to (FlowTrafficManager),
  // -1 if none. Its pass-throughs stay registered until its last tail retires.
  int lf_unit;

  // PCIe credit model (pcie_credits = 1). The packet's payload in bytes, on
  // every flit (-1: no PCIe metadata), and the data credits this flit returns
  // when it leaves an input queue (pcie_credits.hpp: PcieDataReturns). A head
  // reserves one header credit and PcieDataUnits(pcie_payload) data credits.
  int pcie_payload;
  int pcie_data_ret;

  // Lookahead route info
  OutputSet la_route_set;

  void Reset();

  static Flit * New();
  void Free();
  static void FreeAll();

private:

  Flit();
  ~Flit() {}

  static stack<Flit *> _all;
  static stack<Flit *> _free;

};

ostream& operator<<( ostream& os, const Flit& f );

#endif
