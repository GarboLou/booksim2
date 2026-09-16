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

/*booksim_globals.cpp
 *
 * Definitions of the process-wide globals declared in globals.hpp.
 *
 * These lived in main.cpp historically. They were moved here so that the
 * simulator can be archived into libbooksim.a without dragging in main(),
 * which lets it be embedded in another program (see engine/booksim_engine.hpp).
 * main.cpp is still the only caller for the standalone booksim binary.
 */

#include <iostream>
#include <string>

#include "booksim.hpp"
#include "globals.hpp"
#include "trafficmanager.hpp"
#include "booksim_init.hpp"

/* the current traffic manager instance */
TrafficManager * trafficManager = NULL;

int GetSimTime() {
  return trafficManager->getTime();
}

class Stats;
Stats * GetStats(const std::string & name) {
  Stats* test =  trafficManager->getStats(name);
  if(test == 0){
    cout<<"warning statistics "<<name<<" not found"<<endl;
  }
  return test;
}

/* printing activity factor*/
bool gPrintActivity;

int gK;//radix
int gN;//dimension
int gC;//concentration

int gNodes;

//generate nocviewer trace
bool gTrace;

ostream * gWatchOut;
