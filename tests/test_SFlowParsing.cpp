// [Co-developed with claude code -- Adam]
//
// Robustness tests for the sFlow datagram parser.
//
// The kernel has two external input surfaces: the /ndt/ HTTP API, and the sFlow UDP
// collector on port 6343. Everything else in the test suite covers the first. This covers
// the second, which is reachable by anything that can send a UDP packet.
//
// The parser validated only two things -- at least 28 bytes, and a length that is a
// multiple of 4 -- then indexed roughly 40 fixed word offsets (up to index+38) using a
// sample count and lengths taken straight from the datagram, with no further checks. So a
// truncated or crafted packet read past the end of the buffer: wrong numbers, or an
// occasional crash, from a source that needs no authentication. Two of the advancement
// expressions are also unsigned subtractions that wrap when the packet says so.
//
// Every test here asserts the same two things: the parser does not crash, and it returns.
// Under ASan the first would be an out-of-bounds report; without it, these still pin the
// termination behaviour and document the shapes that used to be mishandled.

#include <gtest/gtest.h>

#include "common_types/SFlowType.hpp"
#include "event_system/EventBus.hpp"
#include "ndt_core/collection/Classifier.hpp"
#include "ndt_core/collection/FlowLinkUsageCollector.hpp"
#include "ndt_core/collection/TopologyAndFlowMonitor.hpp"
#include "utils/Logger.hpp"
#include "utils/Utils.hpp"

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

namespace
{

/// Builds sFlow v5 datagrams word by word, so a test can express a malformed shape exactly.
class DatagramBuilder
{
  public:
    /// Standard v5 header: version, address type, agent IP, sub-agent, seq, uptime, count.
    DatagramBuilder& header(uint32_t sampleCount, uint32_t version = 5)
    {
        word(version);
        word(1);                       // address type: IPv4
        raw(inet_addr("192.168.123.11")); // agent IP is stored without ntohl by the parser
        word(0);                       // sub-agent id
        word(1);                       // datagram sequence number
        word(1000);                    // uptime
        word(sampleCount);
        return *this;
    }

    DatagramBuilder& word(uint32_t hostOrderValue)
    {
        m_words.push_back(htonl(hostOrderValue));
        return *this;
    }

    /// Appends a word that is already in network order (or is opaque bytes).
    DatagramBuilder& raw(uint32_t networkOrderValue)
    {
        m_words.push_back(networkOrderValue);
        return *this;
    }

    DatagramBuilder& words(size_t count, uint32_t hostOrderValue = 0)
    {
        for (size_t i = 0; i < count; ++i)
        {
            word(hostOrderValue);
        }
        return *this;
    }

    /// Appends opaque bytes (a sampled frame header) in wire order, zero-padded to a whole
    /// word as sFlow requires. [Co-developed with claude code -- Adam]
    DatagramBuilder& octets(const std::vector<uint8_t>& bytes)
    {
        for (size_t i = 0; i < bytes.size(); i += 4)
        {
            uint8_t chunk[4] = {0, 0, 0, 0};
            for (size_t j = 0; j < 4 && i + j < bytes.size(); ++j)
            {
                chunk[j] = bytes[i + j];
            }
            uint32_t w = 0;
            std::memcpy(&w, chunk, sizeof(w));
            m_words.push_back(w);
        }
        return *this;
    }

    /// Mutable byte buffer, since handlePacket takes char*.
    std::vector<char> bytes() const
    {
        std::vector<char> out(m_words.size() * sizeof(uint32_t));
        // Guarded: memcpy with null src/dst is undefined even for a length of zero, and the
        // zero-word case is exercised by the too-short-datagram test. UBSan flags it.
        if (!m_words.empty())
        {
            std::memcpy(out.data(), m_words.data(), out.size());
        }
        return out;
    }

    size_t wordCount() const { return m_words.size(); }

  private:
    std::vector<uint32_t> m_words;
};

/// Finds tests/fixtures whichever directory the test binary was started from.
/// Same search as test_GoldenFixture.cpp and test_SFlowEmitterRoundtrip.cpp.
std::filesystem::path fixtureDir()
{
    for (const auto* candidate :
         {"tests/fixtures", "../tests/fixtures", "../../tests/fixtures"})
    {
        if (std::filesystem::is_directory(candidate))
        {
            return candidate;
        }
    }
    return {};
}

std::vector<char> loadFixture(const std::string& name)
{
    const auto dir = fixtureDir();
    if (dir.empty())
    {
        return {};
    }
    std::ifstream f(dir / name, std::ios::binary);
    if (!f)
    {
        return {};
    }
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

/// Overwrites word 0, the sFlow version field, leaving every other byte untouched.
void setVersionWord(std::vector<char>& datagram, uint32_t version)
{
    const uint32_t netOrder = htonl(version);
    std::memcpy(datagram.data(), &netOrder, sizeof(netOrder));
}

/// Exposes handlePacket. The collector's collaborators are only stored, not used, by the
/// parsing path under test, so the fixture keeps them minimal.
class TestableCollector : public sflow::FlowLinkUsageCollector
{
  public:
    TestableCollector(std::shared_ptr<TopologyAndFlowMonitor> monitor,
                      std::shared_ptr<EventBus> bus,
                      std::shared_ptr<ndtClassifier::Classifier> classifier,
                      int mode)
        : sflow::FlowLinkUsageCollector(std::move(monitor),
                                        nullptr,
                                        std::move(bus),
                                        mode,
                                        std::move(classifier))
    {
    }

    using sflow::FlowLinkUsageCollector::handlePacket;
    using sflow::FlowLinkUsageCollector::malformedDatagramCount;
};

class SFlowParsingFixture : public ::testing::Test
{
  protected:
    /// The parser logs, and Logger::instance() is null until init runs, so logging without
    /// it segfaults. Must be done per suite rather than relying on another one -- see the
    /// note in test_SwitchKindDispatch.cpp. Logger::init is idempotent.
    static void SetUpTestSuite()
    {
        LogConfig cfg;
        cfg.level = spdlog::level::off; // the expected warnings would otherwise be noise
        Logger::init(cfg);
    }

    void SetUp() override { resetCollector(); }

    /// Discards the collector and its accumulated flow table. Tests that feed several
    /// datagrams and assert on the table after each one need a clean slate between feeds,
    /// otherwise the first feed's flows satisfy the next feed's assertion.
    /// The mode defaults to MININET; the HPE expanded-sample tests use TESTBED, the mode the
    /// kernel runs in against hardware switches. [Co-developed with claude code -- Adam]
    void resetCollector(utils::DeploymentMode mode = utils::DeploymentMode::MININET)
    {
        auto bus = std::make_shared<EventBus>();
        auto monitor = std::make_shared<TopologyAndFlowMonitor>(
            std::make_shared<Graph>(),
            std::make_shared<std::shared_mutex>(),
            bus,
            mode);
        m_collector = std::make_unique<TestableCollector>(
            monitor, bus, std::make_shared<ndtClassifier::Classifier>(), mode);
    }

    /// Feeds a datagram to the parser. Returns normally iff the parser did.
    void feed(const DatagramBuilder& builder)
    {
        auto buf = builder.bytes();
        m_collector->handlePacket(buf.data(), buf.size());
    }

    void feed(std::vector<char>& buf)
    {
        m_collector->handlePacket(buf.data(), buf.size());
    }

    std::unique_ptr<TestableCollector> m_collector;
};

} // namespace

// =====================================================================================
// The pre-existing length checks
// =====================================================================================

TEST_F(SFlowParsingFixture, RejectsNullBuffer)
{
    EXPECT_NO_THROW(m_collector->handlePacket(nullptr, 64));
}

TEST_F(SFlowParsingFixture, RejectsDatagramShorterThanTheHeader)
{
    // Fewer than 7 words: the header reads alone would run off the end.
    for (size_t w = 0; w < 7; ++w)
    {
        DatagramBuilder b;
        b.words(w, 5);
        EXPECT_NO_THROW(feed(b)) << "failed at " << w << " word(s)";
    }
}

TEST_F(SFlowParsingFixture, RejectsUnalignedLength)
{
    DatagramBuilder b;
    b.header(0);
    auto buf = b.bytes();
    buf.pop_back(); // 4n-1 bytes: not 32-bit aligned
    EXPECT_NO_THROW(feed(buf));
}

// [Co-developed with claude code -- Adam]
//
// The two tests below are a matched pair over one buffer: a real OVS capture, with word 0 --
// the sFlow version field, fixed there by the v5 datagram format -- either left at 5 or
// overwritten. Everything else is byte-identical, so the only thing either test can be
// measuring is the version guard.
//
// They replace a single `EXPECT_NO_THROW(feed(b))` over hand-built bodies. A silent drop
// returns normally, so "does not throw" is also what deleting the guard does: the datagrams
// flowed into sample parsing and the test stayed green, as did every other suite, because
// every other suite feeds v5. The observable consequence of a drop is that nothing is
// extracted, so that is what these assert.
//
// The refusal test alone is not enough either -- a guard that rejected *every* version would
// satisfy it. Hence the accept-path test on the same bytes.
//
// The buffer is a captured datagram rather than a DatagramBuilder body because the assertion
// is "these bytes would otherwise yield flows", and only something a real sFlow agent emitted
// can establish that independently of the parser being tested. The fixture below was captured
// from a working OVS + Ryu + Mininet run (see test_GoldenFixture.cpp) in a test-only commit
// that predates this parser's rewrite.
//
// Not every capture qualifies. Decoding the committed set shows tcp_00, tcp_02, mixed_01 and
// mixed_02 carry LLDP frames (ethertype 0x88cc) and tcp_01 carries IPv6 (0x86dd) -- none of
// which is a flow the parser is meant to extract, so using one of those as the control would
// have made "no flows" the right answer on both sides of the guard and the pair would have
// proved nothing. tcp_03.bin is six samples of the captured iperf flow, IPv4/TCP 36154->5001.
constexpr const char* kCapturedV5Fixture = "tcp_03.bin";

TEST_F(SFlowParsingFixture, ExtractsFlowsWhenTheVersionIsFive)
{
    auto v5 = loadFixture(kCapturedV5Fixture);
    ASSERT_FALSE(v5.empty()) << "tests/fixtures/" << kCapturedV5Fixture
                             << " missing; run the test binary from the repository root";

    EXPECT_NO_THROW(feed(v5));

    EXPECT_GT(m_collector->getFlowInfoTable().size(), 0u)
        << "the control for IgnoresUnsupportedVersion: a genuine v5 datagram has to produce "
           "flows, or 'an unsupported version produces none' proves nothing";
}

TEST_F(SFlowParsingFixture, IgnoresUnsupportedVersion)
{
    const auto v5 = loadFixture(kCapturedV5Fixture);
    ASSERT_FALSE(v5.empty()) << "tests/fixtures/" << kCapturedV5Fixture
                             << " missing; run the test binary from the repository root";

    for (uint32_t version : {0u, 4u, 6u, 0xFFFFFFFFu})
    {
        // A fresh collector per version: the flow table accumulates, so one version's leak
        // would otherwise be indistinguishable from the previous version's.
        resetCollector();

        auto datagram = v5;
        setVersionWord(datagram, version);

        EXPECT_NO_THROW(feed(datagram)) << "failed for version " << version;

        EXPECT_TRUE(m_collector->getFlowInfoTable().empty())
            << "version " << version << " is not sFlow v5, so the datagram must be dropped "
            << "whole; " << m_collector->getFlowInfoTable().size()
            << " flow(s) were extracted from it instead";
    }
}

// =====================================================================================
// Truncation -- the shape that used to read past the end of the buffer
// =====================================================================================

TEST_F(SFlowParsingFixture, HandlesHeaderThatPromisesASampleWithNoBodyAtAll)
{
    // Claims one sample, then ends. Previously read data[index] and data[index+1] beyond
    // the buffer.
    DatagramBuilder b;
    b.header(1);
    EXPECT_NO_THROW(feed(b));
}

TEST_F(SFlowParsingFixture, HandlesEveryTruncationOfACounterSample)
{
    // A counter sample reads up to index+38. Cutting a full-length datagram at every word
    // between the header and that maximum exercises each offset's bounds check.
    DatagramBuilder full;
    full.header(1).word(2).word(4 * 40).words(45);
    auto complete = full.bytes();

    for (size_t keepWords = 7; keepWords * 4 <= complete.size(); ++keepWords)
    {
        std::vector<char> truncated(complete.begin(),
                                    complete.begin() + static_cast<long>(keepWords * 4));
        EXPECT_NO_THROW(feed(truncated)) << "failed at " << keepWords << " word(s)";
    }
}

TEST_F(SFlowParsingFixture, HandlesEveryTruncationOfAFlowSample)
{
    // Flow samples reach index+32 on the TCP path.
    DatagramBuilder full;
    full.header(1).word(1).word(4 * 40).words(45);
    auto complete = full.bytes();

    for (size_t keepWords = 7; keepWords * 4 <= complete.size(); ++keepWords)
    {
        std::vector<char> truncated(complete.begin(),
                                    complete.begin() + static_cast<long>(keepWords * 4));
        EXPECT_NO_THROW(feed(truncated)) << "failed at " << keepWords << " word(s)";
    }
}

// =====================================================================================
// Values the datagram controls
// =====================================================================================

TEST_F(SFlowParsingFixture, RejectsAnImpossibleSampleCount)
{
    // sampleCount was trusted verbatim: 2^32-1 meant that many loop iterations.
    DatagramBuilder b;
    b.header(0xFFFFFFFF).words(10);
    EXPECT_NO_THROW(feed(b));
}

TEST_F(SFlowParsingFixture, TerminatesWhenSampleLengthIsZero)
{
    // A zero length leaves the read position unchanged in some branches, which used to spin
    // forever. If the progress guard regresses, this test hangs rather than failing --
    // which is itself the signal.
    for (uint32_t sampleType : {1u, 2u, 3u, 4u, 99u})
    {
        DatagramBuilder b;
        b.header(4).word(sampleType).word(0).words(45);
        EXPECT_NO_THROW(feed(b)) << "failed for sampleType " << sampleType;
    }
}

TEST_F(SFlowParsingFixture, TerminatesWhenFlowDataLengthExceedsSampleLength)
{
    // The dangerous one. Two advancement expressions compute
    //     index += (sampleLen / 4 + 2 - (flowDataLength / 4 + 2));
    // in unsigned arithmetic, so flowDataLength > sampleLen wraps the result to near 2^32
    // and index leaps far outside the buffer.
    //
    // Layout for sampleType 1: word[index+1] = sampleLen, word[index+11] = flowDataLength.
    DatagramBuilder b;
    b.header(2)
        .word(1)        // sampleType: flow sample
        .word(4 * 4)    // sampleLen: deliberately small
        .words(9)       // index+2 .. index+10
        .word(4 * 1000) // index+11: flowDataLength, far larger than sampleLen
        .words(60);
    EXPECT_NO_THROW(feed(b));
}

TEST_F(SFlowParsingFixture, HandlesSampleLengthLargerThanTheDatagram)
{
    DatagramBuilder b;
    b.header(1).word(2).word(0xFFFFFFF0).words(45);
    EXPECT_NO_THROW(feed(b));
}

TEST_F(SFlowParsingFixture, HandlesUnknownSampleTypes)
{
    // Only 1-4 are recognised; the parser must skip the rest by length, not guess.
    for (uint32_t sampleType : {0u, 5u, 6u, 1000u, 0xFFFFFFFFu})
    {
        DatagramBuilder b;
        b.header(1).word(sampleType).word(4 * 10).words(45);
        EXPECT_NO_THROW(feed(b)) << "failed for sampleType " << sampleType;
    }
}

TEST_F(SFlowParsingFixture, HandlesManySamplesWithinOneDatagram)
{
    // Several well-formed-looking samples back to back, to confirm the loop walks them and
    // stops at the end rather than one word past it.
    DatagramBuilder b;
    b.header(5);
    for (int i = 0; i < 5; ++i)
    {
        b.word(2).word(4 * 12).words(10);
    }
    EXPECT_NO_THROW(feed(b));
}

TEST_F(SFlowParsingFixture, HandlesNonIpv4EtherType)
{
    // ARP / IPv6 / LLDP frames take the early-continue path, which also advances the index
    // via the unsigned expression above.
    for (uint32_t etherType : {0x0806u, 0x86DDu, 0x88CCu})
    {
        DatagramBuilder b;
        b.header(1).word(1).word(4 * 30).words(11);
        b.word(4 * 8);              // flowDataLength
        b.word(64);                 // frameLength
        b.words(5);
        b.word(etherType << 16);    // the parser reads the top 16 bits
        b.words(30);
        EXPECT_NO_THROW(feed(b)) << "failed for etherType " << etherType;
    }
}

// =====================================================================================
// Fuzz-ish sweep: structured garbage must never crash or hang
// =====================================================================================

TEST_F(SFlowParsingFixture, SurvivesStructuredGarbage)
{
    // Deterministic pseudo-random payloads with a valid header, so the parser gets past the
    // version check and into the offset arithmetic with arbitrary lengths and types.
    uint32_t seed = 0x12345678;
    auto next = [&seed]() {
        seed = seed * 1664525u + 1013904223u; // numerical recipes LCG
        return seed;
    };

    for (int iteration = 0; iteration < 500; ++iteration)
    {
        DatagramBuilder b;
        b.header(next() % 8);
        const size_t payloadWords = next() % 60;
        for (size_t w = 0; w < payloadWords; ++w)
        {
            b.raw(next());
        }
        EXPECT_NO_THROW(feed(b)) << "failed at iteration " << iteration;
    }
}

TEST_F(SFlowParsingFixture, SurvivesTruncationSweepOverGarbage)
{
    uint32_t seed = 0xDEADBEEF;
    auto next = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return seed;
    };

    for (int iteration = 0; iteration < 100; ++iteration)
    {
        DatagramBuilder b;
        b.header(1 + next() % 4);
        for (size_t w = 0; w < 50; ++w)
        {
            b.raw(next());
        }
        auto complete = b.bytes();
        // Cut at a random 4-byte boundary at or after the header.
        const size_t keepWords = 7 + (next() % (complete.size() / 4 - 7));
        std::vector<char> truncated(complete.begin(),
                                    complete.begin() + static_cast<long>(keepWords * 4));
        EXPECT_NO_THROW(feed(truncated)) << "failed at iteration " << iteration;
    }
}

// =====================================================================================
// Malformed-packet accounting
// =====================================================================================

TEST_F(SFlowParsingFixture, CountsEveryMalformedDatagramEvenThoughLoggingIsRateLimited)
{
    // The log is capped at one per thousand so a flood cannot fill the disk, but the count
    // must stay exact -- otherwise rate limiting would hide how bad the input is.
    const uint64_t before = m_collector->malformedDatagramCount();

    for (int i = 0; i < 50; ++i)
    {
        DatagramBuilder b;
        b.header(1).word(2).word(4 * 40); // promises a counter sample, then ends
        EXPECT_NO_THROW(feed(b));
    }

    EXPECT_EQ(m_collector->malformedDatagramCount(), before + 50);
}

// =====================================================================================
// ICMP type and code extraction, both flow-sample formats
// =====================================================================================
//
// [Co-developed with claude code -- Adam]
//
// The parser carries ICMP type in FlowKey::srcPort and code in FlowKey::dstPort (see
// test_SFlowEmitterRoundtrip.cpp). These tests build one ICMP sample per datagram, with a
// known type and code, in each of the two flow-sample formats the parser accepts, and check
// both fields come back as sent:
//
//  - sFlow v5 flow_sample (format 1), in the shape the Mininet path expects: an extended
//    switch record first, then the raw packet header record, as the bmv2 emitter and OVS
//    send it (compare tests/fixtures/emitted_icmp_unreachable.bin). Untagged Ethernet.
//  - sFlow v5 flow_sample_expanded (format 3), the HPE path: source id, input and output
//    each take two words, so the sampled header bytes start at word 19 of the sample. The HPE
//    branch's fixed offsets (ethertype at +23, IPv4 protocol at +25, L4 at +28) line up with
//    an 802.1Q-tagged frame, so the frame here carries one VLAN tag.
//
// ICMP type and code are a full byte each. In the IANA ICMP registry the only assigned code
// above 15 is type 9 code 16 (router advertisement, "does not route common traffic"), so a
// mask that keeps the low nibble of the code survived almost all real traffic. Type 253 is
// the RFC 3692-style experiment type and has no codes registered; 253 / 0xA5 sets bit 7 of
// both bytes, so a mask narrower than 0xFF on either field fails. The parser does not
// validate type or code values.

namespace
{

/// Ethernet + IPv4 + 8-byte ICMP header, 10.0.0.1 -> 10.0.0.2, optionally 802.1Q-tagged.
std::vector<uint8_t> icmpFrame(uint8_t icmpType, uint8_t icmpCode, bool vlanTagged)
{
    std::vector<uint8_t> f = {0x00, 0x00, 0x00, 0x00, 0x00, 0x02,  // dst MAC
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x01}; // src MAC
    if (vlanTagged)
    {
        f.insert(f.end(), {0x81, 0x00, 0x00, 0x64}); // TPID, TCI (VLAN 100)
    }
    f.insert(f.end(), {0x08, 0x00}); // IPv4
    f.insert(f.end(),
             {0x45, 0x00, 0x00, 0x1c, // version/IHL, TOS, total length 28
              0x00, 0x01, 0x00, 0x00, // id, flags/fragment offset 0
              0x40, 0x01, 0x00, 0x00, // TTL, protocol ICMP, checksum
              10,   0,    0,    1,    // src
              10,   0,    0,    2});  // dst
    f.insert(f.end(), {icmpType, icmpCode, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01});
    return f;
}

size_t paddedWords(size_t byteCount) { return (byteCount + 3) / 4; }

/// One flow_sample (format 1) laid out the way the Mininet-mode parser reads it.
DatagramBuilder standardIcmpDatagram(uint8_t icmpType, uint8_t icmpCode)
{
    const auto frame = icmpFrame(icmpType, icmpCode, false);
    const size_t hdrWords = paddedWords(frame.size());
    DatagramBuilder b;
    b.header(1);
    b.word(1);                              // flow_sample
    b.word(4 * (8 + 6 + 6 + hdrWords));     // sample length
    b.word(1);                              // sequence number
    b.word(0x02000002);                     // source id
    b.word(256);                            // sampling rate
    b.word(256);                            // sample pool
    b.word(0);                              // drops
    b.word(2);                              // input
    b.word(4);                              // output
    b.word(2);                              // flow records
    b.word(1001).word(16).words(4);         // extended switch record
    b.word(1);                              // raw packet header record
    b.word(4 * (4 + hdrWords));             // record length
    b.word(1);                              // header protocol: Ethernet
    b.word(static_cast<uint32_t>(frame.size()) + 4); // frame length (incl. FCS)
    b.word(4);                              // stripped
    b.word(static_cast<uint32_t>(frame.size()));     // header length
    b.octets(frame);
    return b;
}

/// One flow_sample_expanded (format 3), the HPE layout.
DatagramBuilder expandedIcmpDatagram(uint8_t icmpType, uint8_t icmpCode)
{
    const auto frame = icmpFrame(icmpType, icmpCode, true);
    const size_t hdrWords = paddedWords(frame.size());
    DatagramBuilder b;
    b.header(1);
    b.word(3);                              // flow_sample_expanded
    b.word(4 * (11 + 2 + 4 + hdrWords));    // sample length
    b.word(1);                              // sequence number
    b.word(0).word(2);                      // source id type, index
    b.word(256);                            // sampling rate
    b.word(256);                            // sample pool
    b.word(0);                              // drops
    b.word(0).word(2);                      // input format, value
    b.word(0).word(4);                      // output format, value
    b.word(1);                              // flow records
    b.word(1);                              // raw packet header record
    b.word(4 * (4 + hdrWords));             // record length
    b.word(1);                              // header protocol: Ethernet
    b.word(static_cast<uint32_t>(frame.size()) + 4); // frame length (incl. FCS)
    b.word(4);                              // stripped
    b.word(static_cast<uint32_t>(frame.size()));     // header length
    b.octets(frame);
    return b;
}

} // namespace

class SFlowIcmpFieldsTest : public SFlowParsingFixture
{
  protected:
    /// Feeds one datagram to a fresh collector and returns the single ICMP key it produced.
    sflow::FlowKey onlyIcmpKey(const DatagramBuilder& b, utils::DeploymentMode mode)
    {
        resetCollector(mode);
        feed(b);
        const auto table = m_collector->getFlowInfoTable();
        EXPECT_EQ(table.size(), 1u) << "expected exactly one flow from one ICMP sample";
        for (const auto& [key, info] : table)
        {
            EXPECT_EQ(int(key.protocol), 1);
            EXPECT_EQ(key.srcIP, inet_addr("10.0.0.1"));
            EXPECT_EQ(key.dstIP, inet_addr("10.0.0.2"));
            return key;
        }
        ADD_FAILURE() << "no flow extracted";
        return {};
    }
};

TEST_F(SFlowIcmpFieldsTest, StandardSampleKeepsTypeAndLowCode)
{
    // Guards the standard layout's word offset for type and a code below 16.
    const auto key = onlyIcmpKey(standardIcmpDatagram(3, 13), utils::DeploymentMode::MININET);
    EXPECT_EQ(key.srcPort, 3) << "ICMP type";
    EXPECT_EQ(key.dstPort, 13) << "ICMP code";
}

TEST_F(SFlowIcmpFieldsTest, StandardSampleKeepsTheAssignedCodeAbove15)
{
    const auto key = onlyIcmpKey(standardIcmpDatagram(9, 16), utils::DeploymentMode::MININET);
    EXPECT_EQ(key.srcPort, 9) << "ICMP type";
    EXPECT_EQ(key.dstPort, 16) << "ICMP code";
}

TEST_F(SFlowIcmpFieldsTest, StandardSampleKeepsBothFullBytes)
{
    const auto key = onlyIcmpKey(standardIcmpDatagram(253, 0xA5), utils::DeploymentMode::MININET);
    EXPECT_EQ(key.srcPort, 253) << "ICMP type";
    EXPECT_EQ(key.dstPort, 0xA5) << "ICMP code";
}

TEST_F(SFlowIcmpFieldsTest, ExpandedSampleKeepsTypeAndLowCode)
{
    const auto key = onlyIcmpKey(expandedIcmpDatagram(3, 13), utils::DeploymentMode::TESTBED);
    EXPECT_EQ(key.srcPort, 3) << "ICMP type";
    EXPECT_EQ(key.dstPort, 13) << "ICMP code";
}

TEST_F(SFlowIcmpFieldsTest, ExpandedSampleKeepsTheAssignedCodeAbove15)
{
    const auto key = onlyIcmpKey(expandedIcmpDatagram(9, 16), utils::DeploymentMode::TESTBED);
    EXPECT_EQ(key.srcPort, 9) << "ICMP type";
    EXPECT_EQ(key.dstPort, 16) << "ICMP code";
}

TEST_F(SFlowIcmpFieldsTest, ExpandedSampleKeepsBothFullBytes)
{
    const auto key = onlyIcmpKey(expandedIcmpDatagram(253, 0xA5), utils::DeploymentMode::TESTBED);
    EXPECT_EQ(key.srcPort, 253) << "ICMP type";
    EXPECT_EQ(key.dstPort, 0xA5) << "ICMP code";
}

TEST_F(SFlowIcmpFieldsTest, ExpandedSampleKeepsEchoRequest)
{
    // Type 8 code 0, the commonest ICMP sample (ping).
    const auto key = onlyIcmpKey(expandedIcmpDatagram(8, 0), utils::DeploymentMode::TESTBED);
    EXPECT_EQ(key.srcPort, 8) << "ICMP type";
    EXPECT_EQ(key.dstPort, 0) << "ICMP code";
}

// =====================================================================================
// The bounded view itself
// =====================================================================================

TEST(BoundedWordsTest, ReadsInRangeWordsUnchanged)
{
    const uint32_t raw[3] = {0x11111111, 0x22222222, 0x33333333};
    const sflow::BoundedWords view(raw, 3);

    EXPECT_EQ(view[0], 0x11111111u);
    EXPECT_EQ(view[2], 0x33333333u);
    EXPECT_EQ(view.size(), 3u);
}

TEST(BoundedWordsTest, ThrowsPastTheEnd)
{
    const uint32_t raw[2] = {1, 2};
    const sflow::BoundedWords view(raw, 2);

    EXPECT_NO_THROW(view[1]);
    EXPECT_THROW(view[2], sflow::TruncatedDatagram);
    EXPECT_THROW(view[1000000], sflow::TruncatedDatagram);
}

TEST(BoundedWordsTest, ReportsTheOffendingOffset)
{
    const uint32_t raw[2] = {1, 2};
    const sflow::BoundedWords view(raw, 2);

    try
    {
        (void)view[38]; // the deepest offset the counter-sample path reaches
        FAIL() << "expected TruncatedDatagram";
    }
    catch (const sflow::TruncatedDatagram& e)
    {
        EXPECT_EQ(e.requestedWord(), 38u);
        EXPECT_EQ(e.availableWords(), 2u);
    }
}

TEST(BoundedWordsTest, EmptyViewThrowsOnAnyAccess)
{
    const sflow::BoundedWords view(nullptr, 0);
    EXPECT_THROW(view[0], sflow::TruncatedDatagram);
    EXPECT_FALSE(view.has(0));
}
