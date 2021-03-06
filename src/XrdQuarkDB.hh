// ----------------------------------------------------------------------
// File: XrdQuarkDB.hh
// Author: Georgios Bitzes - CERN
// ----------------------------------------------------------------------

/************************************************************************
 * quarkdb - a redis-like highly available key-value store              *
 * Copyright (C) 2016 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 * (at your option) any later version.                                  *
 *                                                                      *
 * This program is distributed in the hope that it will be useful,      *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

#ifndef __QUARKDB_XRDQUARKDB_PROTOCOL_H__
#define __QUARKDB_XRDQUARKDB_PROTOCOL_H__

#include "Xrd/XrdProtocol.hh"
#include "Utils.hh"
#include "utils/InFlightTracker.hh"
#include "EventFD.hh"
#include "qclient/QClient.hh"
#include <atomic>

class XrdLink;

namespace quarkdb {

//------------------------------------------------------------------------------
// Forward declarations
//------------------------------------------------------------------------------
class Link; class Connection; class QuarkDBNode;


class XrdQuarkDB : public XrdProtocol {
public:
  XrdQuarkDB(bool tls);

  /// Read and apply the configuration
  static int Configure(char *parms, XrdProtocol_Config *pi);

  /// Implementation of XrdProtocol interface
  XrdProtocol *Match(XrdLink *lp);
  int Process(XrdLink *lp);
  void Recycle(XrdLink *lp=0,int consec=0,const char *reason=0);
  int Stats(char *buff, int blen, int do_sync=0);

  /// Implementation of XrdJob interface
  void DoIt();

  /// Construction / destruction
  XrdQuarkDB();
  virtual ~XrdQuarkDB();

  static InFlightTracker inFlightTracker;
  static EventFD shutdownFD;
private:
  /// The link we are bound to
  Link *link = nullptr;
  Connection *conn = nullptr;

  qclient::TlsConfig tlsconfig;
  void Reset();
protected:
  static QuarkDBNode *quarkdbNode;
  static void shutdownMonitor();
};

}


#endif
