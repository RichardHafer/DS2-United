#include <winsock2.h>
#include <cassert>
#include <cstdio>
#include "../src/coop_peer_readiness.h"
#pragma comment(lib, "ws2_32.lib")

void Loopback() {
    WSADATA data{};
    assert(WSAStartup(MAKEWORD(2, 2), &data) == 0);
    SOCKET sockets[2]{};
    sockaddr_in addresses[2]{};
    coop_peer::State peers[2];
    for (int i = 0; i < 2; ++i) {
        sockets[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        assert(sockets[i] != INVALID_SOCKET);
        addresses[i].sin_family = AF_INET;
        addresses[i].sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(bind(sockets[i], reinterpret_cast<sockaddr*>(&addresses[i]), sizeof(addresses[i])) == 0);
        int length = sizeof(addresses[i]);
        assert(getsockname(sockets[i], reinterpret_cast<sockaddr*>(&addresses[i]), &length) == 0);
        DWORD timeout = 200;
        assert(setsockopt(sockets[i], SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout)) == 0);
        peers[i].token = 10 + i;
        peers[i].UpdateLocal(true, 100 + i);
    }
    for (int turn = 0; turn < 4; ++turn) {
        int sender = turn % 2, receiver = 1 - sender;
        auto packet = peers[sender].MakePacket();
        assert(sendto(sockets[sender], reinterpret_cast<const char*>(&packet), sizeof(packet), 0,
                      reinterpret_cast<sockaddr*>(&addresses[receiver]), sizeof(addresses[receiver])) == sizeof(packet));
        coop_peer::Packet received{};
        assert(recv(sockets[receiver], reinterpret_cast<char*>(&received), sizeof(received), 0) == sizeof(received));
        assert(peers[receiver].Accept(received, 100 + turn));
    }
    assert(peers[0].Ready(104) && peers[1].Ready(104));
    for (auto socket : sockets) closesocket(socket);
    WSACleanup();
}

int main() {
    coop_peer::State host, client;
    host.token = 1; client.token = 2;
    host.UpdateLocal(true, 0x1234); client.UpdateLocal(true, 0x5678);
    assert(!host.Ready(100) && !client.Ready(100));
    auto h = host.MakePacket();
    assert(client.Accept(h, 100));
    auto c = client.MakePacket();
    assert(host.Accept(c, 100) && host.Ready(100));
    assert(!client.Ready(100));
    assert(client.Accept(host.MakePacket(), 101) && client.Ready(101));
    assert(!host.Accept(c, 102));
    assert(!host.Ready(3101));
    assert(host.Accept(client.MakePacket(), 3102) && host.Ready(3102));

    // A new body requires acknowledgement of its new generation.
    host.UpdateLocal(true, 0x9000);
    assert(!host.Ready(3102));
    assert(client.Accept(host.MakePacket(), 3103));
    assert(host.Accept(client.MakePacket(), 3104) && host.Ready(3104));
    host.UpdateLocal(false, 0);
    assert(!host.Ready(3104));
    assert(client.Accept(host.MakePacket(), 3105) && !client.Ready(3105));
    host.UpdateLocal(true, 0x9100);
    assert(!host.Ready(3105));

    // A different process token cannot replace a peer with a fresh heartbeat.
    auto replacement = client.MakePacket();
    replacement.token = 3;
    assert(!host.Accept(replacement, 3200));
    assert(host.Accept(replacement, 6200));
    replacement.version = 99;
    assert(!host.Accept(replacement, 6201));
    replacement.version = 1; replacement.token = host.token;
    assert(!host.Accept(replacement, 6201));

    // Packet and local-clock wraparound remain ordered by unsigned deltas.
    coop_peer::State wrap;
    wrap.token = 4; wrap.UpdateLocal(true, 123);
    coop_peer::Packet packet{coop_peer::kMagic, 1, 5, 1, wrap.token, wrap.epoch, 1, 0xFFFFFFFE};
    assert(wrap.Accept(packet, 0xFFFFFFF0));
    packet.sequence = 1;
    assert(wrap.Accept(packet, 10) && wrap.Ready(20));
    packet.sequence = 0xFFFFFFFD;
    assert(!wrap.Accept(packet, 21));
    packet.sequence = 2; packet.flags = 2;
    assert(!wrap.Accept(packet, 22));
    Loopback();
    std::puts("Peer readiness handshake, lifecycle, timeout, replay and UDP loopback tests passed.");
}
