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

#ifndef _CREDIT_HPP_
#define _CREDIT_HPP_

#include <set>
#include <stack>
#include <vector>

class Flit;

class Credit {

public:

  set<int> vc;

  // PCIe protocol credits returned with this message (pcie_credits = 1),
  // one record per input VC that freed a flit. Physical flit slots still
  // return through `vc`.
  struct PcieReturn {
    int vc;
    int type;   // PCIE_P / PCIE_NP / PCIE_CPL
    int hdr;    // 1 when the TLP's tail left the queue
    int data;   // 16 B data credits
  };
  vector<PcieReturn> pcie;

  // Record what flit f returns on leaving input VC in_vc; nothing if f
  // carries no PCIe metadata.
  void AddPcieReturn( int in_vc, Flit const * f );

  // these are only used by the event router
  bool head, tail;
  int  id;

  void Reset();
  
  static Credit * New();
  void Free();
  static void FreeAll();
  static int OutStanding();
private:

  static stack<Credit *> _all;
  static stack<Credit *> _free;

  Credit();
  ~Credit() {}

};

#endif
