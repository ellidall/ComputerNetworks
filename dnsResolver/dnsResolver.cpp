#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <algorithm>
#include <optional>
#include <unordered_map>

constexpr uint16_t DNS_PORT = 53;
constexpr int MAX_DNS_PACKET_SIZE = 512;
constexpr int TIMEOUT_SECONDS = 5;
constexpr int MAX_HOPS = 5;
constexpr int MAX_DEPTH = 3;

enum class RecordType
{
    A       = 1,
    AAAA    = 28,
    NS      = 2,
    UNKNOWN = 0
};

struct DnsResourceRecord
{
    std::string name;
    RecordType type;
    uint32_t ttl;
    std::vector<uint8_t> rdata;
};

static bool g_debug = false;

void DebugPrint(const std::string& msg)
{
    if (g_debug)
    {
        std::cerr << "[DEBUG] " << msg << std::endl;
    }
}

RecordType ParseRecordType(const std::string& typeStr)
{
    if (typeStr == "A") return RecordType::A;
    if (typeStr == "AAAA") return RecordType::AAAA;
    return RecordType::UNKNOWN;
}

std::vector<uint8_t> BuildDnsQuery(const std::string& domain, RecordType type)
{
    std::vector<uint8_t> packet(12, 0);

    packet[0] = 0x12;
    packet[1] = 0x34;

    packet[2] = 0x00;
    packet[3] = 0x00;

    packet[4] = 0x00;
    packet[5] = 0x01;

    packet[6] = packet[7] = 0;
    packet[8] = packet[9] = 0;
    packet[10] = packet[11] = 0;

    size_t start = 0;
    for (size_t i = 0; i <= domain.size(); ++i)
    {
        if (i == domain.size() || domain[i] == '.')
        {
            uint8_t labelLen = static_cast<uint8_t>(i - start);
            packet.push_back(labelLen);
            for (size_t j = start; j < i; ++j)
            {
                packet.push_back(static_cast<uint8_t>(domain[j]));
            }
            start = i + 1;
        }
    }
    packet.push_back(0x00);

    uint16_t qtype = static_cast<uint16_t>(type);
    uint16_t qclass = 1;

    packet.push_back(static_cast<uint8_t>(qtype >> 8));
    packet.push_back(static_cast<uint8_t>(qtype & 0xFF));
    packet.push_back(static_cast<uint8_t>(qclass >> 8));
    packet.push_back(static_cast<uint8_t>(qclass & 0xFF));

    return packet;
}

std::string DecodeDnsName(const std::vector<uint8_t>& packet, size_t& offset)
{
    std::string name;
    const size_t maxOffset = packet.size();
    int jumps = 0;

    while (offset < maxOffset && jumps < 10)
    {
        if (const uint8_t len = packet[offset]; (len & 0xC0) == 0xC0)
        {
            if (offset + 1 >= maxOffset) break;
            const uint16_t ptr = ((len & 0x3F) << 8) | packet[offset + 1];
            offset += 2;
            const size_t savedOffset = offset;
            offset = ptr;
            std::string ref = DecodeDnsName(packet, offset);
            offset = savedOffset;
            if (!name.empty()) name += ".";
            name += ref;
            break;
        }
        else if (len == 0)
        {
            offset++;
            break;
        }
        else
        {
            offset++;
            if (offset + len > maxOffset) break;
            if (!name.empty()) name += ".";
            for (uint8_t i = 0; i < len; ++i)
            {
                name += static_cast<char>(packet[offset + i]);
            }
            offset += len;
        }
        jumps++;
    }
    return name;
}

std::optional<std::vector<DnsResourceRecord>> ParseDnsResponse(const std::vector<uint8_t>& packet,
                                                               const RecordType requestedType)
{
    if (packet.size() < 12) return std::nullopt;

    if (packet[0] != 0x12 || packet[1] != 0x34)
    {
        DebugPrint("Invalid transaction ID");
        return std::nullopt;
    }

    if ((packet[2] & 0x80) == 0)
    {
        DebugPrint("Not a response");
        return std::nullopt;
    }

    if (uint8_t rcode = packet[3] & 0x0F; rcode != 0)
    {
        if (rcode == 3)
        {
            DebugPrint("Domain does not exist (NXDOMAIN)");
        }
        else
        {
            DebugPrint("DNS error, RCODE: " + std::to_string(rcode));
        }
        return std::nullopt;
    }

    const uint16_t qdCount = (packet[4] << 8) | packet[5];
    const uint16_t anCount = (packet[6] << 8) | packet[7];
    size_t offset = 12;

    for (uint16_t i = 0; i < qdCount; ++i)
    {
        while (offset < packet.size() && packet[offset] != 0)
        {
            const uint8_t len = packet[offset];
            if ((len & 0xC0) == 0xC0)
            {
                offset += 2;
                break;
            }
            offset += 1 + len;
        }
        offset += 1;
        offset += 4;
    }

    std::vector<DnsResourceRecord> records;
    for (uint16_t i = 0; i < anCount; ++i)
    {
        const std::string name = DecodeDnsName(packet, offset);
        if (offset + 10 > packet.size()) break;

        const uint16_t type = (packet[offset] << 8) | packet[offset + 1];
        const uint32_t ttl = (packet[offset + 4] << 24) | (packet[offset + 5] << 16) | (packet[offset + 6] << 8) | (
                                 packet[offset + 7]);
        const uint16_t rdLen = (packet[offset + 8] << 8) | packet[offset + 9];
        offset += 10;

        if (offset + rdLen > packet.size()) break;

        const std::vector rdata(packet.begin() + offset, packet.begin() + offset + rdLen);
        offset += rdLen;

        RecordType recType = (type == 1) ? RecordType::A : (type == 28) ? RecordType::AAAA : RecordType::UNKNOWN;

        if (recType == requestedType)
        {
            DnsResourceRecord rr;
            rr.name = name;
            rr.type = recType;
            rr.ttl = ttl;
            rr.rdata = rdata;
            records.push_back(rr);
        }
    }

    return records;
}

std::vector<std::string> ExtractNameservers(const std::vector<uint8_t>& packet)
{
    if (packet.size() < 12) return {};

    const uint16_t qdCount = (packet[4] << 8) | packet[5];
    const uint16_t anCount = (packet[6] << 8) | packet[7];
    const uint16_t nsCount = (packet[8] << 8) | packet[9];

    size_t offset = 12;

    for (uint16_t i = 0; i < qdCount; ++i)
    {
        while (offset < packet.size() && packet[offset] != 0)
        {
            const uint8_t len = packet[offset];
            if ((len & 0xC0) == 0xC0)
            {
                offset += 2;
                break;
            }
            offset += 1 + len;
        }
        offset += 1;
        offset += 4;
    }

    for (uint16_t i = 0; i < anCount; ++i)
    {
        std::string name = DecodeDnsName(packet, offset);
        if (offset + 10 > packet.size()) break;
        const uint16_t rdLen = (packet[offset + 8] << 8) | packet[offset + 9];
        offset += 10 + rdLen;
    }

    std::vector<std::string> nameservers;
    for (uint16_t i = 0; i < nsCount; ++i)
    {
        std::string zoneName = DecodeDnsName(packet, offset);
        if (offset + 10 > packet.size()) break;

        const uint16_t type = (packet[offset] << 8) | packet[offset + 1];
        const uint16_t rdLen = (packet[offset + 8] << 8) | packet[offset + 9];
        offset += 10;

        if (type == 2)
        {
            size_t rdataOffset = offset;
            std::string nsName = DecodeDnsName(packet, rdataOffset);
            if (!nsName.empty())
            {
                nameservers.push_back(nsName);
            }
        }

        offset += rdLen;
    }

    return nameservers;
}

std::unordered_map<std::string, std::string> ExtractGlueRecords(const std::vector<uint8_t>& packet)
{
    if (packet.size() < 12) return {};

    const uint16_t qdCount = (packet[4] << 8) | packet[5];
    const uint16_t anCount = (packet[6] << 8) | packet[7];
    const uint16_t nsCount = (packet[8] << 8) | packet[9];
    const uint16_t arCount = (packet[10] << 8) | packet[11];

    size_t offset = 12;

    for (uint16_t i = 0; i < qdCount; ++i)
    {
        while (offset < packet.size() && packet[offset] != 0)
        {
            const uint8_t len = packet[offset];
            if ((len & 0xC0) == 0xC0)
            {
                offset += 2;
                break;
            }
            offset += 1 + len;
        }
        offset += 1 + 4;
    }

    for (uint16_t i = 0; i < anCount; ++i)
    {
        std::string name = DecodeDnsName(packet, offset);
        if (offset + 10 > packet.size()) break;
        uint16_t rdLen = (packet[offset + 8] << 8) | packet[offset + 9];
        offset += 10 + rdLen;
    }

    for (uint16_t i = 0; i < nsCount; ++i)
    {
        std::string name = DecodeDnsName(packet, offset);
        if (offset + 10 > packet.size()) break;
        uint16_t rdLen = (packet[offset + 8] << 8) | packet[offset + 9];
        offset += 10 + rdLen;
    }

    std::unordered_map<std::string, std::string> glue;
    for (uint16_t i = 0; i < arCount; ++i)
    {
        size_t startOffset = offset;
        std::string name = DecodeDnsName(packet, offset);
        if (offset + 10 > packet.size()) break;

        uint16_t type = (packet[offset] << 8) | packet[offset + 1];
        uint16_t rdLen = (packet[offset + 8] << 8) | packet[offset + 9];
        offset += 10;

        if (type == 1 && rdLen == 4)
        {
            in_addr addr;
            memcpy(&addr, packet.data() + offset, 4);
            glue[name] = std::string(inet_ntoa(addr));
        }

        offset += rdLen;
    }

    return glue;
}

std::vector<std::string> GetRootServers()
{
    return {
        "198.41.0.4", "199.9.14.201", "192.33.4.12", "199.7.91.13", "192.203.230.10", "192.5.5.241", "192.112.36.4",
        "198.97.190.53", "192.36.148.17", "192.58.128.30", "193.0.14.129", "199.7.83.42", "202.12.27.33"
    };
}

std::vector<uint8_t> SendDnsQuery(const std::string& serverIp, const std::vector<uint8_t>& query)
{
    const int sockFD = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockFD < 0)
    {
        DebugPrint("Failed to create socket");
        return {};
    }

    timeval timeout;
    timeout.tv_sec = TIMEOUT_SECONDS;
    timeout.tv_usec = 0;
    setsockopt(sockFD, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(DNS_PORT);
    if (inet_pton(AF_INET, serverIp.c_str(), &serverAddr.sin_addr) <= 0)
    {
        close(sockFD);
        DebugPrint("Invalid IP address: " + serverIp);
        return {};
    }

    if (sendto(sockFD, query.data(), query.size(), 0, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) !=
        static_cast<ssize_t>(query.size()))
    {
        close(sockFD);
        DebugPrint("Failed to send query to " + serverIp);
        return {};
    }

    std::vector<uint8_t> response(MAX_DNS_PACKET_SIZE);
    const ssize_t bytesReceived = recvfrom(sockFD, response.data(), response.size(), 0, nullptr, nullptr);
    close(sockFD);

    if (bytesReceived <= 0)
    {
        DebugPrint("No response from " + serverIp);
        return {};
    }

    response.resize(bytesReceived);
    return response;
}

std::optional<std::string> PerformIterativeLookup(const std::string& hostname, RecordType type, int depth);

std::optional<std::string> ResolveNsAddress(const std::string& nsName, int depth)
{
    if (depth <= 0)
    {
        DebugPrint("Max depth reached resolving NS: " + nsName);
        return std::nullopt;
    }
    return PerformIterativeLookup(nsName, RecordType::A, depth - 1);
}

std::optional<std::string> PerformIterativeLookup(const std::string& hostname, RecordType type, int depth)
{
    if (depth <= 0)
    {
        DebugPrint("Max depth reached for " + hostname);
        return std::nullopt;
    }

    std::vector<std::string> currentServers = GetRootServers();
    int hops = 0;

    while (hops < MAX_HOPS)
    {
        bool foundAnswer = false;
        bool foundReferral = false;
        std::vector<std::string> nextServers;

        for (const auto& server : currentServers)
        {
            DebugPrint("Querying " + server + " for " + hostname + " (" + (type == RecordType::A ? "A" : "AAAA") + ")");

            auto query = BuildDnsQuery(hostname, type);
            auto response = SendDnsQuery(server, query);
            if (response.empty()) continue;

            if (auto records = ParseDnsResponse(response, type); records.has_value() && !records->empty())
            {
                for (const auto& rec : *records)
                {
                    if (type == RecordType::A && rec.rdata.size() == 4)
                    {
                        in_addr addr;
                        memcpy(&addr, rec.rdata.data(), 4);
                        std::string ip = inet_ntoa(addr);
                        DebugPrint("Final answer: " + ip);
                        return ip;
                    }
                    else if (type == RecordType::AAAA && rec.rdata.size() == 16)
                    {
                        char ipStr[INET6_ADDRSTRLEN];
                        inet_ntop(AF_INET6, rec.rdata.data(), ipStr, INET6_ADDRSTRLEN);
                        std::string ip = ipStr;
                        DebugPrint("Final answer: " + ip);
                        return ip;
                    }
                }
                foundAnswer = true;
            }

            if (!foundAnswer)
            {
                auto nsNames = ExtractNameservers(response);
                auto glue = ExtractGlueRecords(response);

                for (const auto& nsName : nsNames)
                {
                    if (glue.count(nsName))
                    {
                        DebugPrint("Using glue for " + nsName + ": " + glue[nsName]);
                        nextServers.push_back(glue[nsName]);
                    }
                    else
                    {
                        DebugPrint("Resolving NS: " + nsName);
                        auto nsIp = ResolveNsAddress(nsName, depth - 1);
                        if (nsIp.has_value())
                        {
                            nextServers.push_back(*nsIp);
                        }
                    }
                    if (nextServers.size() >= 3) break;
                }

                if (!nextServers.empty())
                {
                    foundReferral = true;
                    currentServers = nextServers;
                    hops++;
                    break;
                }
            }
        }

        if (foundAnswer || !foundReferral)
        {
            break;
        }
    }

    return std::nullopt;
}

int main(const int argc, char* argv[])
{
    if (argc < 3)
    {
        std::cerr << "Usage: " << argv[0] << " <domain> <type> [-d]" << std::endl;
        std::cerr << "Type: A, AAAA" << std::endl;
        return EXIT_FAILURE;
    }

    const std::string domain = argv[1];
    const std::string typeStr = argv[2];
    g_debug = (argc > 3 && std::string(argv[3]) == "-d");

    const RecordType type = ParseRecordType(typeStr);
    if (type == RecordType::UNKNOWN)
    {
        std::cerr << "Unsupported record type: " << typeStr << std::endl;
        return EXIT_FAILURE;
    }

    std::optional<std::string> result = PerformIterativeLookup(domain, type, MAX_DEPTH);

    if (result.has_value())
    {
        std::cout << *result << std::endl;
        return EXIT_SUCCESS;
    }
    if (!g_debug)
    {
        std::cerr << "Host not found" << std::endl;
    }
    return EXIT_FAILURE;
}
