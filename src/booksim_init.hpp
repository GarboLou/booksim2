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

#ifndef _BOOKSIM_INIT_HPP_
#define _BOOKSIM_INIT_HPP_

class Configuration;
class TrafficManager;

/* The single live TrafficManager. Defined in booksim_globals.cpp; GetSimTime()
 * and GetStats() read through it, so every router and channel in the simulator
 * depends on it being set. BookSim is a one-network-per-process design.
 */
extern TrafficManager * trafficManager;

/* Process-wide setup that must run once, before any Network or TrafficManager
 * is constructed: populates gRoutingFunctionMap (which the TrafficManager and
 * IQRouter constructors look up) and sets gPrintActivity / gTrace / gWatchOut.
 *
 * Called by main() for the standalone binary, and by Engine::Create() when
 * BookSim is embedded as a library.
 */
void BooksimGlobalInit( const Configuration & config );

/* Undo the parts of BooksimGlobalInit that own resources. Safe to call with no
 * matching init. Leaves gRoutingFunctionMap populated -- it is idempotent and
 * holds no ownership.
 */
void BooksimGlobalCleanup( );

#endif
