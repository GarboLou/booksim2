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

#ifndef _ROUTEFUNC_HPP_
#define _ROUTEFUNC_HPP_

#include "flit.hpp"
#include "router.hpp"
#include "outputset.hpp"
#include "config_utils.hpp"

typedef void (*tRoutingFunction)( const Router *, const Flit *, int in_channel, OutputSet *, bool );

void InitializeRoutingMap( const Configuration & config );

extern map<string, tRoutingFunction> gRoutingFunctionMap;

extern int gNumVCs;
extern int gReadReqBeginVC, gReadReqEndVC;
extern int gWriteReqBeginVC, gWriteReqEndVC;
extern int gReadReplyBeginVC, gReadReplyEndVC;
extern int gWriteReplyBeginVC, gWriteReplyEndVC;

// Next-hop output queueing (noq = 1): the part of [range_start, range_end]
// reserved for packets that will leave the next router through next_output
// of its num_outputs ports. per_output == 0 divides the range evenly (stock
// BookSim); per_output > 0 reserves exactly that many VCs per output and
// leaves the rest unused. Returns false if the range is too small.
bool NoqVcSlice( int range_start, int range_end, int next_output, int num_outputs,
                 int per_output, int & vc_start, int & vc_end );

#endif
