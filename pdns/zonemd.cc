#include "zonemd.hh"

#include "dnsrecords.hh"
#include "dnssecinfra.hh"
#include "sha.hh"
#include "zoneparser-tng.hh"
#include "base32.hh"

void pdns::ZoneMD::readRecords(ZoneParserTNG& zpt)
{
  DNSResourceRecord dnsResourceRecord;

  while (zpt.get(dnsResourceRecord)) {
    std::shared_ptr<DNSRecordContent> drc;
    try {
      drc = DNSRecordContent::mastermake(dnsResourceRecord.qtype, QClass::IN, dnsResourceRecord.content);
    }
    catch (const PDNSException& pe) {
      std::string err = "Bad record content in record for '" + dnsResourceRecord.qname.toStringNoDot() + "'|" + dnsResourceRecord.qtype.toString() + ": " + pe.reason;
      throw PDNSException(err);
    }
    catch (const std::exception& e) {
      std::string err = "Bad record content in record for '" + dnsResourceRecord.qname.toStringNoDot() + "|" + dnsResourceRecord.qtype.toString() + "': " + e.what();
      throw PDNSException(err);
    }
    DNSRecord rec;
    rec.d_name = dnsResourceRecord.qname;
    rec.d_content = drc;
    rec.d_type = dnsResourceRecord.qtype;
    rec.d_class = dnsResourceRecord.qclass;
    rec.d_ttl = dnsResourceRecord.ttl;
    rec.d_clen = dnsResourceRecord.content.length(); // XXX is this correct?
    readRecord(rec);
  }
}

void pdns::ZoneMD::readRecords(const vector<DNSRecord>& records)
{
  for (auto& record : records) {
    readRecord(record);
  }
}

void pdns::ZoneMD::readRecord(const DNSRecord& record)
{
  if (!record.d_name.isPartOf(d_zone) && record.d_name != d_zone) {
    return;
  }
  if (record.d_class == QClass::IN && record.d_type == QType::SOA && d_soaRecordContent) {
    return;
  }

  if (record.d_class == QClass::IN && record.d_name == d_zone) {
    switch (record.d_type) {
    case QType::SOA: {
      d_soaRecordContent = std::dynamic_pointer_cast<SOARecordContent>(record.d_content);
      if (d_soaRecordContent == nullptr) {
        throw PDNSException("Invalid SOA record");
      }
      break;
    }
    case QType::DNSKEY: {
      auto dnskey = std::dynamic_pointer_cast<DNSKEYRecordContent>(record.d_content);
      if (dnskey == nullptr) {
        throw PDNSException("Invalid DNSKEY record");
      }
      d_dnskeys.emplace(dnskey);
      break;
    }
    case QType::ZONEMD: {
      auto zonemd = std::dynamic_pointer_cast<ZONEMDRecordContent>(record.d_content);
      if (zonemd == nullptr) {
        throw PDNSException("Invalid ZONEMD record");
      }
      auto inserted = d_zonemdRecords.insert({pair(zonemd->d_scheme, zonemd->d_hashalgo), {zonemd, false}});
      if (!inserted.second) {
        // Mark as duplicate
        inserted.first->second.duplicate = true;
      }
      break;
    }
    case QType::RRSIG: {
      auto rrsig = std::dynamic_pointer_cast<RRSIGRecordContent>(record.d_content);
      if (rrsig == nullptr) {
        throw PDNSException("Invalid RRSIG record");
      }
      d_rrsigs.emplace_back(rrsig);
      if (rrsig->d_type == QType::NSEC) {
        d_nsecs.signatures.emplace_back(rrsig);
      }
      // RRSIG on NEC3 handled below
      break;
    }
    case QType::NSEC: {
      auto nsec = std::dynamic_pointer_cast<NSECRecordContent>(record.d_content);
      if (nsec == nullptr) {
        throw PDNSException("Invalid NSEC record");
      }
      d_nsecs.records.emplace(nsec);
      break;
    }
    case QType::NSEC3:
      // Handled below
      break;
    case QType::NSEC3PARAM: {
      auto param = std::dynamic_pointer_cast<NSEC3PARAMRecordContent>(record.d_content);
      if (param == nullptr) {
        throw PDNSException("Invalid NSEC3PARAM record");
      }
      if (g_maxNSEC3Iterations && param->d_iterations > g_maxNSEC3Iterations) {
        return;
      }
      d_nsec3params.emplace_back(param);
      d_nsec3label = d_zone;
      d_nsec3label.prependRawLabel(toBase32Hex(hashQNameWithSalt(param->d_salt, param->d_iterations, d_zone)));
      // Zap the NSEC3 at labels that we now know are not relevant
      for (auto it = d_nsec3s.begin(); it != d_nsec3s.end();) {
        if (it->first != d_nsec3label) {
          it = d_nsec3s.erase(it);
        }
        else {
          ++it;
        }
      }
      break;
    }
    }
  }
  // Until we have seen the NSEC3PARAM record, we save all of them, as we do not know the label for the zone yet
  if (record.d_class == QClass::IN && (d_nsec3label.empty() || record.d_name == d_nsec3label)) {
    switch (record.d_type) {
    case QType::NSEC3: {
      auto nsec3 = std::dynamic_pointer_cast<NSEC3RecordContent>(record.d_content);
      if (nsec3 == nullptr) {
        throw PDNSException("Invalid NSEC3 record");
      }
      d_nsec3s[record.d_name].records.emplace(nsec3);
      break;
    }
    case QType::RRSIG: {
      auto rrsig = std::dynamic_pointer_cast<RRSIGRecordContent>(record.d_content);
      if (rrsig == nullptr) {
        throw PDNSException("Invalid RRSIG record");
      }
      if (rrsig->d_type == QType::NSEC3) {
        d_nsec3s[record.d_name].signatures.emplace_back(rrsig);
      }
      break;
    }
    }
  }
  RRSetKey_t key = std::pair(record.d_name, record.d_type);
  d_resourceRecordSets[key].push_back(record.d_content);
  d_resourceRecordSetTTLs[key] = record.d_ttl;
}

void pdns::ZoneMD::verify(bool& validationDone, bool& validationOK)
{
  validationDone = false;
  validationOK = false;

  if (!d_soaRecordContent) {
    return;
  }
  // Get all records and remember RRSets and TTLs

  // Determine which digests to compute based on accepted zonemd records present
  unique_ptr<pdns::SHADigest> sha384digest{nullptr}, sha512digest{nullptr};

  for (const auto& it : d_zonemdRecords) {
    // The SOA Serial field MUST exactly match the ZONEMD Serial
    // field. If the fields do not match, digest verification MUST
    // NOT be considered successful with this ZONEMD RR.

    // The Scheme field MUST be checked. If the verifier does not
    // support the given scheme, verification MUST NOT be considered
    // successful with this ZONEMD RR.

    // The Hash Algorithm field MUST be checked. If the verifier does
    // not support the given hash algorithm, verification MUST NOT be
    // considered successful with this ZONEMD RR.
    const auto duplicate = it.second.duplicate;
    const auto& r = it.second.record;
    if (!duplicate && r->d_serial == d_soaRecordContent->d_st.serial && r->d_scheme == 1 && (r->d_hashalgo == 1 || r->d_hashalgo == 2)) {
      // A supported ZONEMD record
      if (r->d_hashalgo == 1) {
        sha384digest = make_unique<pdns::SHADigest>(384);
      }
      else if (r->d_hashalgo == 2) {
        sha512digest = make_unique<pdns::SHADigest>(512);
      }
    }
  }

  if (!sha384digest && !sha512digest) {
    // No supported ZONEMD algo found, mismatch in SOA, mismatch in scheme or duplicate
    return;
  }

  // A little helper
  auto hash = [&sha384digest, &sha512digest](const std::string& msg) {
    if (sha384digest) {
      sha384digest->process(msg);
    }
    if (sha512digest) {
      sha512digest->process(msg);
    }
  };

  // Compute requested digests
  for (auto& rrset : d_resourceRecordSets) {
    const auto& qname = rrset.first.first;
    const auto& qtype = rrset.first.second;
    if (qtype == QType::ZONEMD && qname == d_zone) {
      continue; // the apex ZONEMD is not digested
    }

    sortedRecords_t sorted;
    for (auto& rr : rrset.second) {
      if (qtype == QType::RRSIG) {
        const auto rrsig = std::dynamic_pointer_cast<RRSIGRecordContent>(rr);
        if (rrsig->d_type == QType::ZONEMD && qname == d_zone) {
          continue;
        }
      }
      sorted.insert(rr);
    }

    if (sorted.empty()) {
      continue;
    }

    if (qtype != QType::RRSIG) {
      RRSIGRecordContent rrc;
      rrc.d_originalttl = d_resourceRecordSetTTLs[rrset.first];
      rrc.d_type = qtype;
      auto msg = getMessageForRRSET(qname, rrc, sorted, false, false);
      hash(msg);
    }
    else {
      // RRSIG is special, since  original TTL depends on qtype covered by RRSIG
      // which can be different per record
      for (const auto& rrsig : sorted) {
        auto rrsigc = std::dynamic_pointer_cast<RRSIGRecordContent>(rrsig);
        RRSIGRecordContent rrc;
        rrc.d_originalttl = d_resourceRecordSetTTLs[pair(rrset.first.first, rrsigc->d_type)];
        rrc.d_type = qtype;
        auto msg = getMessageForRRSET(qname, rrc, {rrsigc}, false, false);
        hash(msg);
      }
    }
  }

  // Final verify
  for (const auto& [k, v] : d_zonemdRecords) {
    auto [zonemd, duplicate] = v;
    if (zonemd->d_hashalgo == 1) {
      validationDone = true;
      auto computed = sha384digest->digest();
      if (constantTimeStringEquals(zonemd->d_digest, computed)) {
        validationOK = true;
        break; // Per RFC: a single succeeding validation is enough
      }
    }
    else if (zonemd->d_hashalgo == 2) {
      validationDone = true;
      auto computed = sha512digest->digest();
      if (constantTimeStringEquals(zonemd->d_digest, computed)) {
        validationOK = true;
        break; // Per RFC: a single succeeding validation is enough
      }
    }
  }
}
