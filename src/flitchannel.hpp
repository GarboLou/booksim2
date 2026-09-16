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

// ----------------------------------------------------------------------
//
//  File Name: flitchannel.hpp
//
//  The FlitChannel models a flit channel with a multi-cycle 
//   transmission delay. The channel latency can be specified as 
//   an integer number of simulator cycles.
// ----------------------------------------------------------------------

#ifndef FLITCHANNEL_HPP
#define FLITCHANNEL_HPP

// ----------------------------------------------------------------------
//  $Author: jbalfour $
//  $Date: 2007/06/27 23:10:17 $
//  $Id$
// ----------------------------------------------------------------------

#include "channel.hpp"
#include "flit.hpp"
#include "globals.hpp"
#include <cassert>

using namespace std;

class Router ;

class FlitChannel : public Channel<Flit> {
public:
  FlitChannel(Module * parent, string const & name, int classes);

  void SetSource(Router const * const router, int port) ;
  inline Router const * const GetSource() const {
    return _routerSource;
  }
  inline int const & GetSourcePort() const {
    return _routerSourcePort;
  }
  void SetSink(Router const * const router, int port) ;
  inline Router const * const GetSink() const {
    return _routerSink;
  }
  inline int const & GetSinkPort() const {
    return _routerSinkPort;
  }
  inline vector<int> const & GetActivity() const {
    return _active;
  }

  // Link bandwidth, expressed as the number of cycles one flit occupies the
  // wire. 1 (the default) reproduces stock BookSim exactly: a channel that
  // accepts a flit every cycle. Larger values model a narrower or slower link
  // (e.g. PCIe Gen5 x8 is half of Gen5 x16, so cycles_per_flit == 2 when a
  // Gen5 x16 link is defined as 1 flit/cycle).
  inline void SetCyclesPerFlit(int cpf) {
    assert(cpf >= 1);
    _cycles_per_flit = cpf;
  }
  inline int GetCyclesPerFlit() const {
    return _cycles_per_flit;
  }

  // False while the wire is still serializing the previous flit. Callers must
  // check this BEFORE dequeuing, because Channel<Flit>::Send() overwrites its
  // single input slot unconditionally and would drop the flit on the floor.
  inline bool IsReady() const {
    return GetSimTime() >= _next_free_time;
  }

  // Send flit 
  virtual void Send(Flit * flit);

  virtual void ReadInputs();
  virtual void WriteOutputs();

private:
  
  ////////////////////////////////////////
  //
  // Power Models OBSOLETE
  //
  ////////////////////////////////////////

  Router const * _routerSource;
  int _routerSourcePort;
  Router const * _routerSink;
  int _routerSinkPort;

  // Link bandwidth: cycles occupied per flit, and the cycle at which the wire
  // next becomes free.
  int _cycles_per_flit;
  int _next_free_time;

  // Statistics for Activity Factors
  vector<int> _active;
  int _idle;
};

#endif
