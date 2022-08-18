/*
 * This file is part of PowerDNS or dnsdist.
 * Copyright -- PowerDNS.COM B.V. and its contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * In addition, for the avoidance of any doubt, permission is granted to
 * link this program with OpenSSL and to (re)distribute the binaries
 * produced as the result of such linking.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#pragma once

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "dnsname.hh"
#include "qtype.hh"
#include "dnsrecords.hh"
#include "validate.hh"

class ZoneParserTNG;

namespace pdns
{
class ZoneMD
{
public:
  enum class Config : uint8_t
  {
    Ignore,
    Validate,
    Require
  };
  enum class Result : uint8_t
  {
    OK,
    NoValidationDone,
    ValidationFailure
  };

  ZoneMD(const DNSName& zone) :
    d_zone(zone)
  {}
  void readRecords(ZoneParserTNG& zpt);
  void readRecords(const std::vector<DNSRecord>& records);
  void readRecord(const DNSRecord& record);
  void verify(bool& validationDone, bool& validationOK);

  // Return the zone's apex DNSKEYs
  const std::set<shared_ptr<DNSKEYRecordContent>>& getDNSKEYs() const
  {
    return d_dnskeys;
  }

  // Return the zone's apex RRSIGs
  const std::vector<shared_ptr<RRSIGRecordContent>>& getRRSIGs() const
  {
    return d_rrsigs;
  }

  // Return the zone's apex ZONEMDs
  std::vector<shared_ptr<ZONEMDRecordContent>> getZONEMDs() const
  {
    std::vector<shared_ptr<ZONEMDRecordContent>> ret;
    for (const auto& zonemd : d_zonemdRecords) {
      ret.emplace_back(zonemd.second.record);
    }
    return ret;
  }

  // Return the zone's apex NSECs with signatures
  const ContentSigPair& getNSECs() const
  {
    return d_nsecs;
  }

  // Return the zone's apex NSEC3s with signatures
  const ContentSigPair& getNSEC3s() const
  {
    const auto it = d_nsec3s.find(d_nsec3label);
    return it == d_nsec3s.end() ? empty : d_nsec3s.at(d_nsec3label);
  }

  const DNSName& getNSEC3Label() const
  {
    return d_nsec3label;
  }

  const std::vector<shared_ptr<NSEC3PARAMRecordContent>>& getNSEC3Params() const
  {
    return d_nsec3params;
  }

private:
  typedef std::pair<DNSName, QType> RRSetKey_t;
  typedef std::vector<std::shared_ptr<DNSRecordContent>> RRVector_t;

  struct CanonRRSetKeyCompare
  {
    bool operator()(const RRSetKey_t& a, const RRSetKey_t& b) const
    {
      // FIXME surely we can be smarter here
      if (a.first.canonCompare(b.first)) {
        return true;
      }
      if (b.first.canonCompare(a.first)) {
        return false;
      }
      return a.second < b.second;
    }
  };

  typedef std::map<RRSetKey_t, RRVector_t, CanonRRSetKeyCompare> RRSetMap_t;

  struct ZoneMDAndDuplicateFlag
  {
    std::shared_ptr<ZONEMDRecordContent> record;
    bool duplicate;
  };

  // scheme,hashalgo -> zonemdrecord,duplicate
  std::map<pair<uint8_t, uint8_t>, ZoneMDAndDuplicateFlag> d_zonemdRecords;

  RRSetMap_t d_resourceRecordSets;
  std::map<RRSetKey_t, uint32_t> d_resourceRecordSetTTLs;

  std::shared_ptr<SOARecordContent> d_soaRecordContent;
  std::set<shared_ptr<DNSKEYRecordContent>> d_dnskeys;
  std::vector<shared_ptr<RRSIGRecordContent>> d_rrsigs;
  std::vector<shared_ptr<NSEC3PARAMRecordContent>> d_nsec3params;
  ContentSigPair d_nsecs;
  map<DNSName, ContentSigPair> d_nsec3s;
  DNSName d_nsec3label;
  const DNSName d_zone;
  const ContentSigPair empty;
};

}
