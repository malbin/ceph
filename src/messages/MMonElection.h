// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */


#ifndef CEPH_MMONELECTION_H
#define CEPH_MMONELECTION_H

#include "msg/Message.h"
#include "mon/MonMap.h"

class MMonElection : public Message {

  static const int HEAD_VERSION = 2;

public:
  static const int OP_PROPOSE = 1;
  static const int OP_ACK     = 2;
  static const int OP_NAK     = 3;
  static const int OP_VICTORY = 4;
  static const char *get_opname(int o) {
    switch (o) {
    case OP_PROPOSE: return "propose";
    case OP_ACK: return "ack";
    case OP_NAK: return "nak";
    case OP_VICTORY: return "victory";
    default: assert(0); return 0;
    }
  }
  
  uuid_d fsid;
  int32_t op;
  epoch_t epoch;
  bufferlist monmap_bl;
  set<int> quorum;
  
  MMonElection() : Message(MSG_MON_ELECTION, HEAD_VERSION) { }
  MMonElection(int o, epoch_t e, MonMap *m) : 
    Message(MSG_MON_ELECTION, HEAD_VERSION), 
    fsid(m->fsid), op(o), epoch(e) {

    // encode using full feature set; we will reencode for dest later,
    // if necessary
    m->encode(monmap_bl, CEPH_FEATURES_ALL);
  }
private:
  ~MMonElection() {}

public:  
  const char *get_type_name() const { return "election"; }
  void print(ostream& out) const {
    out << "election(" << fsid << " " << get_opname(op) << " " << epoch << ")";
  }
  
  void encode_payload(uint64_t features) {
    if (monmap_bl.length() && (features & CEPH_FEATURE_MONENC) == 0) {
      // reencode old-format monmap
      MonMap t;
      t.decode(monmap_bl);
      monmap_bl.clear();
      t.encode(monmap_bl, features);
    }

    ::encode(fsid, payload);
    ::encode(op, payload);
    ::encode(epoch, payload);
    ::encode(monmap_bl, payload);
    ::encode(quorum, payload);
  }
  void decode_payload() {
    bufferlist::iterator p = payload.begin();
    if (header.version >= 2)
      ::decode(fsid, p);
    else
      memset(&fsid, 0, sizeof(fsid));
    ::decode(op, p);
    ::decode(epoch, p);
    ::decode(monmap_bl, p);
    ::decode(quorum, p);
  }
  
};

#endif
