/**
 * UDP Client
 *
 * Reads input.jpeg from the current directory and sends it to the UDP server
 * in numbered packets, resends any packets the server reports missing, and
 * prints the server's confirmation.
 *
 * Usage: ./udp_client [server_ip] [port]      (defaults: 127.0.0.1 3940)
 *
 * How the transfer works:
 *   1. The image is split into CHUNK_SIZE (1024) byte chunks. Each chunk is
 *      sent as one UDP packet with an 8-byte header in front of it:
 *        | packet number (4 bytes) | total image size (4 bytes) | image data |
 *      Both header numbers are sent in network byte order (big-endian).
 *   2. After every packet has been sent once, the client listens for replies.
 *      The two kinds of reply are told apart by their length:
 *        - 4 bytes:  the number of a packet the server is missing, so the
 *                    client sends that packet again (packet loss recovery)
 *        - longer:   the server's confirmation message, which is printed
 *   3. If nothing arrives for 1 second, the client resends its last packet to
 *      prompt the server (this recovers a lost confirmation). After
 *      MAX_TIMEOUTS silent seconds in a row it gives up.
 */
#include <sys/types.h>   // basic system types (ssize_t, socklen_t)
#include <sys/socket.h>  // socket(), sendto(), recvfrom(), setsockopt()
#include <iostream>      // std::cout, std::cerr
#include <cstring>       // memset(), memcpy(), strerror()
#include <netdb.h>       // getaddrinfo(), freeaddrinfo(), gai_strerror()
#include <netinet/in.h>  // internet address structures (sockaddr_in, sockaddr_in6)
#include <arpa/inet.h>   // htonl()/ntohl(): host <-> network byte order
#include <unistd.h>      // close()
#include <fstream>       // std::ifstream, reads the image from disk
#include <vector>        // std::vector: the image bytes and each packet's buffer
#include <cerrno>        // errno: the reason the last system call failed (e.g. EAGAIN)
#include <sys/time.h>    // struct timeval, for the receive timeout
#include <cstdint>       // uint32_t, the fixed-size 4-byte header fields
#include <algorithm>     // std::min

#define PORT "3940"        // default server port (must match the server)
#define IP "127.0.0.1"     // default server address (this machine)

// Packet format settings. HEADER_SIZE and CHUNK_SIZE must match udp_server.cpp.
constexpr size_t HEADER_SIZE = 8;     // 4-byte packet number + 4-byte total image size
constexpr size_t CHUNK_SIZE = 1024;   // image bytes carried by each packet
constexpr int MAX_TIMEOUTS = 5;       // give up after this many 1-second silences in a row

// Builds packet number `seq` (header + its slice of the image) and sends it to the server.
bool sendPacket(int, const std::vector<char>&, uint32_t, const sockaddr*, socklen_t);

int main(int argc, char* argv[]) {
    // ---------- Command-line arguments: [server_ip] [port] ----------
    const char* port = nullptr;
    const char* ip = nullptr;
    argc > 1 ? ip = argv[1] : ip = IP;
    argc > 2 ? port = argv[2] : port = PORT;

    int status;           // return value of getaddrinfo
    addrinfo hints, *res; // hints = what we're looking for, res = the results
    int sockfd;           // the client's UDP socket

    // ---------- Look up the server's address ----------
    // AF_UNSPEC: accept an IPv4 or IPv6 address, whichever the given IP is.
    // SOCK_DGRAM: we want a UDP (datagram) socket.
    memset(&hints, 0, sizeof(hints)); // start from all zeros, not leftover garbage
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    // res becomes a list of matching addresses; we use the first one
    status = getaddrinfo(ip, port, &hints, &res);
    if (status != 0) {
        std::cerr << "Error loading address info: " << gai_strerror(status) << "\n";
        return 1;
    }

    // ---------- Create the UDP socket ----------
    // UDP needs no connect() or bind() here: the OS picks a local port on the
    // first sendto(), and every sendto() names the destination itself.
    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

    if (sockfd == -1) {
        std::cerr << "Error creating socket\n";
        freeaddrinfo(res);
        return 1;
    }

    // ---------- Receive timeout ----------
    // Without this, recvfrom() would wait forever if the server is down or a
    // reply is lost. With it, recvfrom() returns -1 (errno EAGAIN) after 1 s.
    // It must be longer than the server's 500 ms timeout, so the server gets
    // the chance to ask for missing packets before we prompt it.
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1) {
        std::cerr << "Error setting receive timeout: " << strerror(errno) << "\n";
        freeaddrinfo(res);
        close(sockfd);
        return 1;
    }

    // ---------- Read the whole image file into memory ----------
    // Binary mode, so every byte is read exactly as it is on disk.
    std::ifstream image("input.jpeg", std::ios::binary);
    if (!image) {
        std::cerr << "Could not open image\n";
        return 1;
    }
    // Find the file size: jump to the end, read the position, jump back.
    image.seekg(0, std::ios::end);
    std::streamsize size = image.tellg();
    image.seekg(0, std::ios::beg);

    // tellg() returns -1 on error, and an empty file has nothing to send
    if (size <= 0) {
        std::cerr << "input.jpeg is empty or unreadable\n";
        return 1;
    }

    std::vector<char> bytes(size); // buffer big enough for the whole file

    if (!image.read(bytes.data(), size)) {
        std::cerr << "Failed to read input.jpeg\n";
        return 1;
    }

    std::cout << "Image read into buffer successfully\n";

    // ---------- Work out how many packets are needed ----------
    // Rounding up means a final partial chunk still gets its own packet.
    uint32_t total = static_cast<uint32_t>(bytes.size());
    uint32_t numPackets = (total + CHUNK_SIZE - 1) / CHUNK_SIZE; // divide (rounding up)
    std::cout << "Sending " << total << " bytes in " << numPackets << " packets\n";

    // ---------- Step 1: send every packet once ----------
    // Packets go out back to back. Some may be lost on the way (or dropped by
    // the server if its receive buffer fills up); step 2 recovers those.
    for (uint32_t seq = 0; seq < numPackets; seq++) {
        if (!sendPacket(sockfd, bytes, seq, res->ai_addr, res->ai_addrlen)) {
            std::cerr << "Error sending packet " << seq + 1 << ": " << strerror(errno) << "\n";
            freeaddrinfo(res);
            close(sockfd);
            return 1;
        }
    }
    std::cout << "All packets sent, waiting for the server\n";

    // ---------- Step 2: resend whatever the server asks for, until it confirms ----------
    char reply[256];           // a resend request (4 bytes) or the confirmation text
    int timeouts = 0;         // 1-second silences in a row
    bool confirmed = false;
    while (!confirmed && timeouts < MAX_TIMEOUTS) {
        // sizeof(reply) - 1 leaves room for the '\0' added below.
        // We don't need the sender's address, so the last two arguments are nullptr.
        ssize_t n = recvfrom(sockfd, reply, sizeof(reply) - 1, 0, nullptr, nullptr);

        // -1 means either the 1 s timeout expired (EAGAIN) or a real error
        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 1 second with no reply. Resend the last packet to prompt the server:
                //  - if the server already saved the image but its confirmation was
                //    lost, it treats this packet as a new transfer, asks for the
                //    rest, saves the image again and sends a new confirmation
                //  - otherwise the server ignores it as a duplicate (or it fills
                //    the gap, if the last packet was the one that got lost)
                timeouts++;
                sendPacket(sockfd, bytes, numPackets - 1, res->ai_addr, res->ai_addrlen);
                continue;
            }
            std::cerr << "Error receiving from server: " << strerror(errno) << "\n";
            break;
        }
        timeouts = 0; // got a reply, so the server is alive: reset the silence counter

        if (n == 4) {
            // 4 bytes = the number of a packet the server is missing
            uint32_t seq;
            memcpy(&seq, reply, 4);
            seq = ntohl(seq);   // network byte order -> this machine's byte order
            if (seq < numPackets) {   // ignore numbers outside the image (corrupt/stray reply)
                std::cout << "Server is missing packet " << seq + 1 << ", resending\n";
                sendPacket(sockfd, bytes, seq, res->ai_addr, res->ai_addrlen);
            }
        } else if (n > 4) {
            // anything longer is the confirmation text
            reply[n] = '\0';    // end the string so it can be printed
            std::cout << "Server replied with: " << reply << "\n";
            confirmed = true;
        }
    }

    // the loop ended without a confirmation: server down, unreachable, or gave up
    if (!confirmed) {
        std::cerr << "No confirmation from the server (is it running?)\n";
    }

    // ---------- Clean up ----------
    // Exit code 0 = the server confirmed the image, 1 = it never did.
    freeaddrinfo(res);
    close(sockfd);
    return confirmed ? 0 : 1;
}

/**
 * Sends packet number `seq` of the image to the server.
 * Packet layout: | seq (4 bytes) | total image size (4 bytes) | image bytes |
 * Every packet carries CHUNK_SIZE bytes of the image except the last one,
 * which carries whatever is left over.
 * Returns false if the OS refused to send it.
 */
bool sendPacket(int fd, const std::vector<char>& image, uint32_t seq, const sockaddr* to, socklen_t toLen) {
    uint32_t total = static_cast<uint32_t>(image.size());

    // where this packet's slice starts in the image, and how long it is
    size_t offset = static_cast<size_t>(seq) * CHUNK_SIZE;
    size_t dataLen = std::min<size_t>(CHUNK_SIZE, total - offset); // last packet may be shorter

    std::vector<char> packet(HEADER_SIZE + dataLen); // temp buffer for this one packet

    // Header numbers go out in network byte order (big-endian), so the
    // receiver reads the same values whatever kind of machine it runs on.
    uint32_t netSeq = htonl(seq);
    uint32_t netTotal = htonl(total);

    // memcpy places the bytes exactly where we want them (a struct could add padding)
    memcpy(packet.data(), &netSeq, 4);                                    // bytes 0-3: packet number
    memcpy(packet.data() + 4, &netTotal, 4);                              // bytes 4-7: total image size
    memcpy(packet.data() + HEADER_SIZE, image.data() + offset, dataLen);  // bytes 8+: image data

    // one sendto() call = one UDP datagram
    return sendto(fd, packet.data(), packet.size(), 0, to, toLen) != -1;
}
