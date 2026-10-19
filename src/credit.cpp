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

/*credit.cpp
 *
 *A class for credits
 */

#include "booksim.hpp"
#include "credit.hpp"
#include "flit.hpp"
#include "pcie_credits.hpp"

stack<Credit *> Credit::_all;
stack<Credit *> Credit::_free;

Credit::Credit()
{
  Reset();
}

void Credit::Reset()
{
  vc.clear();
  pcie.clear();
  head = false;
  tail = false;
  id   = -1;
}

void Credit::AddPcieReturn( int in_vc, Flit const * f )
{
  if(f->pcie_payload < 0) return;
  PcieReturn r;
  r.vc   = in_vc;
  r.type = PcieTypeOfFlit(f->type);
  r.hdr  = f->tail ? 1 : 0;
  r.data = f->pcie_data_ret;
  if(r.hdr || r.data) pcie.push_back(r);
}

Credit * Credit::New() {
  Credit * c;
  if(_free.empty()) {
    c = new Credit();
    _all.push(c);
  } else {
    c = _free.top();
    c->Reset();
    _free.pop();
  }
  return c;
}

void Credit::Free() {
  _free.push(this);
}

// See Flit::FreeAll: the free list points into the deleted objects.
void Credit::FreeAll() {
  while(!_all.empty()) {
    delete _all.top();
    _all.pop();
  }
  while(!_free.empty()) _free.pop();
}


int Credit::OutStanding(){
  return _all.size()-_free.size();
}
